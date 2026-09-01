#pragma once
/**
 * @file Spin.hpp
 * @brief Bounded ring behind a spinlock that parks on `std::atomic::wait`.
 * @ingroup algo
 */

#include "util/align.hpp"
#include <core/SegmentTraits.hpp>
#include <linkage/Linkage.hpp>
#include <mem/SingleBlock.hpp>
#include <meta/OptionsPack.hpp>
#include <util/bit.hpp>
#include <util/specs.hpp>
#include <atomic>
#include <cassert>
#include <cstdint>

namespace algo {

/** @brief Compile-time configuration for the spinlock-based ring. */
struct SpinOpt {
    /**
     * @brief Take the capacity exactly as asked instead of rounding up to a power of two.
     *
     * Rounding is the default so that a capacity request means the same thing across every
     * algorithm and a cross-algorithm benchmark compares the same geometry.
     */
    struct no_pow2 {};

    /**
     * @brief How many times a contending thread spins before parking on the atomic.
     *
     * The whole point of the lock: a critical section here is a handful of instructions, so a
     * thread that spins briefly usually acquires without ever entering the kernel. Past that,
     * spinning is worse than sleeping -- it burns a core that the lock holder may need.
     * Zero parks immediately, which turns this into a pure futex and is the interesting
     * comparison against algo::Mutex.
     * @note value option: `SpinOpt::spins_before_park<0>`
     */
    template <auto N> struct spins_before_park {};
};

/**
 * @brief A bounded ring guarded by a spinlock, as a second lock-based control.
 *
 * algo::Mutex answers "what does a `std::mutex` cost", but it answers it together with the cost
 * of the condition-variable wait it now performs when full or empty, and the two are hard to
 * separate from the outside. This is the other half of that measurement: the same ring and the
 * same critical sections, with a lock that never waits for the *queue* to change state -- only
 * for the lock itself. enqueue() and dequeue() refuse immediately when full or empty, so
 * whatever this is slower or faster by is attributable to lock acquisition alone.
 *
 * The lock is the usual three-state futex rather than a bare `while (exchange)`:
 *
 * ```
 *   0  free
 *   1  held, nobody waiting        -> unlock is a plain store, no syscall
 *   2  held, at least one waiter   -> unlock must notify
 * ```
 *
 * The distinction is what keeps the uncontended path free of a `notify_one()`, which would
 * otherwise be paid on every single unlock for the rare case that somebody is parked.
 *
 * `std::atomic::wait`/`notify_one` are C++20 and map onto the platform's futex where there is
 * one, so a parked thread costs nothing while it waits -- unlike a spin, which is why the
 * spin count is bounded.
 */
template <typename T, typename Opt = meta::EmptyOptions, typename Link = linkage::None>
    requires meta::AcceptsOnly<Opt, typename SpinOpt::no_pow2,
                               meta::ValueOption<SpinOpt::spins_before_park>>
class Spin : public mem::SingleBlock<Spin<T, Opt, Link>> {
    using Self = Spin<T, Opt, Link>;

    /// Wrap is a mask rather than a modulo; see SpinOpt::no_pow2.
    static constexpr bool pow2 = !Opt::template has<typename SpinOpt::no_pow2>;

    static constexpr uint32_t kSpins =
        static_cast<uint32_t>(Opt::template get<SpinOpt::spins_before_park, uint32_t{64}>);

    /// @name Lock states; see the class note.
    /// @{
    static constexpr uint32_t kFree = 0;
    static constexpr uint32_t kHeld = 1;
    static constexpr uint32_t kContended = 2;
    /// @}

public:
    using cell_type = T;
    using link_state = typename Link::template state<Self>;
    using handle_type = typename link_state::handle;

    /// @name Tuning in effect
    /// @{
    static constexpr uint32_t spins_before_park = kSpins;
    /// @}

    static constexpr std::size_t round_size(std::size_t n) noexcept {
        const std::size_t f = n < 2 ? 2 : n;
        if constexpr (pow2) return bit::round_to_next_pow2(f);
        else return f;
    }

    /// @return the capacity a segment built with @p n will report.
    static constexpr std::size_t capacity_for(std::size_t n) noexcept { return round_size(n); }

    /// @brief Where the co-allocated regions go. See @ref block-construction.
    /// @param n requested capacity; the only thing the layout may depend on.
    static constexpr auto plan(std::size_t n) noexcept {
        mem::LayoutBuilder b{sizeof(Self), alignof(Self)};
        mem::Plan<1> p{};
        p.regions[0] = b.add(round_size(n) * sizeof(T), alignof(T));
        p.total = b.total();
        p.block_align = b.block_align();
        return p;
    }

    Spin(std::size_t n, mem::Blocks blk) noexcept
        : capacity_{round_size(n)}, cells_{blk.template at<T>(plan(n).regions[0])} {
        assert(n != 0 && "Spin: capacity must be non-null");
    }

    Spin(const Spin&) = delete;
    Spin& operator=(const Spin&) = delete;

