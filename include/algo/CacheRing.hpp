#pragma once
#include <cell/PlainCell.hpp>
#include <mem/Handle.hpp>
#include <meta/OptionsPack.hpp>
#include <util/bit.hpp>
#include <util/specs.hpp>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>

namespace algo {

struct CacheRingOpt {
    /// Pack the cells. Off by default: producers and consumers chase each other around the
    /// ring, so adjacent cells are genuinely contended.
    struct no_cell_padding {};
};

/**
 * @brief A bounded MPMC ring of small indices, one word per cell.
 *
 * The same trick as algo::VyukovNoABA, applied to a payload that makes it cheaper. Vyukov's
 * ring normally needs a sequence word beside every cell; VyukovNoABA folds the lap number
 * into the *empty* cell instead, which works because a valid pointer cannot look like a lap
 * marker. Here the payload is a small index rather than a pointer, so there is room to fold
 * the lap into **every** cell, empty or not:
 *
 * ```
 *  63                    index_bits                    0
 * +--------------------------+--------------------------+
 * |           lap            |     index, or kEmpty     |
 * +--------------------------+--------------------------+
 * ```
 *
 * That is exactly the split `mem::VersionedIndex` already performs, so the encoding is reused
 * rather than rewritten -- `index_bits` is sized to the value range and the lap takes the
 * remaining fifty-odd bits.
 *
 * The consequence is the point: enqueue and dequeue are a **single-word** compare-exchange,
 * and they are ABA-safe without a double-width CAS, because the lap in the word changes on
 * every pass. A plain `{value}` cell would need a counted pointer or a DWCAS to say "still the
 * empty I read" apart from "empty again, one lap later".
 *
 * @note `mem::VersionedIndex` is used here purely as a bit-packer. Its "version 0 means the
 *       null handle" convention does not apply: lap 0 is an ordinary state, and a cell is
 *       never compared against nil.
 *
 * @note Intended as the reuse cache in front of a slower structure -- somewhere to park
 *       indices that were just released so the next request does not go all the way to the
 *       shared path. It is a general bounded ring; nothing below assumes that use.
 *
 * @tparam Capacity slots, fixed at compile time and rounded up to a power of two, so the wrap
 *                  is a mask and the lap is a shift -- both constants the compiler folds.
 *                  Values must be < Capacity.
 */
template <std::size_t Capacity, typename Opt = meta::EmptyOptions>
    requires meta::AcceptsOnly<Opt, typename CacheRingOpt::no_cell_padding>
class CacheRing {
    static constexpr bool pad_cells = !Opt::template has<typename CacheRingOpt::no_cell_padding>;

    static constexpr std::size_t kSize =
        bit::round_to_next_pow2(Capacity < 2 ? std::size_t{2} : Capacity);
    static constexpr std::size_t kMask = kSize - 1;
    static constexpr std::size_t kShift = bit::log2(kSize);

    /// One more than the value range, so the sentinel has a slot of its own in the encoding.
    using Word = mem::VersionedIndex<Capacity + 1>;
    using cell_type = cell::PlainCell<uint64_t, pad_cells>;

public:
    using value_type = std::size_t;

    /// Reserved: marks an empty cell. Values must therefore be < Capacity.
    static constexpr value_type kEmpty = Capacity;

    static_assert(Capacity >= 1, "CacheRing: capacity must be non-zero");
    static_assert(std::atomic<uint64_t>::is_always_lock_free,
                  "CacheRing: 64-bit atomics must be lock-free");

    CacheRing() noexcept {
        // Every cell starts empty on lap 0; ticket i sits on lap i / kSize, which is 0 for
        // every initial slot.
        for (std::size_t i = 0; i < kSize; ++i)
            cells_[i].val.store(empty_word(0), std::memory_order_relaxed);
    }

    CacheRing(const CacheRing&) = delete;
    CacheRing& operator=(const CacheRing&) = delete;

    static constexpr std::size_t capacity() noexcept { return kSize; }

    /// @return false when the ring is full.
    bool enqueue(value_type item) noexcept {
        assert(item != kEmpty && "CacheRing: kEmpty is reserved");
        assert(item < Capacity && "CacheRing: value out of range");

        for (;;) {
            const uint64_t t = tail_.load(std::memory_order_relaxed);
            const uint64_t h = head_.load(std::memory_order_acquire);
            if (t != tail_.load(std::memory_order_acquire)) continue;
            if (t == h + kSize) return false; // full

            uint64_t expect = empty_word(t >> kShift);
            const bool won = cells_[t & kMask].val.compare_exchange_weak(
                expect, value_word(t >> kShift, item), std::memory_order_acq_rel,
                std::memory_order_relaxed);

            // Help the tail along whether or not we won: a losing thread still knows this
            // ticket is spoken for.
            uint64_t tt = t;
            (void)tail_.compare_exchange_weak(tt, t + 1, std::memory_order_relaxed);
            if (won) return true;
        }
    }

    /// @return false when the ring is empty.
    bool dequeue(value_type& out) noexcept {
        for (;;) {
            const uint64_t t = tail_.load(std::memory_order_relaxed);
            uint64_t h = head_.load(std::memory_order_relaxed);
            cell_type& c = cells_[h & kMask];
            uint64_t word = c.val.load(std::memory_order_acquire);
            if (h != head_.load(std::memory_order_acquire)) continue;
            if (t == h) return false; // empty

            const uint64_t next = empty_word((h >> kShift) + 1);
            if (word_value(word) == kEmpty) { // already taken; help the head along
                if (word == next) {
                    uint64_t hh = h;
                    (void)head_.compare_exchange_weak(hh, h + 1, std::memory_order_relaxed);
                }
                continue;
            }

            const bool won = c.val.compare_exchange_weak(word, next, std::memory_order_acq_rel,
                                                         std::memory_order_relaxed);
            uint64_t hh = h;
            (void)head_.compare_exchange_weak(hh, h + 1, std::memory_order_relaxed);
            if (won) {
                out = word_value(word);
                return true;
            }
        }
    }

    std::size_t size() const noexcept {
        const uint64_t t = tail_.load(std::memory_order_acquire);
        const uint64_t h = head_.load(std::memory_order_acquire);
        return t < h ? 0 : static_cast<std::size_t>(t - h);
    }

    bool empty() const noexcept { return size() == 0; }

private:
    /// Decode without building a Word: VersionedIndex has no raw constructor, and the mask
    /// is the only part of the encoding this needs.
    static value_type word_value(uint64_t w) noexcept {
        return static_cast<value_type>(w & Word::index_mask);
    }

    static uint64_t empty_word(uint64_t lap) noexcept {
        return Word{static_cast<typename Word::version_type>(lap),
                    static_cast<typename Word::index_type>(kEmpty)}
            .raw;
    }
    static uint64_t value_word(uint64_t lap, value_type v) noexcept {
        return Word{static_cast<typename Word::version_type>(lap),
                    static_cast<typename Word::index_type>(v)}
            .raw;
    }

    cell_type cells_[kSize];
    ALIGNED_CACHE std::atomic<uint64_t> tail_{0};
    CACHE_PAD_TYPES(std::atomic<uint64_t>);
    ALIGNED_CACHE std::atomic<uint64_t> head_{0};
    CACHE_PAD_TYPES(std::atomic<uint64_t>);
};

} // namespace algo
