#pragma once
/**
 * @file HQ.hpp
 * @brief FAAArray's array with a non-destructive dequeue for the tail segment.
 * @ingroup algo
 */

#include <cell/PlainCell.hpp>
#include <cell/Tagging.hpp>
#include <core/SegmentTraits.hpp>
#include <linkage/Linkage.hpp>
#include <mem/SingleBlock.hpp>
#include <meta/OptionsPack.hpp>
#include <util/align.hpp>
#include <util/specs.hpp>
#include <atomic>
#include <cassert>

namespace algo {

struct HQOpt {
    struct force_cell_padding {};

    /**
     * @brief Loads a consumer spends waiting for a straggling producer before claiming a cell.
     *
     * Only reached when something *is* queued behind the stalled index, so the cell has to be
     * passed one way or the other; this decides how hard to try for the payload first. Each
     * iteration is one load plus a `SPIN_HINT()`, so the useful range is small -- a producer
     * that has not landed within a few dozen cycles is descheduled, not merely behind.
     */
    template <auto N> struct patience {};
};

/**
 * @brief FAAArray's linear array with a non-destructive slow path for the tail segment.
 *
 * Same write-once array as FAAArray, but dequeue picks a strategy:
 *
 *  - **fast path** (a successor exists, so this segment will never grow again):
 *    fetch-add the head and invalidate stragglers, exactly as FAAArray does.
 *  - **slow path** (this is the only segment): advance the head by CAS instead, so a
 *    consumer that arrives at an empty cell does *not* burn the slot. On a
 *    near-empty queue this is what keeps a producer/consumer pair from destroying
 *    capacity, at the cost of a heavier loop.
 *
 * ## What the destructive exchange is for, and when it is not needed
 *
 * Both paths can overwrite a cell a producer has claimed but not yet written. That is not
 * about impatience with stragglers -- it is the only cure for **head-of-line blocking**.
 * A producer claims index `h` and stalls; another publishes at `h + 1`. A consumer that
 * refused to pass `h` would report empty while a published item sat behind it, which is
 * the linearizability violation the burn exists to prevent.
 *
 * It follows that when nothing is queued behind `h` -- `tail == h + 1` -- the burn buys
 * nothing, and the slow path now says empty instead. That is linearizable at the load of
 * the cell: the producer's enqueue linearizes at the CAS that publishes its payload, so
 * before that the queue really is empty. It is also the whole point of the slow path,
 * which previously destroyed the cell anyway after two loads.
 *
 * ## Progress
 *
 * Lock-free, and bounded-wait-free but for spurious `compare_exchange_weak` failures.
 * `kPatience` is sometimes mistaken for a blocking wait; it is not. Every loop here is
 * bounded by something monotone: patience by a compile-time constant, the enqueue retry by
 * a distinct `tail_` ticket per iteration, and every slow-path `continue` by `head_`
 * having advanced -- and both indices are capped at `capacity_`. No operation can be
 * delayed indefinitely by another thread stalling. What `kPatience` trades is capacity,
 * not liveness.
 *
 * This is the segment the README describes as trading throughput for a better memory
 * footprint. It had been dead since the interface moved out from under it.
 *
 * Reuse works exactly as in FAAArray: a life ends with every cell `consumed`, and a
 * generation flag swaps which sentinel word means empty, so reopen() is O(1) rather than a
 * sweep of the array. See reopen().
 *
 * @note **Linked-only.** HQ has FAAArray's write-once cells, plus a dequeue that picks its
 *       strategy from whether a successor exists, so standalone it is both single-use and
 *       permanently on the slow path. The class template is constrained on linkage::Linked,
 *       so `algo::HQ<T, Opt, linkage::None>` is not a nameable type.
 *
 * @tparam Tag cell::Tagging policy. LowTag by default, matching the original encoding
 *             (0 = empty, 1 = consumed) -- which means null payloads are unstorable.
 *             Pass cell::MsbTag<T> if null must round-trip.
 */
template <typename T, typename Opt, typename Link,
          typename Tag = cell::LowTag<T>>
    requires meta::AcceptsOnly<Opt, typename HQOpt::force_cell_padding,
                               meta::ValueOption<HQOpt::patience>> &&
             linkage::Linked<Link> && cell::Tagging<Tag, T>
class HQ : public mem::SingleBlock<HQ<T, Opt, Link, Tag>> {
    using Self = HQ<T, Opt, Link, Tag>;

    using word = typename Tag::word;

