#pragma once
/**
 * @file Mutex.hpp
 * @brief Lock-based bounded ring. The control every lock-free algorithm here is measured against.
 * @ingroup algo
 */

#include <core/SegmentTraits.hpp>
#include <linkage/Linkage.hpp>
#include <mem/SingleBlock.hpp>
#include <meta/OptionsPack.hpp>
#include <util/bit.hpp>
#include <util/specs.hpp>
#include <cassert>
#include <condition_variable>
#include <mutex>

namespace algo {

/** @brief Compile-time configuration for the lock-based ring. */
struct MutexOpt {
    /**
     * @brief Wake every waiter instead of one.
     *
     * `notify_one` is the default because a single enqueue creates work for exactly one
     * consumer: waking the rest only to have them find the queue empty again is the thundering
     * herd the two separate condition variables exist to avoid. `notify_all` is here because it
     * is the obvious thing to compare against, and the difference is one of the few genuinely
     * interesting knobs on a lock-based queue.
     *
     * A close() always notifies everybody regardless -- see there.
     */
    struct notify_all {};

    /**
     * @brief Take the capacity exactly as asked instead of rounding up to a power of two.
     *
     * Rounding is the default so that a capacity request means the same thing across every
     * algorithm and a cross-algorithm benchmark compares the same geometry. It also lets the
     * wrap be a mask rather than a `%` -- a small win, though on a queue that takes a mutex per
     * operation it is not the interesting cost.
     */
    struct no_pow2 {};
};

/**
 * @brief A blocking bounded ring behind one mutex and two condition variables.
 *
 * The control every lock-free queue here is measured against, and deliberately the simplest
 * correct implementation of one: if a lock-free ring cannot beat this under the benchmark's
 * contention profile, the added complexity is not paying for itself.
 *
 * **The waits are unbounded, and `close()` is what ends them.** A producer parks until there is
 * room, a consumer parks until there is an item, and both are released by a close -- which sets
 * the flag under the lock, because it is part of both predicates, and then notifies *all*
 * waiters on both variables. After a close the queue still drains: `dequeue()` degrades to the
 * semantics of `try_dequeue()` and hands back whatever is left.
 *
 * That contract is why the blocking pair is spelled separately from the non-blocking one:
 *
 * | | blocks | for |
 * | --- | --- | --- |
 * | `enqueue` / `dequeue` | until room, an item, or a close | callers that own the queue's lifetime |
 * | `try_enqueue` / `try_dequeue` | never | generic code that drains without closing |
 *
 * Every other algorithm here implements both as the same function, since none of them block.
 *
 * @note Blocking applies only to the **standalone** queue. As a linked segment the blocking
 *       operations forward to the try-versions, because `LinkedProxy` reads a refusal on full as
 *       "link a successor" -- parking there would wait for room this segment is never going to
 *       have, and the traversal would stall behind it.
 */
template <typename T, typename Opt = meta::EmptyOptions, typename Link = linkage::None>
    requires meta::AcceptsOnly<Opt, typename MutexOpt::no_pow2, typename MutexOpt::notify_all>
class Mutex : public mem::SingleBlock<Mutex<T, Opt, Link>> {
    using Self = Mutex<T, Opt, Link>;

    /// Wrap is a mask rather than a modulo; see MutexOpt::no_pow2.
    static constexpr bool pow2 = !Opt::template has<typename MutexOpt::no_pow2>;

    /// @see MutexOpt::notify_all
    static constexpr bool wake_all = Opt::template has<typename MutexOpt::notify_all>;

    /**
     * @brief Whether the blocking pair actually blocks.
     *
     * Standalone only; see the class note for why a linked segment must refuse instantly.
     */
    static constexpr bool blocks = !Link::is_linked;

public:
    /// @name Tuning in effect
    /// @{
    static constexpr bool notify_all_policy = wake_all;
    /// False for a linked segment, where enqueue/dequeue forward to the try-versions.
    static constexpr bool parks_when_blocked = blocks;
    /// @}

    using cell_type = T;
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
        p.regions[0] = b.add(round_size(n) * sizeof(T), alignof(T));
        p.total = b.total();
        p.block_align = b.block_align();
        return p;
    }

    Mutex(std::size_t n, mem::Blocks blk) noexcept
        : capacity_{round_size(n)}, cells_{blk.template at<T>(plan(n).regions[0])} {
        assert(n != 0 && "Mutex: capacity must be non-null");
    }

    /**
     * @brief Add an item, blocking until there is room or the queue is closed.
     * @return false only if the queue is closed; a full queue is waited on, not refused.
     */
    bool enqueue(T item) noexcept {
        if constexpr (!blocks) return try_enqueue(item);
        else {
            std::unique_lock<std::mutex> lock(mu_);
            not_full_.wait(lock, [this] { return tail_ - head_ < capacity_ || closed_; });
            if (closed_) return false;
            put(item);
            lock.unlock();   // signal outside the lock: a woken thread would only block on it
            wake(not_empty_);
            return true;
        }
    }

