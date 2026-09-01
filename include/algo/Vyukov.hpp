#pragma once
/**
 * @file Vyukov.hpp
 * @brief Bounded MPMC ring with a sequence number per cell; the baseline every other algorithm here is compared against.
 * @ingroup algo
 */

#include <cell/SequencedCell.hpp>
#include <core/SegmentTraits.hpp>
#include <linkage/Linkage.hpp>
#include <mem/SingleBlock.hpp>
#include <meta/OptionsPack.hpp>
#include <util/bit.hpp>
#include <util/align.hpp>
#include <util/specs.hpp>
#include <atomic>
#include <cassert>

namespace algo {

/** @brief Compile-time configuration for Vyukov-style ring buffers. */
struct VyukovOpt {
    /**
     * @brief Take the capacity exactly as asked instead of rounding up to a power of two.
     *
     * Rounding is the default so that every algorithm answers a capacity request the same way
     * and a cross-algorithm benchmark compares the same geometry. Opting out costs a division
     * where the default path masks.
     */
    struct no_pow2 {};
    /** Pack cells instead of padding each to a cache line. */
    struct no_cell_padding {};
};

/**
 * @brief Vyukov's bounded MPMC ring: one sequence counter per cell, CAS on the index.
 *
 * One body serves both shapes. With `Link = linkage::None` this is a standalone bounded
 * queue; with `Link = linkage::Node<...>` it is a segment of a linked unbounded queue, and
 * the closed check inside the enqueue loop compiles in. Previously these were two
 * classes coupled by a `Derived = void` CRTP sentinel, where the same class was
 * simultaneously an abstract-base implementer and a CRTP base calling back down into
 * itself.
 *
 * @tparam T    element type
 * @tparam Opt  meta::OptionsPack of VyukovOpt flags
 * @tparam Link linkage::None or linkage::Node<HandlePolicy>
 */
template <typename T, typename Opt = meta::EmptyOptions, typename Link = linkage::None>
    requires meta::AcceptsOnly<Opt, typename VyukovOpt::no_pow2, typename VyukovOpt::no_cell_padding>
class Vyukov : public mem::SingleBlock<Vyukov<T, Opt, Link>> {
    using Self = Vyukov<T, Opt, Link>;


    static constexpr bool pad_cells = !Opt::template has<typename VyukovOpt::no_cell_padding>;
    /// Index mapping is a mask rather than a modulo; see VyukovOpt::no_pow2.
    static constexpr bool pow2 = !Opt::template has<typename VyukovOpt::no_pow2>;

public:
    using cell_type = cell::SequencedCell<T, pad_cells>;
    using link_state = typename Link::template state<Self>;
    using handle_type = typename link_state::handle;

    /** @brief Capacity actually used for a requested size. */
    /**
     * @note The floor of two is not cosmetic. These cells distinguish laps by `seq == t +
     *       capacity`, and at capacity 1 that aliases with the very next ticket: a one-slot
     *       ring never reports itself full, accepts unboundedly, and overwrites its own cell.
     *       Measured directly: `create(1)` accepted three items and reported `size() == 3`.
     *       A linked segment that never closes also never gets a successor, so the proxy
     *       spins there for ever.
     */
    static constexpr std::size_t round_size(std::size_t n) noexcept {
        if constexpr (pow2) return bit::round_to_next_pow2(n > 1 ? n : 2);
        else return n > 1 ? n : 2;
    }

    // -- single-block layout -------------------------------------------------
    /**
     * @brief What a capacity request of @p n actually yields.
     * @return the capacity a segment built with @p n will report.
     *
     * Static so a caller can size a split before anything is constructed: LinkedProxy divides
     * its total across the segments that will exist, and has to know what each one rounds to
     * before it can report a capacity the queue can genuinely reach.
     */
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

    Vyukov(std::size_t n, mem::Blocks blk) noexcept
        : capacity_{round_size(n)}, cells_{blk.template at<cell_type>(plan(n).regions[0])} {
        assert(capacity_ != 0 && "Vyukov: capacity must be non-null");
        for (std::size_t i = 0; i < capacity_; ++i) {
            cells_[i].val.store(T{}, std::memory_order_relaxed);
            cells_[i].seq.store(i, std::memory_order_relaxed);
        }
    }

