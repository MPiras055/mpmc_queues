#pragma once
/**
 * @file FAAArray.hpp
 * @brief Write-once linear array claimed by fetch-add; reopened in O(1) by flipping which sentinel means empty.
 * @ingroup algo
 */

#include <cell/PlainCell.hpp>
#include <cell/Tagging.hpp>
#include <core/SegmentTraits.hpp>
#include <linkage/Linkage.hpp>
#include <mem/SingleBlock.hpp>
#include <meta/OptionsPack.hpp>
#include <util/align.hpp>
#include <util/bit.hpp>
#include <util/specs.hpp>
#include <atomic>
#include <cassert>

namespace algo {

struct FAAArrayOpt {
    /**
     * @brief Take the capacity exactly as asked instead of rounding up to a power of two.
     *
     * Purely a *sizing* choice here, not an indexing one: this structure walks its cells
     * linearly and closes when it runs out, so it never wraps and has no mask to speed up.
     * Rounding is still the default so that a capacity request means the same thing across
     * every algorithm and a cross-algorithm benchmark compares the same geometry.
     */
    struct no_pow2 {};

    /** Pad each cell to a cache line. Off by default: the linear sweep means adjacent
     *  cells are rarely contended by the same pair of threads. */
    struct force_cell_padding {};

    /**
     * @brief Loads a consumer spends waiting for a straggling producer before invalidating
     *        the slot.
     *
     * The invalidation is unconditional here -- the head has already fetch-added past this
     * index, so the cell is spent either way -- and this only decides how long to hope the
     * payload lands first. Each iteration is a single load, so large values buy little: a
     * producer that has not published within a few dozen cycles has been descheduled.
     */
    template <auto N> struct patience {};
};

/**
 * @brief A linear, single-pass array segment: fetch-add an index, write once, never reuse.
 *
 * Each cell is claimed by exactly one producer and one consumer via fetch-add, so there
 * is no CAS loop on the index and no ABA.
 *
 * Obstruction-free: a consumer that has waited long enough will exchange `consumed` into
 * a cell a producer has not yet written, invalidating that slot. The producer detects
 * this and takes the next index.
 *
 * ## Reuse without rewriting the array
 *
 * Cells are written once and never returned to `empty`, so a life ends with every cell
 * holding `consumed`. Rather than sweep the array to reset it, the segment carries a
 * generation flag that **swaps which of the two sentinel words means empty**: `consumed`
 * of generation *g* is a perfectly good `empty` for generation *g+1*. Reopening is then
 * one flag flip and two index stores -- O(1) rather than O(capacity) -- and the array is
 * already in the right state. See reopen().
 *
 * @note **Linked-only.** The indices only advance, so a standalone FAAArray is single-use:
 *       the first fill/drain works and every later enqueue is refused (measured: cycle 0
 *       accepts 8, cycles 1 and 2 accept 0). Recovering capacity means reopening, and
 *       reopening needs the segment quiescent and unlinked first -- which only a proxy
 *       over a recycling source can arrange. The class template is constrained on
 *       linkage::Linked, so `algo::FAAArray<T, Opt, linkage::None>` is not a nameable type.
 *
 * @tparam Tag cell::Tagging policy. MsbTag by default, so a null payload is storable.
 */
template <typename T, typename Opt, typename Link,
          typename Tag = cell::LowTag<T>>
    requires meta::AcceptsOnly<Opt, typename FAAArrayOpt::force_cell_padding,
                               typename FAAArrayOpt::no_pow2,
                               meta::ValueOption<FAAArrayOpt::patience>> &&
             linkage::Linked<Link> && cell::Tagging<Tag, T>
class FAAArray : public mem::SingleBlock<FAAArray<T, Opt, Link, Tag>> {
    using Self = FAAArray<T, Opt, Link, Tag>;

    using word = typename Tag::word;

    static constexpr bool pad_cells = Opt::template has<typename FAAArrayOpt::force_cell_padding>;
    /// Round the cell count up to a power of two; see FAAArrayOpt::no_pow2.
    static constexpr bool pow2 = !Opt::template has<typename FAAArrayOpt::no_pow2>;
    /// How long a consumer waits for a straggling producer before invalidating the slot.
    /// Cast rather than trusted: `get` returns the option's own type when one is present.
    static constexpr std::size_t kPatience =
        static_cast<std::size_t>(Opt::template get<FAAArrayOpt::patience, std::size_t{1024}>);

public:
    /// Loads a consumer spends on a straggling producer; see FAAArrayOpt::patience.
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
    static constexpr std::size_t round_size(std::size_t n) noexcept {
        const std::size_t f = n < 2 ? 2 : n;
        if constexpr (pow2) return bit::round_to_next_pow2(f);
        else return f;
    }

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

    FAAArray(std::size_t n, mem::Blocks blk) noexcept
        : capacity_{round_size(n)}, cells_{blk.template at<cell_type>(plan(n).regions[0])} {
        assert(n != 0 && "FAAArray: capacity must be non-null");
        for (std::size_t i = 0; i < n; ++i)
            cells_[i].val.store(empty_w(), std::memory_order_relaxed);
    }

