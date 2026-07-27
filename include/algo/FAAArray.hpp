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
 * is no CAS loop on the index and no ABA. The price is that the array is single-use: a
 * drained segment cannot be reset without rewriting every cell, which is why
 * `recyclable` is false (see reopen()).
 *
 * Obstruction-free: a consumer that has waited long enough will exchange `consumed` into
 * a cell a producer has not yet written, invalidating that slot. The producer detects
 * this and takes the next index.
 *
 * @note **Linked-only.** FAAArray writes each cell once and never returns it to `empty`, and its indices only
 * advance. Standalone that makes it single-use: the first fill/drain works and every
 * later enqueue is refused (measured: cycle 0 accepts 8, cycles 1 and 2 accept 0).
 * A proxy discards a drained segment instead of reusing it, which is the only way
 * the algorithm makes sense.
 *       The class template is constrained on linkage::Linked, so
 *       `algo::FAAArray<T, Opt, linkage::None>` is not a nameable type.
 *
 * @tparam Tag cell::Tagging policy. MsbTag by default, so a null payload is storable.
 */
template <typename T, typename Opt, typename Link,
          typename Tag = cell::MsbTag<T>>
    requires linkage::Linked<Link> && cell::Tagging<Tag, T>
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
            cells_[i].val.store(Tag::empty(), std::memory_order_relaxed);
    }

    FORCE_INLINE bool enqueue(T item) noexcept {
        for (;;) {
            const uint64_t t = tail_.fetch_add(1, std::memory_order_acq_rel);
            if (t >= capacity_) return false; // segment exhausted (and thereby closed)
            cell_type& c = cells_[t];
            word expected = Tag::empty();
            if (!Tag::is_empty(c.val.load(std::memory_order_acquire)))
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
                if (!Tag::is_empty(c.val.load(std::memory_order_acquire))) break;
            const word w = c.val.exchange(Tag::consumed(), std::memory_order_acq_rel);
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
     * @brief Cannot be reopened.
     * @return false, always.
     *
     * Cells are written once and never returned to `empty`, so resetting the indices
     * would expose a full array of `consumed` words. Doing this properly needs
     * alternating empty/consumed encodings keyed off a generation bit so a reset costs
     * O(1) rather than O(capacity).
     *
     * Previously this was `assert(false && "TODO")` — a debug-only runtime abort inside
     * open(). As a return value plus `segment_traits::recyclable == false` it is a fact
     * the pooled source can check before ever selecting this segment.
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
struct core::segment_traits<algo::FAAArray<T, Opt, Link, Tag>> {
    static constexpr bool needs_close_hint = false;
    static constexpr bool needs_dequeue_prepare = false;
    /// A single atomic step publishes the item; nothing can be mid-insert.
    static constexpr bool needs_inflight_drain = false;
    /// See FAAArray::reopen -- cells are write-once.
    static constexpr bool recyclable = false;
    static constexpr bool can_store_null = Tag::can_store_null;
};

namespace seg {
template <typename T, typename Opt = meta::EmptyOptions, typename HP = mem::PtrHandle>
using FAAArray = algo::FAAArray<T, Opt, linkage::Node<HP>>;
}