    /// @brief Add an item.
    /// @return false if the queue is full, or closed.
    bool try_enqueue(T item) noexcept {
        const Guard g{*this};
        if (closed_) return false;
        if (count_ == capacity_) {
            // A linked segment closes itself here and the close is permanent; see
            // algo::Mutex::enqueue for why a segment that refuses once must refuse for ever.
            // Standalone, a drained queue has to accept again, so the close is conditional.
            if constexpr (Link::is_linked) closed_ = true;
            return false;
        }
        cells_[tail_] = item;
        tail_ = wrap(tail_ + 1);
        ++count_;
        return true;
    }

    /// @brief Add an item, skipping the attempt when the caller already knows it is closed.
    /// @param closed_hint the caller believes this segment is closed; see
    ///        core::segment_traits::needs_close_hint.
    /// @return false if the queue is full, or closed.
    bool enqueue(T item, bool /*closed_hint*/) noexcept { return try_enqueue(item); }
    bool enqueue(T item) noexcept {return try_enqueue(item);}

    /// @brief Take the oldest item.
    /// @return false if the queue is empty. Never waits for one to arrive -- the lock is the
    ///         only thing this algorithm ever blocks on.
    bool try_dequeue(T& out) noexcept {
        const Guard g{*this};
        if (count_ == 0) return false;
        out = cells_[head_];
        head_ = wrap(head_ + 1);
        --count_;
        return true;
    }

    bool dequeue(T& out) noexcept {return try_dequeue(out);}

    /// @return Items currently held. Approximate under concurrency, exact when quiescent.
    std::size_t size() const noexcept {
        const Guard g{*this};
        return count_;
    }

    /// @return Items this queue can hold.
    std::size_t capacity() const noexcept { return capacity_; }

    // -- linkage (only when linked) -----------------------------------------

    /// @brief Refuse all further enqueues, permanently.
    void close() noexcept
        requires(Link::is_linked)
    {
        const Guard g{*this};
        closed_ = true;
    }

    /// @return true once closed; a closed segment still drains.
    bool is_closed() const noexcept
        requires(Link::is_linked)
    {
        const Guard g{*this};
        return closed_;
    }

    /**
     * @brief Reset a drained segment for reuse.
     * @warning Not MT-safe; the segment must already be drained and unreachable.
     */
    bool reopen() noexcept
        requires(Link::is_linked)
    {
        const Guard g{*this};
        link_.unlink();
        head_ = tail_ = count_ = 0;
        closed_ = false;
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
     * @brief Bounded spin, then park. See the class note for the three states.
     *
     * `exchange(kContended)` rather than a CAS on the slow path: the thread is about to sleep
     * anyway, so it can afford to claim the contended state unconditionally, and doing so means
     * a lock that was actually free is still acquired by the same instruction.
     */
    void acquire() const noexcept {
        for (uint32_t i = 0; i < kSpins; ++i) {
            uint32_t expect = kFree;
            if (lock_.compare_exchange_weak(expect, kHeld, std::memory_order_acquire,
                                            std::memory_order_relaxed))
                return;
            SPIN_HINT();
        }
        while (lock_.exchange(kContended, std::memory_order_acquire) != kFree)
            lock_.wait(kContended, std::memory_order_relaxed);
    }

    void release() const noexcept {
        // Only the contended state owes a wakeup, which is the whole reason for having three
        // states rather than two.
        if (lock_.exchange(kFree, std::memory_order_release) == kContended)
            lock_.notify_one();
    }

    /// Scoped lock, so an early `return` inside a critical section cannot leak it.
    struct Guard {
        const Spin& q;
        explicit Guard(const Spin& s) noexcept : q{s} { q.acquire(); }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
        ~Guard() { q.release(); }
    };

    FORCE_INLINE std::size_t wrap(std::size_t i) const noexcept {
        if constexpr (pow2) return i & (capacity_ - 1);
        else return i % capacity_;
    }

    CACHE_ALIGN mutable std::atomic<uint32_t> lock_{kFree};
    CACHE_PAD(std::atomic<uint32_t>);
    std::size_t head_ = 0, tail_ = 0, count_ = 0;
    bool closed_ = false;
    [[no_unique_address]] link_state link_{};
    const std::size_t capacity_;
    T* const cells_;
};

} // namespace algo

/// @brief Capabilities of algo::Spin as a linked segment. Every field is mandatory:
/// core::segment_traits has no primary definition, so omitting one is a compile error.
template <typename T, typename Opt, typename Link>
struct core::segment_traits<algo::Spin<T, Opt, Link>> {
    static constexpr bool needs_close_hint = false;
    static constexpr bool needs_dequeue_prepare = false;
    /// The whole insert happens under the lock; nothing can be observed mid-insert.
    static constexpr bool needs_inflight_drain = false;
    static constexpr bool recyclable = true;
    static constexpr bool can_store_null = true;
};
MPMC_ASSERT_SEGMENT_TRAITS(algo::Spin<int*, meta::EmptyOptions, linkage::None>);

namespace queue {
template <typename T, typename Opt = meta::EmptyOptions>
/// Standalone spinlock-guarded ring; the second lock-based control.
using Spin = algo::Spin<T, Opt, linkage::None>;
}

namespace seg {
template <typename T, typename Opt = meta::EmptyOptions, typename HP = mem::PtrHandle>
/// The spinlock-guarded ring as a linked segment.
using Spin = algo::Spin<T, Opt, linkage::Node<HP>>;
}
