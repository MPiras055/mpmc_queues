#pragma once
#include <cell/PlainCell.hpp>
#include <cell/Tagging.hpp>
#include <core/SegmentTraits.hpp>
#include <linkage/Linkage.hpp>
#include <mem/SingleBlock.hpp>
#include <meta/OptionsPack.hpp>
#include <util/specs.hpp>
#include <atomic>
#include <cassert>

namespace algo {

struct FAAArrayOpt {
    /** Pad each cell to a cache line. Off by default: the linear sweep means adjacent
     *  cells are rarely contended by the same pair of threads. */
    struct force_cell_padding {};
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
    requires meta::AcceptsOnly<Opt, typename FAAArrayOpt::force_cell_padding> && linkage::Linked<Link> && cell::Tagging<Tag, T>
class FAAArray : public mem::SingleBlock<FAAArray<T, Opt, Link, Tag>> {
    using Self = FAAArray<T, Opt, Link, Tag>;

    using word = typename Tag::word;

    static constexpr bool pad_cells = Opt::template has<typename FAAArrayOpt::force_cell_padding>;
    /// How long a consumer waits for a straggling producer before invalidating the slot.
    static constexpr std::size_t kPatience = 1024 << 2;

public:
    using tag_type = Tag;
    using cell_type = cell::PlainCell<word, pad_cells>;
    using link_state = typename Link::template state<Self>;
    using handle_type = typename link_state::handle;

    static constexpr auto plan(std::size_t n) noexcept {
        mem::LayoutBuilder b{sizeof(Self), alignof(Self)};
        mem::Plan<1> p{};
        p.regions[0] = b.add(n * sizeof(cell_type), alignof(cell_type));
        p.total = b.total();
        p.block_align = b.block_align();
        return p;
    }

    FAAArray(std::size_t n, mem::Blocks blk) noexcept
        : capacity_{n}, cells_{blk.template at<cell_type>(plan(n).regions[0])} {
        assert(n != 0 && "FAAArray: capacity must be non-null");
        for (std::size_t i = 0; i < n; ++i)
            cells_[i].val.store(empty_w(), std::memory_order_relaxed);
    }

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

    FORCE_INLINE bool enqueue(T item, bool /*closed_hint*/) noexcept { return enqueue(item); }

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

    std::size_t size() const noexcept {
        const uint64_t t = tail_.load(std::memory_order_acquire);
        const uint64_t h = head_.load(std::memory_order_acquire);
        const uint64_t capped = t > capacity_ ? capacity_ : t;
        return capped > h ? static_cast<std::size_t>(capped - h) : 0;
    }

    std::size_t capacity() const noexcept { return capacity_; }

    void close() noexcept
        requires(Link::is_linked)
    {
        tail_.store(capacity_, std::memory_order_release);
    }

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
     *
     * @debug: Implementation Hint
     * Each segment starts with all cells `empty` and after closure and full drain ends up
     * with all cells as `consumed`. The segment would have to store an additional flag which
     * simply flips the consumed cells and treats them as opened. Furthermore everytime enqueing
     * an item each thread should check for `empty/consumed` cells based on the flag, in this way
     * the reopen only has to flip the instance flag (can be done via CAS with no retry). This would
     * also need to rework a bit how the whole segment uses the flags, the most simple way is to have
     * a ternary check for each time a tag is evaluated or stored. The tag can be also benefit from not
     * relying on padding since is setted only once per segment lifetime. We only need one bit for encoding
     * and both enqueue and dequeue read from the capacity counter, so we could encode it as the LSB or MSB
     * depending on the data type, else we can use explicit encoding
     *
     * @debug: Implementation 02:
     * the reopen method has to be executed in isolation so no need to be MT-safe:
     * The reopen method has also to set the handle so that no successor is set, and i was
     * fixating on the interleaving while it's not needed
     */
    bool reopen() noexcept
        requires(Link::is_linked)
    {
        link_.unlink(); // a recycled segment must not carry a stale successor

        const uint64_t h = head_.exchange(0,std::memory_order_relaxed);
        const uint64_t t = tail_.exchange(0,std::memory_order_relaxed);


        if (h == 0 && t == 0);  //no cell used
        else if (h >= capacity_) {  //all cells used
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

    handle_type next() const noexcept
        requires(Link::is_linked)
    {
        return link_.next();
    }

    bool link_next(handle_type h) noexcept
        requires(Link::is_linked)
    {
        return link_.link_next(h);
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

    ALIGNED_CACHE std::atomic<uint64_t> head_{0};
    CACHE_PAD_TYPES(std::atomic<uint64_t>);
    ALIGNED_CACHE std::atomic<uint64_t> tail_{0};
    CACHE_PAD_TYPES(std::atomic<uint64_t>);
    [[no_unique_address]] link_state link_{};
    const std::size_t capacity_;
    cell_type* const cells_;
    /// Which sentinel currently means empty. Written only by reopen(), read on every
    /// operation -- so it shares the read-mostly line rather than getting a padded one.
    bool gen_ = false;
};

} // namespace algo

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
using FAAArray = algo::FAAArray<T, Opt, linkage::Node<HP>>;
}