    // -- queue ---------------------------------------------------------------

    /// @brief Add an item.
    /// @return false if the queue is full, or closed.
    FORCE_INLINE bool enqueue(T item) noexcept {
        uint64_t t = tail_.load(std::memory_order_relaxed);
        for (;;) {
            if constexpr (Link::is_linked) {
                if (is_closed_ticket(t)) return false;
            }
            cell_type& c = cells_[mod(t)];
            const uint64_t seq = c.seq.load(std::memory_order_acquire);
            if (t == seq) {
                if (tail_.compare_exchange_weak(t, t + 1, std::memory_order_relaxed)) {
                    c.val.store(item, std::memory_order_relaxed);
                    c.seq.store(seq + 1, std::memory_order_release);
                    return true;
                }
            } else if (t > seq) {
                // Ring is full. A linked segment closes itself so the proxy stops
                // retrying here and links a successor instead.
                if constexpr (Link::is_linked) close();
                return false;
            } else {
                t = tail_.load(std::memory_order_acquire);
            }
        }
    }

    /// Hinted overload; Vyukov's own closed check is already inside the loop.
    FORCE_INLINE bool enqueue(T item, bool /*closed_hint*/) noexcept { return enqueue(item); }

    /// @brief Take the oldest item.
    /// @return false if the queue is empty.
    FORCE_INLINE bool dequeue(T& out) noexcept {
        uint64_t h = head_.load(std::memory_order_relaxed);
        for (;;) {
            cell_type& c = cells_[mod(h)];
            const uint64_t seq = c.seq.load(std::memory_order_acquire);
            if (seq == h + 1) {
                if (head_.compare_exchange_weak(h, h + 1, std::memory_order_relaxed)) {
                    out = c.val.load(std::memory_order_acquire);
                    c.seq.store(h + capacity_, std::memory_order_release);
                    return true;
                }
            } else if (seq < h + 1 && size() == 0) {
                return false;
            }
            h = head_.load(std::memory_order_acquire);
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
        return t >= h ? static_cast<std::size_t>(t - h) : 0;
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
     * @return true — Vyukov recycles by realigning indices, with no buffer rewrite.
     * @warning Not MT-safe; the segment must already be drained and unreachable.
     */
    bool reopen() noexcept
        requires(Link::is_linked)
    {
        link_.unlink();
        tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release);
        return true;
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

    CACHE_LINE_MEMBER(std::atomic<uint64_t>, head_, {0});
    CACHE_LINE_MEMBER(std::atomic<uint64_t>, tail_, {0});
    [[no_unique_address]] link_state link_{};
    const std::size_t capacity_;
    cell_type* const cells_;
};

} // namespace algo

/// @brief Capabilities of algo::Vyukov as a linked segment. Every field is mandatory:
/// core::segment_traits has no primary definition, so omitting one is a compile error.
template <typename T, typename Opt, typename Link>
struct core::segment_traits<algo::Vyukov<T, Opt, Link>> {
    /// Vyukov re-checks its own closed flag on every loop iteration, so an external
    /// hint would be redundant.
    static constexpr bool needs_close_hint = false;
    static constexpr bool needs_dequeue_prepare = false;
    /// A single atomic step publishes the item; nothing can be mid-insert.
    static constexpr bool needs_inflight_drain = false;
    /// reopen() is index realignment only.
    static constexpr bool recyclable = true;
    static constexpr bool can_store_null = true;
};
MPMC_ASSERT_SEGMENT_TRAITS(algo::Vyukov<int*, meta::EmptyOptions, linkage::None>);

namespace queue {
/// Standalone bounded Vyukov ring.
template <typename T, typename Opt = meta::EmptyOptions>
/// Standalone bounded Vyukov ring.
using Vyukov = algo::Vyukov<T, Opt, linkage::None>;
} // namespace queue

namespace seg {
/// Vyukov as a linked segment.
template <typename T, typename Opt = meta::EmptyOptions, typename HP = mem::PtrHandle>
/// Vyukov's ring as a linked segment.
using Vyukov = algo::Vyukov<T, Opt, linkage::Node<HP>>;
} // namespace seg
