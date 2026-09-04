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
 * **One word holds both the lock and the closed flag.**
 *
 * ```
 *   bit  0   locked
 *   bit 31   closed          (kClosed, the MSB)
 * ```
 *
 * There is deliberately no third "contended" state. Tracking it would let an uncontended
 * release skip its `notify_one()`, but the critical sections here are a bounds check, a store
 * and two index updates -- contention is brief and rare enough that the wider state machine
 * costs more on every acquire than the notify saves. A `notify_one()` with nobody parked is
 * close to free: the standard libraries keep a waiter count per address and skip the futex
 * syscall when it is zero.
 *
 * Sharing the word with the closed flag is what dictates the instructions. **Every update must
 * be `fetch_or`/`fetch_and`** -- an `exchange` or a plain store would clobber the flag, which is
 * exactly the class of bug that lost items in three other segments earlier. `fetch_or` costs the
 * same single RMW as an exchange and returns more: the previous word, whose `kClosed` bit tells
 * the new owner the close state without a second load.
 *
 * `std::atomic::wait`/`notify_one` are C++20 and map onto the platform's futex where there is
 * one, so a parked thread costs nothing while it waits -- unlike a spin, which is why the spin
 * count is bounded.
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

    /// @name The two bits of `lock_`; see the class note.
    /// @{
    static constexpr uint32_t kLocked = 1u;
    static constexpr uint32_t kClosed = bit::msb_mask<uint32_t>;
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
        if (g.was_closed()) return false;
        if (count_ == capacity_) {
            // A linked segment closes itself here and the close is permanent; see
            // algo::Mutex::enqueue for why a segment that refuses once must refuse for ever.
            // Standalone, a drained queue has to accept again, so the close is conditional.
            // Set under the lock, which is what orders it against an insert: the fullness
            // check, the close and the insert all sit inside one critical section, so nobody
            // can be part-way past the check when the close lands.
            if constexpr (Link::is_linked)
                lock_.fetch_or(kClosed, std::memory_order_relaxed);
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
    /**
     * @brief Refuse all further enqueues, permanently.
     *
     * Takes the lock even though setting a bit does not need it: the lock is what orders a
     * close against an insert already inside its critical section. Making this lock-free would
     * let a producer that read "open" commit afterwards -- precisely the hole that lost items
     * in PSCQ and VyukovNoABA.
     */
    void close() noexcept
        requires(Link::is_linked)
    {
        const Guard g{*this};
        lock_.fetch_or(kClosed, std::memory_order_relaxed);
    }

    /**
     * @return true once closed; a closed segment still drains.
     *
     * Lock-free, unlike the rest: the flag is a bit of an atomic word, and this is called on
     * the proxy's retire path where taking the lock was pure overhead. The answer is a
     * snapshot rather than a serialised read, which is all the proxy treats it as.
     */
    bool is_closed() const noexcept
        requires(Link::is_linked)
    {
        return (lock_.load(std::memory_order_acquire) & kClosed) != 0;
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
        lock_.fetch_and(static_cast<uint32_t>(~kClosed), std::memory_order_relaxed);
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
     * @brief Bounded spin, then park.
     * @return the word as it stood *before* the lock was taken, so its kClosed bit is the close
     *         state the new owner should act on -- read for free, out of the same RMW.
     *
     * `fetch_or` rather than `exchange` because the closed flag shares this word; see the class
     * note. The bound is the point of the spin: past a few attempts, spinning is worse than
     * sleeping, because it burns a core the lock holder may be trying to run on.
     */
    uint32_t acquire() const noexcept {
        for (;;) {
            // `<=` so that spins_before_park<0> still makes one attempt before parking, which
            // is what makes 0 mean "a pure futex" rather than "deadlock".
            for (uint32_t i = 0; i <= kSpins; ++i) {
                const uint32_t prev = lock_.fetch_or(kLocked, std::memory_order_acquire);
                if ((prev & kLocked) == 0) return prev;
                SPIN_HINT();
            }
            const uint32_t held = lock_.load(std::memory_order_relaxed);
            // Re-checked rather than assumed: the holder may have released between the last
            // attempt and here, and waiting on a value that is already stale never wakes.
            if (held & kLocked) lock_.wait(held, std::memory_order_relaxed);
        }
    }

    /// Clears the lock bit and *only* the lock bit -- a store here would drop the close.
    void release() const noexcept {
        lock_.fetch_and(static_cast<uint32_t>(~kLocked), std::memory_order_release);
        lock_.notify_one();
    }

    /// Scoped lock, so an early `return` inside a critical section cannot leak it. Carries the
    /// word observed at acquisition, so a critical section that needs the close state has it.
    struct Guard {
        const Spin& q;
        uint32_t prev;
        explicit Guard(const Spin& s) noexcept : q{s}, prev{s.acquire()} {}
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
        ~Guard() { q.release(); }

        /// Was the queue already closed when this lock was taken?
        bool was_closed() const noexcept { return (prev & kClosed) != 0; }
    };

    FORCE_INLINE std::size_t wrap(std::size_t i) const noexcept {
        if constexpr (pow2) return i & (capacity_ - 1);
        else return i % capacity_;
    }

    /// Lock bit and closed bit in one word. **Only ever updated with fetch_or/fetch_and** --
    /// an exchange or a plain store would silently clear the close. See the class note.
    CACHE_ALIGN mutable std::atomic<uint32_t> lock_{0};
    CACHE_PAD(std::atomic<uint32_t>);
    std::size_t head_ = 0, tail_ = 0, count_ = 0;
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
