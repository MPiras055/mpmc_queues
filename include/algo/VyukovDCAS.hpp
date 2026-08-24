#pragma once
/**
 * @file VyukovDCAS.hpp
 * @brief Vyukov's ring updating value and sequence in one double-width CAS; a comparator for what that instruction costs.
 * @ingroup algo
 */

#include <cell/SequencedCell.hpp>
#include <core/SegmentTraits.hpp>
#include <linkage/Linkage.hpp>
#include <mem/SingleBlock.hpp>
#include <meta/OptionsPack.hpp>
#include <util/atomic/cas2.hpp>
#include <util/bit.hpp>
#include <util/align.hpp>
#include <util/specs.hpp>
#include <atomic>
#include <cassert>

namespace algo {

struct VyukovDCASOpt {
    struct no_cell_padding {};
};

/**
 * @brief Vyukov's ring where value and sequence are swapped together by a double-width CAS.
 *
 * A benchmark comparator, not a segment.
 *
 * The plain Vyukov ring writes the payload and then publishes the sequence, so a stalled
 * producer leaves a claimed-but-unwritten cell. Updating both words in one instruction
 * removes that window, at the cost of needing a 16-byte CAS -- `lock cmpxchg16b` on
 * x86_64. The point of the comparison is what that instruction costs under contention.
 *
 * @note Uses the shared `cas2()` helper, which has x86_64, aarch64 and ARMv7 backends.
 *       The previous version carried its own inline-asm `CAS2` macro, its own is_pow2 and
 *       round_to_next_pow2, its own `#define CACHE_LINE`, and hand-written padding --
 *       all duplicates of things that already existed.
 */
template <typename T, typename Opt = meta::EmptyOptions, typename Link = linkage::None>
    requires meta::AcceptsOnly<Opt, typename VyukovDCASOpt::no_cell_padding>
class VyukovDCAS : public mem::SingleBlock<VyukovDCAS<T, Opt, Link>> {
    using Self = VyukovDCAS<T, Opt, Link>;

    static_assert(sizeof(T) == sizeof(uintptr_t), "VyukovDCAS: T must be pointer-sized");
    static_assert(Link::is_linked == false, "VyukovDCAS is a standalone comparator only");

    static constexpr bool pad_cells = !Opt::template has<typename VyukovDCASOpt::no_cell_padding>;

public:
    /// Value and sequence must be adjacent and 16-byte aligned for the double-width CAS.
    using cell_type = cell::SequencedCell<T, pad_cells>;
    using link_state = typename Link::template state<Self>;
    using handle_type = typename link_state::handle;

    static constexpr std::size_t round_size(std::size_t n) noexcept {
        return bit::round_to_next_pow2(n < 2 ? 2 : n);
    }

    /// @brief Where the co-allocated regions go. See @ref block-construction.
    /// @param n requested capacity; the only thing the layout may depend on.
    static constexpr auto plan(std::size_t n) noexcept {
        mem::LayoutBuilder b{sizeof(Self), alignof(Self)};
        mem::Plan<1> p{};
        p.regions[0] = b.add(round_size(n) * sizeof(cell_type), alignof(cell_type));
        p.total = b.total();
        p.block_align = b.block_align();
        return p;
    }

    VyukovDCAS(std::size_t n, mem::Blocks blk) noexcept
        : capacity_{round_size(n)}, cells_{blk.template at<cell_type>(plan(n).regions[0])} {
        static_assert(alignof(cell_type) >= 16, "cas2 requires the pair to be 16-byte aligned");
        for (std::size_t i = 0; i < capacity_; ++i) {
            cells_[i].val.store(T{}, std::memory_order_relaxed);
            cells_[i].seq.store(i, std::memory_order_relaxed);
        }
    }

    /// @brief Add an item.
    /// @return false if the queue is full, or closed.
    bool enqueue(T item) noexcept {
        uint64_t t = tail_.load(std::memory_order_relaxed);
        for (;;) {
            cell_type& c = cells_[t & (capacity_ - 1)];
            const uint64_t seq = c.seq.load(std::memory_order_relaxed);
            if (t == seq) {
                uint64_t expect_val = 0, expect_seq = seq;
                // Install payload and next sequence as one indivisible update.
                const bool installed = p_atomic::dcas(&c, expect_val, expect_seq,
                                                          reinterpret_cast<uint64_t>(item), t + 1);
                uint64_t tt = t;
                const bool moved = tail_.compare_exchange_weak(tt, t + 1, std::memory_order_relaxed);
                if (installed) return true;
                t += moved ? 1 : 0; // avoid re-loading the tail when we know it moved
                if (!moved) t = tail_.load(std::memory_order_acquire);
            } else if (t > seq) {
                return false; // full
            } else {
                uint64_t tt = t;
                const bool moved = tail_.compare_exchange_weak(tt, t + 1, std::memory_order_relaxed);
                t = moved ? t + 1 : tail_.load(std::memory_order_acquire);
            }
        }
    }

    /// @brief Take the oldest item.
    /// @return false if the queue is empty.
    bool dequeue(T& out) noexcept {
        uint64_t h = head_.load(std::memory_order_relaxed);
        for (;;) {
            cell_type& c = cells_[h & (capacity_ - 1)];
            const uint64_t seq = c.seq.load(std::memory_order_acquire);
            T value = c.val.load(std::memory_order_acquire);
            if (seq == h + 1) {
                uint64_t expect_val = reinterpret_cast<uint64_t>(value), expect_seq = seq;
                const bool took = p_atomic::dcas(&c, expect_val, expect_seq, 0, h + capacity_);
                uint64_t hh = h;
                const bool moved = head_.compare_exchange_weak(hh, h + 1, std::memory_order_relaxed);
                if (took) {
                    out = value;
                    return true;
                }
                h = moved ? h + 1 : head_.load(std::memory_order_acquire);
            } else if (seq < h + 1) {
                return false; // empty
            } else {
                uint64_t hh = h;
                const bool moved = head_.compare_exchange_weak(hh, h + 1, std::memory_order_relaxed);
                h = moved ? h + 1 : head_.load(std::memory_order_acquire);
            }
        }
    }

    /// @return Items currently held. Approximate under concurrency, exact when quiescent.
    std::size_t size() const noexcept {
        const uint64_t t = tail_.load(std::memory_order_acquire);
        const uint64_t h = head_.load(std::memory_order_acquire);
        return t > h ? static_cast<std::size_t>(t - h) : 0;
    }

    /// @return Items this queue can hold.
    std::size_t capacity() const noexcept { return capacity_; }

private:
    const std::size_t capacity_;
    cell_type* const cells_;
    CACHE_LINE_MEMBER(std::atomic<uint64_t>, tail_, {0});
    CACHE_LINE_MEMBER(std::atomic<uint64_t>, head_, {0});
};

} // namespace algo

namespace queue {
template <typename T, typename Opt = meta::EmptyOptions>
/// Standalone ring updating value and sequence in one double-width CAS.
using VyukovDCAS = algo::VyukovDCAS<T, Opt, linkage::None>;
}
