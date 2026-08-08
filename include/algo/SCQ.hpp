#pragma once
#include <algo/LFring.hpp>
#include <core/SegmentTraits.hpp>
#include <linkage/Linkage.hpp>
#include <mem/SingleBlock.hpp>
#include <meta/OptionsPack.hpp>
#include <util/bit.hpp>
#include <util/specs.hpp>
#include <atomic>
#include <cassert>

namespace algo {

struct SCQOpt {
    struct no_cell_padding {};
};


/**
 * @debug: no need to check for inflight since in the case of linked policy
 * each segment is protected via hazard Pointer: REMOVE THE INFLIGHT GUARD
 * 
 * Also no need for LFring to implement block_dequeue just revert to the old closure
 * on enqueue. If a slot cannot be enqueued then place it back in the old ring (should be
 * always successful due of the capacity of the ring). Moreover the threshold is not a heuristic
 * but is a conservative upper bound on the number of empty dequeue given an enqueue. So LFring linearizes
 * dequeues thanks to the threshold. Make sure that no slot is ever lost, so when processing reopen, there's no
 * need in resetting both rings, just opening the one closed and resetting the threshold for for the free slots 
 * queue
 */



/**
 * @brief Indirect queue: two index rings plus a payload buffer.
 *
 * One ring holds the indices of free slots, the other the indices of occupied slots.
 * Enqueue pops a free index, writes the payload, pushes the index to the data ring;
 * dequeue does the reverse. Because only *indices* circulate, the payload is ordinary
 * memory: SCQ is the one implementation here that stores arbitrarily sized `T` rather
 * than a single word.
 *
 * The cost is that an insert is not one atomic step. A thread that dies between claiming
 * a slot and publishing it leaks that slot permanently.
 *
 * @note This is the layout that motivated N-region planning: **five** regions in one
 *       block -- two ring headers, their two cell arrays, and the payload. The previous
 *       version built this with a bump-allocator overload that mutated a running size
 *       and returned the next free address, which could not be checked. Here the plan is
 *       constexpr and mem::SingleBlock static_asserts that the regions do not overlap.
 */
template <typename T, typename Opt = meta::EmptyOptions, typename Link = linkage::None>
    requires meta::AcceptsOnly<Opt, typename SCQOpt::no_cell_padding>
class SCQ : public mem::SingleBlock<SCQ<T, Opt, Link>> {
    using Self = SCQ<T, Opt, Link>;


    /// The rings index into our buffer, so they never need a fullness pre-check.
    using RingOpt = typename meta::OptionsPack<typename LFringOpt::indirect_store>::
        template add_if<Opt::template has<typename SCQOpt::no_cell_padding>,
                        typename LFringOpt::no_cell_padding>;

public:
    using Ring = algo::LFring<RingOpt, linkage::None>;
    using ring_cell = typename Ring::cell_type;
    using link_state = typename Link::template state<Self>;
    using handle_type = typename link_state::handle;

    static constexpr std::size_t round_size(std::size_t n) noexcept {
        return bit::round_to_next_pow2(n < 2 ? 2 : n);
    }

    static constexpr auto plan(std::size_t n) noexcept {
        const std::size_t cap = round_size(n);
        const std::size_t order = Ring::order_for(cap);
        const std::size_t rcells = Ring::cells_for(order);

        mem::LayoutBuilder b{sizeof(Self), alignof(Self)};
        mem::Plan<5> p{};
        p.regions[0] = b.add(sizeof(Ring), alignof(Ring));                  // free-ring header
        p.regions[1] = b.add(rcells * sizeof(ring_cell), alignof(ring_cell)); // its cells
        p.regions[2] = b.add(sizeof(Ring), alignof(Ring));                  // data-ring header
        p.regions[3] = b.add(rcells * sizeof(ring_cell), alignof(ring_cell)); // its cells
        p.regions[4] = b.add(cap * sizeof(T), alignof(T));                  // payload
        p.total = b.total();
        p.block_align = b.block_align();
        return p;
    }

    SCQ(std::size_t n, mem::Blocks blk) noexcept : capacity_{round_size(n)} {
        const auto p = plan(n);
        const std::size_t order = Ring::order_for(capacity_);
        // Free ring starts holding every index; data ring starts empty.
        free_ = ::new (blk.template at<void>(p.regions[0]))
            Ring(order, blk.template at<ring_cell>(p.regions[1]), true);
        data_ = ::new (blk.template at<void>(p.regions[2]))
            Ring(order, blk.template at<ring_cell>(p.regions[3]), false);
        buffer_ = blk.template at<T>(p.regions[4]);
    }