    /**
     * @brief Add an item without ever blocking.
     * @return false if the queue is full, or closed.
     *
     * What generic code and LinkedProxy call. A linked segment closes itself on full here, and
     * the close is permanent: the proxy reads the refusal as "link a successor", and from that
     * moment this segment must refuse every later item even once consumers have drained it and
     * made room. Otherwise a producer that read next() == nil just before some other producer
     * linked a successor goes on to enqueue into a segment the consumers have since drained and
     * unlinked -- the item lands somewhere nothing will ever traverse, and is counted as
     * enqueued. Standalone, the opposite is wanted: a drained queue must accept again, so the
     * close is conditional on the linkage.
     */
    bool try_enqueue(T item) noexcept {
        std::unique_lock<std::mutex> lock(mu_);
        if (closed_) return false;
        if (tail_ - head_ == capacity_) {
            if constexpr (Link::is_linked) closed_ = true;
            return false;
        }
        put(item);
        if constexpr (blocks) {
            lock.unlock();
            wake(not_empty_);
        }
        return true;
    }

    /// @brief Add an item, skipping the attempt when the caller already knows it is closed.
    /// @param closed_hint the caller believes this segment is closed; see
    ///        core::segment_traits::needs_close_hint.
    bool enqueue(T item, bool /*closed_hint*/) noexcept { return try_enqueue(item); }

    /**
     * @brief Take the oldest item, blocking until one arrives or the queue is closed.
     * @return false only once the queue is both closed and empty.
     *
     * A closed queue still drains: past the wait this behaves exactly as try_dequeue(), which is
     * what lets a caller close and then collect what is left.
     */
    bool dequeue(T& out) noexcept {
        if constexpr (!blocks) return try_dequeue(out);
        else {
            std::unique_lock<std::mutex> lock(mu_);
            not_empty_.wait(lock, [this] { return tail_ != head_ || closed_; });
            if (tail_ == head_) return false;   // closed and drained
            take(out);
            lock.unlock();
            wake(not_full_);
            return true;
        }
    }

    /// @brief Take the oldest item without ever blocking.
    /// @return false if the queue is empty.
    bool try_dequeue(T& out) noexcept {
        std::unique_lock<std::mutex> lock(mu_);
        if (tail_ == head_) return false;
        take(out);
        if constexpr (blocks) {
            lock.unlock();
            wake(not_full_);
        }
        return true;
    }

    /// @return Items currently held. Approximate under concurrency, exact when quiescent.
    std::size_t size() const noexcept {
        std::lock_guard<std::mutex> g(mu_);
        return static_cast<std::size_t>(tail_ - head_);
    }

    /// @return Items this queue can hold.
    std::size_t capacity() const noexcept { return capacity_; }

    /**
     * @brief Refuse all further enqueues, permanently, and release everyone parked.
     *
     * Not restricted to the linked configuration: this is how a *standalone* blocking queue
     * terminates. `dequeue()` waits for an item or a close, so without this a consumer on an
     * empty queue would park for ever -- the owner closes when production is done, and the
     * parked consumers wake, drain what is left, and get `false`.
     *
     * The flag is set **under the lock** because it is part of both predicates; setting it
     * outside would be a race with a waiter evaluating them. The notify happens after the lock
     * is dropped, so a woken thread does not immediately block on it, and it is `notify_all`
     * regardless of MutexOpt::notify_all -- a close concerns every waiter, not one of them.
     *
     * Idempotent: closing twice is a no-op beyond a second round of notifications.
     */
    void close() noexcept {
        {
            std::lock_guard<std::mutex> g(mu_);
            if (closed_) return;
            closed_ = true;
        }
        not_full_.notify_all();
        not_empty_.notify_all();
    }

    /// @return true once closed; a closed queue still drains.
    bool is_closed() const noexcept {
        std::lock_guard<std::mutex> g(mu_);
        return closed_;
    }

    bool reopen() noexcept
        requires(Link::is_linked)
    {
        std::lock_guard<std::mutex> g(mu_);
        link_.unlink();
        head_ = tail_ = 0;
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
    /// @pre the lock is held. Split out so the blocking and try- forms share one body.
    void put(T item) noexcept {
        cells_[wrap(tail_)] = item;
        ++tail_;
    }

    /// @pre the lock is held, and the queue is not empty.
    void take(T& out) noexcept {
        out = cells_[wrap(head_)];
        ++head_;
    }

    /// @see MutexOpt::notify_all
    static void wake(std::condition_variable& cv) noexcept {
        if constexpr (wake_all) cv.notify_all();
        else cv.notify_one();
    }

    FORCE_INLINE std::size_t wrap(std::size_t i) const noexcept {
        if constexpr (pow2) return i & (capacity_ - 1);
        else return i % capacity_;
    }

    mutable std::mutex mu_;
    /// Two, so a producer waking for room does not also wake every consumer waiting for work.
    /// One mutex guards both, which is what keeps the predicates consistent.
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    /// Monotonic, never wrapped: occupancy is `tail_ - head_` and the index is wrap()ped at the
    /// point of use. One fewer piece of state to keep consistent than a separate count.
    std::size_t head_ = 0, tail_ = 0;
    bool closed_ = false;
    [[no_unique_address]] link_state link_{};
    const std::size_t capacity_;
    T* const cells_;
};

} // namespace algo

/// @brief Capabilities of algo::Mutex as a linked segment. Every field is mandatory:
/// core::segment_traits has no primary definition, so omitting one is a compile error.
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
/// Standalone lock-based ring. The control the lock-free algorithms are measured against.
using Mutex = algo::Mutex<T, Opt, linkage::None>;
}

namespace seg {
template <typename T, typename Opt = meta::EmptyOptions, typename HP = mem::PtrHandle>
/// The lock-based ring as a linked segment.
using Mutex = algo::Mutex<T, Opt, linkage::Node<HP>>;
}