    /// @brief Add an item.
    /// @return false if the queue is full, or closed.
    FORCE_INLINE bool enqueue(T item) noexcept {
        for (;;) {
            const uint64_t t = tail_.fetch_add(1, std::memory_order_acq_rel);
            if (t >= capacity_) return false; // segment exhausted (and thereby closed)
            cell_type& c = cells_[t];
            word expected = empty_w();
            if (!is_empty_w(c.val.load(std::memory_order_acquire)))
                continue; // a consumer already invalidated this slot
            if (c.val.compare_exchange_strong(expected, Tag::encode(item),
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
        for (;;) {
            const uint64_t h = head_.fetch_add(1, std::memory_order_acq_rel);
            if (h >= capacity_) return false;
            cell_type& c = cells_[h];
            // Give a straggling producer a bounded chance to publish.
            for (std::size_t i = 0; i < kPatience; ++i)
                if (!is_empty_w(c.val.load(std::memory_order_acquire))) break;
            const word w = c.val.exchange(consumed_w(), std::memory_order_acq_rel);
            if (Tag::is_payload(w)) {
                out = Tag::decode(w);
                return true;
            }
            // We exchanged over an empty cell: that slot is now invalid, take the next.
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
        tail_.store(capacity_, std::memory_order_release);
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
     * A life ends with every cell holding `consumed`. Sweeping the array back to `empty`
     * would cost O(capacity); instead the flag swaps which sentinel word *means* empty, so
     * the array the previous life left behind already reads as a fresh one.
     *
     * @pre The segment is quiescent: no producer or consumer is inside enqueue/dequeue.
     *      The proxy guarantees it -- it only reopens a segment it has just acquired and
     *      has not yet published, and a pooled source only hands one back after two epoch
     *      advances with no live pin.
     *
     * The flip alone is *not* enough, because the proxy reopens every segment it acquires
     * and only one of the three states it can be in is uniformly `consumed`:
     *
     * | state                       | how it arises                          | cost |
     * |-----------------------------|----------------------------------------|------|
     * | pristine (head = tail = 0)  | first hand-out of a pool slot          | O(1), nothing to do |
     * | fully drained (head >= cap) | the ordinary recycle                   | O(1), flip |
     * | partially used              | acquired, filled, lost the link race, `discard`ed | O(capacity) sweep |
     *
     * `head_ >= capacity_` is an exact test for "every cell is consumed": the head only
     * gets there by every index being fetch-added, and each of those exchanged `consumed`
     * into its cell. Flipping out of either of the other two states would turn live or
     * `empty` cells into `consumed` ones -- silently shrinking the segment to nothing, or
     * resurrecting a stale payload as a duplicate.
     *
     * @note Why quiescence is load-bearing rather than merely tidy. A producer that has
     *       already fetch-added an index holds a CAS whose `expected` is the *old*
     *       generation's empty -- which is the new generation's `consumed`. If such a
     *       producer could still be running, its CAS would succeed and write a payload into
     *       a cell of the reopened segment. It cannot, because reopen only ever happens
     *       under a recycling source and those do not return a segment until every reader
     *       and writer has left it.
     *
     * @note `gen_` is a plain bool. reopen runs single-threaded by the precondition above,
     *       and the segment is published afterwards by a release CAS (`link_next`) that
     *       every reader reaches through an acquire load, so the write is properly
     *       ordered without being atomic itself.
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
     * swap -- both words are non-payload in either arrangement -- so decoding is
     * unaffected. Going through the Tag predicates rather than comparing words keeps this
     * inside the cell::Tagging contract instead of assuming the sentinels are equality
     * tested.
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

/// @brief Capabilities of algo::FAAArray as a linked segment. Every field is mandatory:
/// core::segment_traits has no primary definition, so omitting one is a compile error.
template <typename T, typename Opt, typename Link, typename Tag>
struct core::segment_traits<algo::FAAArray<T, Opt, Link, Tag>> {
    static constexpr bool needs_close_hint = false;
    static constexpr bool needs_dequeue_prepare = false;
    /// A single atomic step publishes the item; nothing can be mid-insert.
    static constexpr bool needs_inflight_drain = false;
    /// See FAAArray::reopen -- the generation flip resets the array in O(1).
    static constexpr bool recyclable = true;
    static constexpr bool can_store_null = Tag::can_store_null;
};
MPMC_ASSERT_SEGMENT_TRAITS(algo::FAAArray<int*, meta::EmptyOptions, linkage::Node<mem::PtrHandle>>);

namespace seg {
template <typename T, typename Opt = meta::EmptyOptions, typename HP = mem::PtrHandle>
/// Write-once fetch-add array as a linked segment. Linked-only by construction.
using FAAArray = algo::FAAArray<T, Opt, linkage::Node<HP>>;
}