    ~SCQ() {
        free_->~Ring();
        data_->~Ring();
    }
    
    FORCE_INLINE bool enqueue(T item) noexcept {
        std::size_t slot = 0;
        if (!free_->dequeue(slot)) {
            if constexpr (Link::is_linked) close();
            return false;
        }
        buffer_[slot] = item;
        const bool ok = data_->enqueue(slot);
        if (!ok) {
            // Hand the slot back rather than losing it for the rest of this segment's life.
            // Kept outside the assert: an assert argument disappears under NDEBUG, so putting
            // the enqueue inside one would silently drop the slot in a release build.
            const bool returned = free_->enqueue(slot);
            assert(returned && "SCQ: free ring refused a slot it must have had room for");
            (void)returned;
        }
        return ok;
    }

    FORCE_INLINE bool enqueue(T item, bool closed_hint) noexcept {
        if constexpr (Link::is_linked) {
            if (closed_hint && is_closed()) return false;
        }
        return enqueue(item);
    }

    FORCE_INLINE bool dequeue(T& out) noexcept {
        std::size_t slot = 0;
        if (!data_->dequeue(slot)) return false;
        out = buffer_[slot];
        // Must succeed even on a closed segment: block_dequeue() leaves enqueue open
        // precisely so a consumer can always hand the slot back.
        const bool returned = free_->enqueue(slot);
        assert(returned && "SCQ: free ring refused a slot being returned");
        (void)returned;
        return true;
    }

    std::size_t size() const noexcept { return data_->size(); }
    std::size_t capacity() const noexcept { return capacity_; }

    /// See LFring::reset_threshold -- disarm the empty shortcut before the final drain.
    void prepare_dequeue_after_link() noexcept { data_->reset_threshold(); }

    void close() noexcept
        requires(Link::is_linked)
    {
        // Blocks slot acquisition only; consumers must still be able to return slots.
        data_->close();
    }

    bool is_closed() const noexcept
        requires(Link::is_linked)
    {
        return data_->is_closed();
    }

    bool reopen() noexcept
        requires(Link::is_linked)
    {
        link_.unlink();
        // Both rings are rebuilt from scratch rather than just re-flagged.
        //
        // A segment is retired once dequeue reports empty twice, but LFring's empty
        // answer comes from a threshold heuristic, not a count: it can say empty while
        // indices remain queued. Under an allocating source that only leaks -- the
        // segment is destroyed and the strays go with it. Under a pooled source the
        // segment comes back, and those stray indices are handed out again, so the same
        // buffer slot is dequeued twice. Measured before this fix: 20004 items consumed
        // against 20000 produced.
        free_->reset(/*init_full=*/true);  // every slot available again
        data_->reset(/*init_full=*/false); // nothing queued
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
    /// Decrements the in-flight count however enqueue() leaves.
    // struct InflightGuard {
    //     std::atomic<uint32_t>& counter;
    //     ~InflightGuard() { counter.fetch_sub(1, std::memory_order_release); }
    // };

    [[no_unique_address]] link_state link_{};
    const std::size_t capacity_;
    Ring* free_ = nullptr;
    Ring* data_ = nullptr;
    T* buffer_ = nullptr;
    // ALIGNED_CACHE std::atomic<uint32_t> inflight_{0};
    // CACHE_PAD_TYPES(std::atomic<uint32_t>);
};

} // namespace algo

template <typename T, typename Opt, typename Link>
struct core::segment_traits<algo::SCQ<T, Opt, Link>> {
    /// Re-entering enqueue on a closed SCQ burns free-ring work for nothing.
    static constexpr bool needs_close_hint = true;
    /// See SCQ::prepare_dequeue_after_link.
    static constexpr bool needs_dequeue_prepare = true;
    /// Its insert is not atomic; see SCQ::has_inflight.
    static constexpr bool needs_inflight_drain = false;
    static constexpr bool recyclable = true;
    /// The payload is ordinary memory; nothing is stolen from its value space.
    static constexpr bool can_store_null = true;
};
MPMC_ASSERT_SEGMENT_TRAITS(algo::SCQ<int*, meta::EmptyOptions, linkage::None>);

namespace queue {
template <typename T, typename Opt = meta::EmptyOptions>
using SCQ = algo::SCQ<T, Opt, linkage::None>;
}

namespace seg {
template <typename T, typename Opt = meta::EmptyOptions, typename HP = mem::PtrHandle>
using SCQ = algo::SCQ<T, Opt, linkage::Node<HP>>;
}
