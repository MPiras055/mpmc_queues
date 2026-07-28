#pragma once
#include <cell/PlainCell.hpp>
#include <core/SegmentTraits.hpp>
#include <linkage/Linkage.hpp>
#include <mem/SingleBlock.hpp>
#include <meta/OptionsPack.hpp>
#include <util/bit.hpp>
#include <util/specs.hpp>
#include <atomic>
#include <cassert>

namespace algo {

struct VyukovNoABAOpt {
    struct no_cell_padding {};
};

/**
 * @brief Vyukov's ring with the lap number folded into the empty cell itself.
 *
 * A benchmark comparator, not a segment.
 *
 * The plain Vyukov ring needs a separate sequence word per cell. This variant exploits
 * the fact that a valid user-space pointer has both its top and bottom bits clear: a
 * word with bit 63 *and* bit 0 set can never be a payload, so an empty cell can carry
 * its lap number in the middle 62 bits and the cell collapses to a single word.
 *
 * The trade is that enqueue and dequeue become plain single-word CAS, which on x86_64 is
 * **not** ABA-safe in general -- the lap number in the cell is what closes that hole. It
 * is the comparison point for VyukovDCAS, which instead pays for a real double-width CAS.
 */
template <typename T, typename Opt = meta::EmptyOptions, typename Link = linkage::None>
    requires meta::AcceptsOnly<Opt, typename VyukovNoABAOpt::no_cell_padding>
class VyukovNoABA : public mem::SingleBlock<VyukovNoABA<T, Opt, Link>> {
    using Self = VyukovNoABA<T, Opt, Link>;

    static_assert(sizeof(T) == sizeof(uintptr_t), "VyukovNoABA: T must be pointer-sized");
    static_assert(Link::is_linked == false, "VyukovNoABA is a standalone comparator only");

    static constexpr bool pad_cells = !Opt::template has<typename VyukovNoABAOpt::no_cell_padding>;

    /// Both ends set: unreachable for any real pointer, so it marks a non-payload word.
    static constexpr uintptr_t kMask = bit::set_msb<uintptr_t>(0) | uintptr_t{1};

    static constexpr uintptr_t lap_word(uint64_t lap) noexcept {
        return (static_cast<uintptr_t>(lap) << 1) | kMask;
    }
    static constexpr bool is_lap_word(uintptr_t w) noexcept { return (w & kMask) == kMask; }

public:
    using cell_type = cell::PlainCell<uintptr_t, pad_cells>;
    using link_state = typename Link::template state<Self>;
    using handle_type = typename link_state::handle;

    static constexpr std::size_t round_size(std::size_t n) noexcept {
        return bit::round_to_next_pow2(n < 2 ? 2 : n);
    }

    static constexpr auto plan(std::size_t n) noexcept {
        mem::LayoutBuilder b{sizeof(Self), alignof(Self)};
        mem::Plan<1> p{};
        p.regions[0] = b.add(round_size(n) * sizeof(cell_type), alignof(cell_type));
        p.total = b.total();
        p.block_align = b.block_align();
        return p;
    }

    VyukovNoABA(std::size_t n, mem::Blocks blk) noexcept
        : capacity_{round_size(n)}, shift_{bit::log2(round_size(n))},
          cells_{blk.template at<cell_type>(plan(n).regions[0])} {
        for (std::size_t i = 0; i < capacity_; ++i)
            cells_[i].val.store(lap_word(i >> shift_), std::memory_order_relaxed);
    }

    bool enqueue(T item) noexcept {
        assert(!is_lap_word(reinterpret_cast<uintptr_t>(item)) &&
               "VyukovNoABA: item collides with the reserved encoding");
        for (;;) {
            const uint64_t t = tail_.load(std::memory_order_relaxed);
            const uint64_t h = head_.load(std::memory_order_acquire);
            if (t != tail_.load(std::memory_order_acquire)) continue;
            if (t == h + capacity_) return false; // full

            uintptr_t expect = lap_word(t >> shift_);
            const bool won = cells_[t & (capacity_ - 1)].val.compare_exchange_weak(
                expect, reinterpret_cast<uintptr_t>(item), std::memory_order_acq_rel,
                std::memory_order_relaxed);
            uint64_t tt = t;
            (void)tail_.compare_exchange_weak(tt, t + 1, std::memory_order_relaxed);
            if (won) return true;
        }
    }

    bool dequeue(T& out) noexcept {
        for (;;) {
            const uint64_t t = tail_.load(std::memory_order_relaxed);
            uint64_t h = head_.load(std::memory_order_relaxed);
            cell_type& c = cells_[h & (capacity_ - 1)];
            uintptr_t val = c.val.load(std::memory_order_acquire);
            if (h != head_.load(std::memory_order_acquire)) continue;
            if (t == h) return false; // empty

            const uintptr_t next_lap = lap_word((h >> shift_) + 1);
            if (is_lap_word(val)) { // already taken; help the head along
                if (val == next_lap) {
                    uint64_t hh = h;
                    (void)head_.compare_exchange_weak(hh, h + 1, std::memory_order_relaxed);
                }
                continue;
            }

            const bool won = c.val.compare_exchange_weak(val, next_lap, std::memory_order_acq_rel,
                                                         std::memory_order_relaxed);
            uint64_t hh = h;
            (void)head_.compare_exchange_weak(hh, h + 1, std::memory_order_relaxed);
            if (won) {
                out = reinterpret_cast<T>(val);
                return true;
            }
        }
    }

    std::size_t size() const noexcept {
        const uint64_t t = tail_.load(std::memory_order_acquire);
        const uint64_t h = head_.load(std::memory_order_acquire);
        return t < h ? 0 : static_cast<std::size_t>(t - h);
    }

    std::size_t capacity() const noexcept { return capacity_; }

private:
    const std::size_t capacity_;
    const std::size_t shift_;
    cell_type* const cells_;
    ALIGNED_CACHE std::atomic<uint64_t> tail_{0};
    CACHE_PAD_TYPES(std::atomic<uint64_t>);
    ALIGNED_CACHE std::atomic<uint64_t> head_{0};
    CACHE_PAD_TYPES(std::atomic<uint64_t>);
};

} // namespace algo

namespace queue {
template <typename T, typename Opt = meta::EmptyOptions>
using VyukovNoABA = algo::VyukovNoABA<T, Opt, linkage::None>;
}
