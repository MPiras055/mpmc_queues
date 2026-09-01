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
    /**
     * @brief Take the capacity exactly as asked instead of rounding up to a power of two.
     *
     * Rounding is the default so that every algorithm answers a capacity request the same way
     * and a cross-algorithm benchmark compares the same geometry. Opting out costs a division
     * where the default path masks.
     */
    struct no_pow2 {};
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
    requires meta::AcceptsOnly<Opt, typename VyukovDCASOpt::no_cell_padding, typename VyukovDCASOpt::no_pow2>
class VyukovDCAS : public mem::SingleBlock<VyukovDCAS<T, Opt, Link>> {
    using Self = VyukovDCAS<T, Opt, Link>;

    static_assert(sizeof(T) == sizeof(uintptr_t), "VyukovDCAS: T must be pointer-sized");

    static constexpr bool pad_cells = !Opt::template has<typename VyukovDCASOpt::no_cell_padding>;
    /// Index mapping is a mask rather than a modulo; see VyukovDCASOpt::no_pow2.
    static constexpr bool pow2 = !Opt::template has<typename VyukovDCASOpt::no_pow2>;

public:
    /// Value and sequence must be adjacent and 16-byte aligned for the double-width CAS.
    using cell_type = cell::SequencedCell<T, pad_cells>;
    using link_state = typename Link::template state<Self>;
    using handle_type = typename link_state::handle;

    static constexpr std::size_t round_size(std::size_t n) noexcept {
        // Floored at 2: a ring that distinguishes laps by `seq == t + capacity` aliases at
        // capacity 1, so it never reports itself full.
        const std::size_t f = n < 2 ? 2 : n;
        if constexpr (pow2) return bit::round_to_next_pow2(f);
        else return f;
    }

    /// @return the capacity a queue built with @p n will report.
    static constexpr std::size_t capacity_for(std::size_t n) noexcept { return round_size(n); }

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

    /// @brief Add an item, skipping the attempt when the caller already knows it is closed.
    /// @param closed_hint the caller believes this segment is closed; see
    ///        core::segment_traits::needs_close_hint.
    bool enqueue(T item, bool /*closed_hint*/) noexcept { return enqueue(item); }