    static constexpr bool pad_cells = Opt::template has<typename HQOpt::force_cell_padding>;
    /// Cast rather than trusted: `get` returns the option's own type when one is present, so
    /// `patience<8>` would otherwise arrive as `int`. See OptionsPack::get.
    static constexpr std::size_t kPatience =
        static_cast<std::size_t>(Opt::template get<HQOpt::patience, std::size_t{2}>);

public:
    /// Loads a consumer spends on a straggling producer; see HQOpt::patience.
    static constexpr std::size_t patience = kPatience;

    using tag_type = Tag;
    using cell_type = cell::PlainCell<word, pad_cells>;
    using link_state = typename Link::template state<Self>;
    using handle_type = typename link_state::handle;

    /**
     * @brief What a capacity request of @p n actually yields.
     * @return the capacity a segment built with @p n will report.
     *
     * Static so a caller can size a split before anything is constructed: LinkedProxy divides
     * its total across the segments that will exist, and has to know what each one rounds to
     * before it can report a capacity the queue can genuinely reach.
     */
    static constexpr std::size_t capacity_for(std::size_t n) noexcept { return n; }

    /// @brief Where the co-allocated regions go. See @ref block-construction.
    /// @param n requested capacity; the only thing the layout may depend on.
    static constexpr auto plan(std::size_t n) noexcept {
        mem::LayoutBuilder b{sizeof(Self), alignof(Self)};
        mem::Plan<1> p{};
        p.regions[0] = b.add(n * sizeof(cell_type), alignof(cell_type));
        p.total = b.total();
        p.block_align = b.block_align();
        return p;
    }

    HQ(std::size_t n, mem::Blocks blk) noexcept
        : capacity_{n}, cells_{blk.template at<cell_type>(plan(n).regions[0])} {
        assert(n != 0 && "HQ: capacity must be non-null");
        for (std::size_t i = 0; i < n; ++i)
            cells_[i].val.store(empty_w(), std::memory_order_relaxed);
    }

    /// @brief Add an item.
    /// @return false if the queue is full, or closed.
    FORCE_INLINE bool enqueue(T item) noexcept {
        assert((Tag::can_store_null || Tag::is_payload(Tag::encode(item))) &&
               "HQ: this tagging policy cannot store that value (see can_store_null)");
        for (;;) {
            const uint64_t t = tail_.fetch_add(1, std::memory_order_acq_rel);
            if (t >= capacity_) return false;
            // NB: `expected` is reloaded every iteration. The original hoisted it out of
            // the loop, so one failed CAS left it holding the observed word and every
            // later attempt compared against that stale value instead of `empty`.
            word expected = empty_w();
            if (cells_[t].val.compare_exchange_strong(expected, Tag::encode(item),
                                                      std::memory_order_acq_rel,
                                                      std::memory_order_acquire))
                return true;
        }
    }

    /// @brief Add an item, skipping the attempt when the caller already knows it is closed.
    /// @param closed_hint the caller believes this segment is closed; see
    ///        core::segment_traits::needs_close_hint.
    /// @return false if the queue is full, or closed.
    FORCE_INLINE bool enqueue(T item, bool /*closed_hint*/) noexcept { return enqueue(item); }

    /// @brief Take the oldest item.
    /// @return false if the queue is empty.
    FORCE_INLINE bool dequeue(T& out) noexcept {
        return has_successor() ? fast_dequeue(out) : slow_dequeue(out);
    }

    /// @return Items currently held. Approximate under concurrency, exact when quiescent.
    std::size_t size() const noexcept {
        const uint64_t t = tail_.load(std::memory_order_acquire);
        const uint64_t h = head_.load(std::memory_order_acquire);
        const uint64_t capped = t > capacity_ ? capacity_ : t;
        return capped > h ? static_cast<std::size_t>(capped - h) : 0;
    }

    /// @return Items this queue can hold.
    std::size_t capacity() const noexcept { return capacity_; }

    /// @brief Refuse all further enqueues, permanently.
    void close() noexcept
        requires(Link::is_linked)
    {
        tail_.fetch_add(capacity_, std::memory_order_release);
    }

    /// @return true once closed; a closed segment still drains.
    bool is_closed() const noexcept
        requires(Link::is_linked)
    {
        return tail_.load(std::memory_order_acquire) >= capacity_;
    }

