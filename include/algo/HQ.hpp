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

struct HQOpt {
    struct force_cell_padding {};
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
 * This is the segment the README describes as trading throughput for a better memory
 * footprint. It had been dead since the interface moved out from under it.
 *
 * @note **Linked-only.** HQ has FAAArray's write-once cells, plus a dequeue that picks its strategy from whether
 * a successor exists. Standalone it is both single-use and permanently on the slow path.
 *       The class template is constrained on linkage::Linked, so
 *       `algo::HQ<T, Opt, linkage::None>` is not a nameable type.
 *
 * @tparam Tag cell::Tagging policy. LowTag by default, matching the original encoding
 *             (0 = empty, 1 = consumed) -- which means null payloads are unstorable.
 *             Pass cell::MsbTag<T> if null must round-trip.
 */
template <typename T, typename Opt, typename Link,
          typename Tag = cell::LowTag<T>>
    requires meta::AcceptsOnly<Opt, typename HQOpt::force_cell_padding> && linkage::Linked<Link> && cell::Tagging<Tag, T>
class HQ : public mem::SingleBlock<HQ<T, Opt, Link, Tag>> {
    using Self = HQ<T, Opt, Link, Tag>;

    using word = typename Tag::word;

    static constexpr bool pad_cells = Opt::template has<typename HQOpt::force_cell_padding>;
    static constexpr std::size_t kPatience = 4 * 1024;

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

    HQ(std::size_t n, mem::Blocks blk) noexcept
        : capacity_{n}, cells_{blk.template at<cell_type>(plan(n).regions[0])} {
        assert(n != 0 && "HQ: capacity must be non-null");
        for (std::size_t i = 0; i < n; ++i)
            cells_[i].val.store(Tag::empty(), std::memory_order_relaxed);
    }

    FORCE_INLINE bool enqueue(T item) noexcept {
        assert((Tag::can_store_null || Tag::is_payload(Tag::encode(item))) &&
               "HQ: this tagging policy cannot store that value (see can_store_null)");
        for (;;) {
            const uint64_t t = tail_.fetch_add(1, std::memory_order_acq_rel);
            if (t >= capacity_) return false;
            // NB: `expected` is reloaded every iteration. The original hoisted it out of
            // the loop, so one failed CAS left it holding the observed word and every
            // later attempt compared against that stale value instead of `empty`.
            word expected = Tag::empty();
            if (cells_[t].val.compare_exchange_strong(expected, Tag::encode(item),
                                                      std::memory_order_acq_rel,
                                                      std::memory_order_acquire))
                return true;
        }
    }

    FORCE_INLINE bool enqueue(T item, bool /*closed_hint*/) noexcept { return enqueue(item); }

    FORCE_INLINE bool dequeue(T& out) noexcept {
        return has_successor() ? fast_dequeue(out) : slow_dequeue(out);
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
        tail_.fetch_add(capacity_, std::memory_order_release);
    }

    bool is_closed() const noexcept
        requires(Link::is_linked)
    {
        return tail_.load(std::memory_order_acquire) >= capacity_;
    }

    /**
     * @brief Cannot be reopened. @return false, always.
     *
     * Cells are never returned to `empty`, so resetting the indices would present an
     * array full of `consumed`. The original open() stored 0 into head and tail and
     * returned true, which left exactly that state: every subsequent enqueue CAS failed
     * and the segment behaved as instantly full.
     */
    bool reopen() noexcept
        requires(Link::is_linked)
    {
        return false;
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
            for (std::size_t i = 0; i < kPatience; ++i)
                if (!Tag::is_empty(c.val.load(std::memory_order_acquire))) break;
            const word w = c.val.exchange(Tag::consumed(), std::memory_order_acq_rel);
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

            if (Tag::is_consumed(w)) { // already taken; help head along
                (void)head_.compare_exchange_weak(h, h + 1, std::memory_order_relaxed);
                continue;
            }

            if (Tag::is_empty(w)) { // producer has not published yet
                bool resolved = false;
                for (std::size_t i = 0; i < kPatience; ++i) {
                    w = c.val.load(std::memory_order_acquire);
                    if (Tag::is_consumed(w)) break;
                    if (!Tag::is_empty(w)) { resolved = true; break; }
                }
                if (!resolved && Tag::is_consumed(w)) {
                    (void)head_.compare_exchange_weak(h, h + 1, std::memory_order_relaxed);
                    continue;
                }
            }

            // Patience exhausted or a payload is present: claim the cell. This races
            // with the producer and may invalidate the slot -- the obstruction-free part.
            w = c.val.exchange(Tag::consumed(), std::memory_order_acq_rel);
            (void)head_.compare_exchange_weak(h, h + 1, std::memory_order_relaxed);
            if (Tag::is_payload(w)) {
                out = Tag::decode(w);
                return true;
            }
        }
    }

    ALIGNED_CACHE std::atomic<uint64_t> head_{0};
    CACHE_PAD_TYPES(std::atomic<uint64_t>);
    ALIGNED_CACHE std::atomic<uint64_t> tail_{0};
    CACHE_PAD_TYPES(std::atomic<uint64_t>);
    [[no_unique_address]] link_state link_{};
    const std::size_t capacity_;
    cell_type* const cells_;
};

} // namespace algo

template <typename T, typename Opt, typename Link, typename Tag>
struct core::segment_traits<algo::HQ<T, Opt, Link, Tag>> {
    static constexpr bool needs_close_hint = false;
    static constexpr bool needs_dequeue_prepare = false;
    /// A single atomic step publishes the item; nothing can be mid-insert.
    static constexpr bool needs_inflight_drain = false;
    static constexpr bool recyclable = false; ///< see HQ::reopen
    static constexpr bool can_store_null = Tag::can_store_null;
};
MPMC_ASSERT_SEGMENT_TRAITS(algo::HQ<int*, meta::EmptyOptions, linkage::Node<mem::PtrHandle>>);

namespace seg {
template <typename T, typename Opt = meta::EmptyOptions, typename HP = mem::PtrHandle>
using HQ = algo::HQ<T, Opt, linkage::Node<HP>>;
}