    /// @brief Add an item.
    /// @return false if the queue is full, or closed.
    bool enqueue(T item) noexcept {
        uint64_t t = tail_.load(std::memory_order_relaxed);
        for (;;) {
            if constexpr (Link::is_linked) {
                if (is_closed_ticket(t)) return false;
            }
            cell_type& c = cells_[mod(t)];
            const uint64_t seq = c.seq.load(std::memory_order_relaxed);
            if (t == seq) {
                // Reserve the ticket *before* touching the cell. This is what makes close()
                // enforcing: it sets the top bit of the tail, so every outstanding reservation
                // CAS fails from that moment on. Previously the double-CAS below decided
                // success while this CAS was best-effort, which meant a producer could publish
                // into a segment the proxy had already drained, unlinked and retired -- the
                // item counted as enqueued and never traversed again. Measured at 4P/4C on
                // 16-slot segments: 12 of 25 trials lost items before this reordering.
                uint64_t tt = t;
                if (!tail_.compare_exchange_weak(tt, t + 1, std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
                    t = tt;
                    continue;
                }
                // Ticket t is now ours alone, and the full check above guarantees the cell has
                // been consumed, so the sequence still reads t and no consumer will touch it
                // until we publish t + 1. Install payload and sequence as one indivisible
                // update; retry only a spurious failure.
                for (;;) {
                    uint64_t expect_val = 0, expect_seq = seq;
                    if (p_atomic::dcas(&c, expect_val, expect_seq,
                                       reinterpret_cast<uint64_t>(item), t + 1))
                        return true;
                    assert(c.seq.load(std::memory_order_acquire) == seq &&
                           "VyukovDCAS: a reserved cell moved under its owner");
                }
            } else if (t > seq) {
                // Ring is full. A linked segment closes itself so the proxy stops retrying
                // here and links a successor instead; see algo::Vyukov::enqueue.
                if constexpr (Link::is_linked) close();
                return false;
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
            cell_type& c = cells_[mod(h)];
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


    /// @copydoc core::Queue::try_enqueue
    /// Never blocks, so this is the same operation as enqueue().
    bool try_enqueue(T item) noexcept { return enqueue(item); }
    /// @copydoc core::Queue::try_dequeue
    /// Never blocks, so this is the same operation as dequeue().
    bool try_dequeue(T& out) noexcept { return dequeue(out); }

    /// @return Items currently held. Approximate under concurrency, exact when quiescent.
    std::size_t size() const noexcept {
        uint64_t t = tail_.load(std::memory_order_acquire);
        if constexpr (Link::is_linked) t = bit::clear_msb(t);
        const uint64_t h = head_.load(std::memory_order_acquire);
        return t > h ? static_cast<std::size_t>(t - h) : 0;
    }

    /// @return Items this queue can hold.
    std::size_t capacity() const noexcept { return capacity_; }

    // -- linkage (only when linked) -----------------------------------------

    /// @brief Refuse all further enqueues, permanently.
    void close() noexcept
        requires(Link::is_linked)
    {
        tail_.fetch_or(bit::set_msb<uint64_t>(0), std::memory_order_release);
    }

    /// @return true once closed; a closed segment still drains.
    bool is_closed() const noexcept
        requires(Link::is_linked)
    {
        return is_closed_ticket(tail_.load(std::memory_order_acquire));
    }

    /**
     * @brief Reset a drained segment for reuse.
     * @return true -- realigning the tail on the head is enough; the cells a drained segment
     *         leaves behind already carry the sequence numbers the next lap expects.
     * @warning Not MT-safe; the segment must already be drained and unreachable.
     */
    bool reopen() noexcept
        requires(Link::is_linked)
    {
        link_.unlink();
        tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release);
        return true;
    }

    /**
     * @brief Is a producer holding a reserved ticket it has not published yet?
     *
     * Reserving the tail and publishing the cell are two steps, so between them the cell still
     * reads as the previous lap and a consumer calls the segment empty. Without this the proxy
     * would take that "empty" at face value, unlink and retire the segment, and the reservation
     * would then publish into something nothing traverses.
     *
     * Every reserved ticket is published (the reservation is unconditional now), so `head` can
     * only lag `tail` when items are pending or a publish is in flight -- and the proxy only
     * asks once dequeue has already reported empty.
     * @see core::segment_traits::needs_inflight_drain
     */
    bool has_inflight() const noexcept
        requires(Link::is_linked)
    {
        return bit::clear_msb(tail_.load(std::memory_order_acquire)) !=
               head_.load(std::memory_order_acquire);
    }

    /// @return The successor handle, or nil if this is the tail.
    handle_type next() const noexcept
        requires(Link::is_linked)
    {
        return link_.next();
    }

    /// @brief Publish @p h as the successor.
    /// @param current set to the successor now installed -- @p h if we won, the winner's
    ///        handle if we lost, so a losing caller need not re-read next().
    /// @return true for exactly one caller; the loser must discard its segment.
    bool link_next(handle_type h, handle_type& current) noexcept
        requires(Link::is_linked)
    {
        return link_.link_next(h, current);
    }

private:
    static constexpr bool is_closed_ticket(uint64_t t) noexcept { return bit::get_msb(t) != 0; }

    FORCE_INLINE std::size_t mod(uint64_t i) const noexcept {
        if constexpr (pow2) return i & (capacity_ - 1);
        else return i % capacity_;
    }

    [[no_unique_address]] link_state link_{};
    const std::size_t capacity_;
    cell_type* const cells_;
    CACHE_LINE_MEMBER(std::atomic<uint64_t>, tail_, {0});
    CACHE_LINE_MEMBER(std::atomic<uint64_t>, head_, {0});
};

} // namespace algo

/// @brief Capabilities of algo::VyukovDCAS as a linked segment. Every field is mandatory:
/// core::segment_traits has no primary definition, so omitting one is a compile error.
template <typename T, typename Opt, typename Link>
struct core::segment_traits<algo::VyukovDCAS<T, Opt, Link>> {
    /// The closed flag lives in the tail ticket, which every loop iteration re-reads.
    static constexpr bool needs_close_hint = false;
    static constexpr bool needs_dequeue_prepare = false;
    /// The double-width CAS publishes value and sequence together, so no *cell* is ever seen
    /// half-written. The gap that matters is earlier: between reserving the tail ticket and
    /// running that CAS. See VyukovDCAS::has_inflight.
    static constexpr bool needs_inflight_drain = true;
    /// reopen() is tail realignment only.
    static constexpr bool recyclable = true;
    /// A null payload is indistinguishable from the empty cell this uses as its CAS expectation.
    static constexpr bool can_store_null = false;
};
MPMC_ASSERT_SEGMENT_TRAITS(algo::VyukovDCAS<int*, meta::EmptyOptions, linkage::None>);

namespace queue {
template <typename T, typename Opt = meta::EmptyOptions>
/// Standalone ring updating value and sequence in one double-width CAS.
using VyukovDCAS = algo::VyukovDCAS<T, Opt, linkage::None>;
}

namespace seg {
template <typename T, typename Opt = meta::EmptyOptions, typename HP = mem::PtrHandle>
/// The double-width-CAS ring as a linked segment.
using VyukovDCAS = algo::VyukovDCAS<T, Opt, linkage::Node<HP>>;
}