    /**
     * @brief Reopen a drained segment in O(1) by flipping the generation flag.
     * @return true, always.
     *
     * A life ends with every cell holding `consumed`. The flag swaps which sentinel word
     * *means* empty, so the array the previous life left behind already reads as fresh --
     * no sweep. The original open() stored 0 into head and tail and returned true without
     * touching the cells, which left the segment behaving as instantly full: every
     * subsequent enqueue CAS compared against `empty` and found `consumed`.
     *
     * @pre The segment is quiescent. The proxy guarantees it -- reopen only happens on a
     *      segment just acquired from a recycling source and not yet published, and such a
     *      source does not hand one back while any thread can still be inside it. That
     *      matters rather than being tidy: a producer that had already fetch-added an
     *      index holds a CAS whose `expected` is the old generation's empty, which is the
     *      new generation's `consumed`, so it would succeed and write a payload into the
     *      reopened segment.
     *
     * The flip alone is not enough, because the proxy reopens every segment it acquires
     * and only one of the three states it can be in is uniformly `consumed`:
     *
     * | state                       | how it arises                          | cost |
     * |-----------------------------|----------------------------------------|------|
     * | pristine (head = tail = 0)  | first hand-out of a pool slot          | O(1), nothing to do |
     * | fully drained (head >= cap) | the ordinary recycle                   | O(1), flip |
     * | partially used              | acquired, filled, lost the link race, `discard`ed | O(capacity) sweep |
     *
     * `head_ >= capacity_` is an exact test for "every cell is consumed" on both dequeue
     * paths: each only advances past an index after exchanging `consumed` into it.
     *
     * @note `gen_` is a plain bool: reopen is single-threaded by that precondition, and
     *       the segment is published afterwards by a release CAS (`link_next`) that every
     *       reader reaches through an acquire load.
     */
    bool reopen() noexcept
        requires(Link::is_linked)
    {
        link_.unlink(); // a recycled segment must not carry a stale successor

        const uint64_t h = head_.exchange(0,std::memory_order_relaxed);
        const uint64_t t = tail_.exchange(0,std::memory_order_relaxed);


        // Pristine: this slot has never been handed out, so every cell is already empty for
        // the current generation and there is nothing to undo. Returned early rather than
        // written as `if (...);` with an empty body -- a semicolon-terminated `if` is the
        // classic silent-bug shape, and these three branches decide whether a recycled
        // segment is left alone, flipped, or swept.
        if (h == 0 && t == 0) return true;

        if (h >= capacity_) {  //all cells used
#ifndef NDEBUG
            for (std::size_t i = 0; i < capacity_; ++i)
                assert(is_consumed_w(cells_[i].val.load(std::memory_order_relaxed)) &&
                       "FAAArray::reopen: head ran past capacity but a cell is not consumed");
#endif
            gen_ = !gen_;   //every cell was used so we flip the generation
        } else {
            //hard reset of all the cells
            for (std::size_t i = 0; i < capacity_; ++i)
                cells_[i].val.store(empty_w(), std::memory_order_relaxed);
        }
        
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
    /**
     * @name Generation-relative sentinels
     *
     * The two reserved words swap roles on every life. `is_payload` is untouched by the
     * swap -- both words are non-payload either way -- so decoding is unaffected. Going
     * through the Tag predicates rather than comparing words keeps this inside the
     * cell::Tagging contract instead of assuming the sentinels are equality tested.
     * @{
     */
    word empty_w() const noexcept { return gen_ ? Tag::consumed() : Tag::empty(); }
    word consumed_w() const noexcept { return gen_ ? Tag::empty() : Tag::consumed(); }
    bool is_empty_w(word w) const noexcept {
        return gen_ ? Tag::is_consumed(w) : Tag::is_empty(w);
    }
    bool is_consumed_w(word w) const noexcept {
        return gen_ ? Tag::is_empty(w) : Tag::is_consumed(w);
    }
    /// @}

    bool has_successor() const noexcept {
        if constexpr (Link::is_linked) return link_.next() != link_state::nil();
        else return false;
    }

    /// Destructive: burns a slot if the producer has not arrived. Safe once closed.
    bool fast_dequeue(T& out) noexcept {
        for (;;) {
            const uint64_t h = head_.fetch_add(1, std::memory_order_acq_rel);
            if (h >= capacity_) return false;
            cell_type& c = cells_[h];
            // A pure delay, and deliberately so: the exchange below re-reads the cell, so
            // this loop's value is never used. Its only job is to give a producer that is
            // a few cycles behind time to land, because once the head has fetch-added past
            // this index the slot is gone either way. Without the hint two dependent loads
            // of an already-hot line retire in a handful of cycles and the wait is noise.
            for (std::size_t i = 0; i < kPatience; ++i) {
                if (!is_empty_w(c.val.load(std::memory_order_acquire))) break;
                SPIN_HINT();
            }
            const word w = c.val.exchange(consumed_w(), std::memory_order_acq_rel);
            if (Tag::is_payload(w)) {
                out = Tag::decode(w);
                return true;
            }
        }
    }

    /// Non-destructive until patience runs out: advances head by CAS, not fetch-add.
    bool slow_dequeue(T& out) noexcept {
        for (;;) {
            uint64_t h = head_.load(std::memory_order_relaxed);
            if (h >= capacity_) return false;

            cell_type& c = cells_[h];
            word w = c.val.load(std::memory_order_acquire);
            const uint64_t t = tail_.load(std::memory_order_acquire);

            if (h != head_.load(std::memory_order_acquire)) continue; // someone moved head
            if (h == t) return false;                                 // genuinely empty

            if (is_consumed_w(w)) { // already taken; help head along
                (void)head_.compare_exchange_weak(h, h + 1, std::memory_order_relaxed);
                continue;
            }

            if (is_empty_w(w)) { // producer claimed this index but has not published
                // Nothing is queued behind it, so there is no head-of-line blocking to
                // break -- and breaking it is the *only* thing the destructive exchange
                // below buys. Burning the cell here would cost capacity for nothing,
                // which on a near-empty queue is precisely the pathology the slow path
                // exists to avoid, and the old code still walked into it after two loads.
                //
                // Reporting empty is linearizable: it linearizes at the load of `w`
                // above, and at that instant the producer's enqueue had not yet
                // linearized -- it does so at the CAS that publishes the payload -- so
                // the queue genuinely held no item.
                if (t == h + 1) return false;

                // Something *is* published behind this index, so the head has to get
                // past it. Give the producer a bounded chance to land first.
                for (std::size_t i = 0; i < kPatience; ++i) {
                    w = c.val.load(std::memory_order_acquire);
                    if (!is_empty_w(w)) break; // resolved, either payload or consumed
                    SPIN_HINT();
                }
                if (is_consumed_w(w)) { // another consumer got there; help head along
                    (void)head_.compare_exchange_weak(h, h + 1, std::memory_order_relaxed);
                    continue;
                }
            }

            // Patience exhausted or a payload is present: claim the cell. This races
            // with the producer and may invalidate the slot -- the obstruction-free part.
            w = c.val.exchange(consumed_w(), std::memory_order_acq_rel);
            (void)head_.compare_exchange_weak(h, h + 1, std::memory_order_relaxed);
            if (Tag::is_payload(w)) {
                out = Tag::decode(w);
                return true;
            }
        }
    }

    CACHE_LINE_MEMBER(std::atomic<uint64_t>, head_, {0});
    CACHE_LINE_MEMBER(std::atomic<uint64_t>, tail_, {0});
    [[no_unique_address]] link_state link_{};
    const std::size_t capacity_;
    cell_type* const cells_;
    /// Which sentinel currently means empty. Written only by reopen(), read on every
    /// operation -- so it shares the read-mostly line rather than getting a padded one.
    bool gen_ = false;
};

} // namespace algo

/// @brief Capabilities of algo::HQ as a linked segment. Every field is mandatory:
/// core::segment_traits has no primary definition, so omitting one is a compile error.
template <typename T, typename Opt, typename Link, typename Tag>
struct core::segment_traits<algo::HQ<T, Opt, Link, Tag>> {
    static constexpr bool needs_close_hint = false;
    static constexpr bool needs_dequeue_prepare = false;
    /// A single atomic step publishes the item; nothing can be mid-insert.
    static constexpr bool needs_inflight_drain = false;
    static constexpr bool recyclable = true; ///< see HQ::reopen -- O(1) generation flip
    static constexpr bool can_store_null = Tag::can_store_null;
};

MPMC_ASSERT_SEGMENT_TRAITS(algo::HQ<int*, meta::EmptyOptions, linkage::Node<mem::PtrHandle>>);

namespace seg {
template <typename T, typename Opt = meta::EmptyOptions, typename HP = mem::PtrHandle>
/// Hybrid write-once array as a linked segment. Linked-only by construction.
using HQ = algo::HQ<T, Opt, linkage::Node<HP>>;
}
