#pragma once
#include <core/SegmentTraits.hpp>
#include <linkage/Linkage.hpp>
#include <mem/SingleBlock.hpp>
#include <meta/OptionsPack.hpp>
#include <util/specs.hpp>
#include <cassert>
#include <mutex>

namespace algo {

/**
 * @brief A lock-based bounded ring. The baseline every lock-free queue is measured against.
 *
 * Deliberately the simplest correct implementation: one mutex, one ring. Its value is
 * as a control -- if a lock-free queue cannot beat this under the benchmark's
 * contention profile, the added complexity is not paying for itself.
 */
template <typename T, typename Opt = meta::EmptyOptions, typename Link = linkage::None>
class Mutex : public mem::SingleBlock<Mutex<T, Opt, Link>> {
    using Self = Mutex<T, Opt, Link>;

public:
    using cell_type = T;
    using link_state = typename Link::template state<Self>;
    using handle_type = typename link_state::handle;

    static constexpr auto plan(std::size_t n) noexcept {
        mem::LayoutBuilder b{sizeof(Self), alignof(Self)};
        mem::Plan<1> p{};
        p.regions[0] = b.add(n * sizeof(T), alignof(T));
        p.total = b.total();
        p.block_align = b.block_align();
        return p;
    }

    Mutex(std::size_t n, mem::Blocks blk) noexcept
        : capacity_{n}, cells_{blk.template at<T>(plan(n).regions[0])} {
        assert(n != 0 && "Mutex: capacity must be non-null");
    }

    bool enqueue(T item) noexcept {
        std::lock_guard<std::mutex> g(mu_);
        if (closed_) return false;
        if (count_ == capacity_) {
            // Full. A *linked* segment closes itself here, and the close is permanent: the
            // proxy reads a refusal as "link a successor", and from that moment this segment
            // must refuse every later item even once consumers have drained it and made room.
            //
            // Without that, a producer that read next() == nil just before some other
            // producer linked a successor goes on to enqueue into a segment the consumers
            // have since drained and unlinked -- the item lands somewhere nothing will ever
            // traverse, and is counted as enqueued. Every other linked segment closes on
            // full for this reason; see algo::Vyukov::enqueue.
            //
            // Standalone, the opposite is wanted: a bounded queue that has been drained must
            // accept items again, so the close is conditional on the linkage policy.
            if constexpr (Link::is_linked) closed_ = true;
            return false;
        }
        cells_[tail_] = item;
        tail_ = (tail_ + 1) % capacity_;
        ++count_;
        return true;
    }

    bool enqueue(T item, bool /*closed_hint*/) noexcept { return enqueue(item); }

    bool dequeue(T& out) noexcept {
        std::lock_guard<std::mutex> g(mu_);
        if (count_ == 0) return false;
        out = cells_[head_];
        head_ = (head_ + 1) % capacity_;
        --count_;
        return true;
    }

    std::size_t size() const noexcept {
        std::lock_guard<std::mutex> g(mu_);
        return count_;
    }

    std::size_t capacity() const noexcept { return capacity_; }

    void close() noexcept
        requires(Link::is_linked)
    {
        std::lock_guard<std::mutex> g(mu_);
        closed_ = true;
    }

    bool is_closed() const noexcept
        requires(Link::is_linked)
    {
        std::lock_guard<std::mutex> g(mu_);
        return closed_;
    }

    bool reopen() noexcept
        requires(Link::is_linked)
    {
        std::lock_guard<std::mutex> g(mu_);
        link_.unlink();
        head_ = tail_ = count_ = 0;
        closed_ = false;
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
    mutable std::mutex mu_;
    std::size_t head_ = 0, tail_ = 0, count_ = 0;
    bool closed_ = false;
    [[no_unique_address]] link_state link_{};
    const std::size_t capacity_;
    T* const cells_;
};

} // namespace algo

template <typename T, typename Opt, typename Link>
struct core::segment_traits<algo::Mutex<T, Opt, Link>> {
    static constexpr bool needs_close_hint = false;
    static constexpr bool needs_dequeue_prepare = false;
    /// A single atomic step publishes the item; nothing can be mid-insert.
    static constexpr bool needs_inflight_drain = false;
    static constexpr bool recyclable = true;
    static constexpr bool can_store_null = true;
};
MPMC_ASSERT_SEGMENT_TRAITS(algo::Mutex<int*, meta::EmptyOptions, linkage::None>);

namespace queue {
template <typename T, typename Opt = meta::EmptyOptions>
using Mutex = algo::Mutex<T, Opt, linkage::None>;
}

namespace seg {
template <typename T, typename Opt = meta::EmptyOptions, typename HP = mem::PtrHandle>
using Mutex = algo::Mutex<T, Opt, linkage::Node<HP>>;
}
