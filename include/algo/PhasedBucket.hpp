#pragma once
#include <meta/OptionsPack.hpp>
#include <util/specs.hpp>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>

namespace algo {

struct PhasedBucketOpt {
    /**
     * @brief Do not reset the opposite index lazily; the owner will call reset().
     *
     * The default folds the reset into the first operation of each phase (see the class
     * documentation). Turn it off when the phase boundary is already a synchronisation point
     * the owner controls -- an epoch advance, say -- because then the reset can happen once,
     * by the thread that performs the flip, instead of being re-checked by every operation.
     */
    struct no_implicit_reset {};
};

/**
 * @brief A phased, fixed-capacity MPMC bucket: fill with producers, drain with consumers.
 *
 * Built for an epoch reclaimer's access pattern rather than for general use. A bucket in an
 * epoch rotation is never pushed and popped at the same time: while it is the *current* stage
 * it only accumulates, and by the time it becomes reusable nothing is still writing to it.
 * Given that, the usual machinery of an MPMC ring -- a sequence number per cell, a CAS loop
 * per operation, padding so producers and consumers do not share lines -- is paying for a race
 * that cannot happen.
 *
 * Both ends are a single `fetch_add` on one packed word:
 *
 * ```
 *  63                      32 31                       0
 * +--------------------------+--------------------------+
 * |        tail (write)      |        head (read)        |
 * +--------------------------+--------------------------+
 * ```
 *
 * Enqueue adds to the high half and writes its slot; dequeue adds to the low half and takes
 * one. Neither retries, so enqueue is wait-free and dequeue is wait-free on the fast path.
 * There is one cell array and no per-cell sequence word, so a bucket of N slots is
 * `8 * (N + 1)` bytes rather than a padded cache line each.
 *
 * ## The phase invariants
 *
 * These are the caller's to keep, and the class asserts them under NDEBUG-off rather than
 * defending against them:
 *
 *  1. **Accumulate:** many producers, no consumers.
 *  2. **Reclaim:** many consumers, no producers.
 *  3. **Never overfills.** At most `Capacity` values are enqueued per fill phase. For a
 *     reclaimer that holds because the values are the slot indices themselves: there are only
 *     `Capacity` of them and each can be retired once per cycle.
 *
 * Violating (3) is the dangerous one -- the tail runs past the array -- so it is checked by
 * assertion on every enqueue, and in a debug build every write also verifies it is landing on
 * an empty cell, which catches a producer/consumer overlap that (1) and (2) should have
 * prevented.
 *
 * ## Resetting between phases
 *
 * A phase leaves one half of the state word dirty: after draining, the head is non-zero, and
 * the next fill has to start from zero again. By default that is handled lazily -- the first
 * enqueue of a fill phase notices a dirty head and clears it, and the first exhausted dequeue
 * of a drain phase clears the tail -- so the flip costs nothing but a predictable branch and
 * needs no coordination.
 *
 * `PhasedBucketOpt::no_implicit_reset` removes both checks, and the owner must call `reset()`
 * at the flip. Worth it when the flip is already a serialising event with exactly one winner,
 * which is precisely the shape of an epoch advance.
 *
 * @warning With the implicit reset disabled, a `dequeue()` on an empty bucket still advances
 *          the head. Repeatedly polling an empty bucket that is never reset will eventually
 *          run the head past 32 bits.
 *
 * @tparam Capacity slots, fixed at compile time so the buffer is a member and the modulo is
 *                  never computed -- indices are used directly, never wrapped.
 */
template <std::size_t Capacity, typename Opt = meta::EmptyOptions>
    requires meta::AcceptsOnly<Opt, typename PhasedBucketOpt::no_implicit_reset>
class PhasedBucket {
    static constexpr bool implicit_reset =
        !Opt::template has<typename PhasedBucketOpt::no_implicit_reset>;

    static constexpr uint64_t kShift = 32;
    static constexpr uint64_t kHeadInc = 1;
    static constexpr uint64_t kHeadMask = (uint64_t{1} << kShift) - 1;
    static constexpr uint64_t kTailInc = uint64_t{1} << kShift;
    static constexpr uint64_t kTailMask = ~kHeadMask;

public:
    using value_type = std::size_t;

    /// Reserved: never a stored value. Values must therefore be < Capacity.
    static constexpr value_type kEmpty = Capacity;

    static_assert(Capacity > 0, "PhasedBucket: capacity must be non-zero");
    static_assert(Capacity < (uint64_t{1} << kShift), "PhasedBucket: capacity must fit in 32 bits");
    static_assert(std::atomic<uint64_t>::is_always_lock_free,
                  "PhasedBucket: 64-bit atomics must be lock-free");

    PhasedBucket() noexcept {
        state_.store(0, std::memory_order_relaxed);
        for (std::size_t i = 0; i < Capacity; ++i)
            buffer_[i].store(kEmpty, std::memory_order_relaxed);
    }

    PhasedBucket(const PhasedBucket&) = delete;
    PhasedBucket& operator=(const PhasedBucket&) = delete;

    static constexpr std::size_t capacity() noexcept { return Capacity; }

    /**
     * @brief Add a value. Wait-free: one fetch_add and one store, no retry.
     * @pre Fewer than Capacity values have been added since the last reset, and no consumer
     *      is running (phase invariants 1 and 3).
     */
    void enqueue(value_type item) noexcept {
        assert(item != kEmpty && "PhasedBucket: kEmpty is reserved");
        assert(item < Capacity && "PhasedBucket: value out of range");

        const uint64_t old = state_.fetch_add(kTailInc, std::memory_order_acq_rel);
        const auto tail = static_cast<uint32_t>(old >> kShift);

        if constexpr (implicit_reset) {
            // First fill after a drain: the head is still where the drain left it.
            if (old & kHeadMask) state_.fetch_and(kTailMask, std::memory_order_relaxed);
        }

        assert(tail < Capacity && "PhasedBucket: overfilled -- phase invariant 3 violated");

#ifndef NDEBUG
        const value_type prev = buffer_[tail].exchange(item, std::memory_order_acq_rel);
        assert(prev == kEmpty &&
               "PhasedBucket: wrote over a live cell -- a consumer was running, or the "
               "bucket was not reset between phases");
#else
        buffer_[tail].store(item, std::memory_order_release);
#endif
    }

    /**
     * @brief Take a value.
     * @pre No producer is running (phase invariant 2).
     * @return false when the bucket is drained.
     */
    [[nodiscard]] bool dequeue(value_type& out) noexcept {
        const uint64_t old = state_.fetch_add(kHeadInc, std::memory_order_acq_rel);
        const auto head = static_cast<uint32_t>(old & kHeadMask);

        if (head < Capacity) {
            const value_type v = buffer_[head].exchange(kEmpty, std::memory_order_acq_rel);
            if (v != kEmpty) {
                out = v;
                return true;
            }
        }

        if constexpr (implicit_reset) {
            // Drained, or past the end: clear the tail so the next fill starts at zero.
            if (old & kTailMask) state_.fetch_and(kHeadMask, std::memory_order_release);
        }
        return false;
    }

    /// Start a fresh fill phase. Only meaningful when nothing else is touching the bucket.
    void reset() noexcept { state_.store(0, std::memory_order_release); }

    /// Values still available to dequeue. Approximate while either phase is in flight.
    std::size_t size() const noexcept {
        const uint64_t s = state_.load(std::memory_order_relaxed);
        const auto tail = static_cast<uint32_t>(s >> kShift);
        const auto head = static_cast<uint32_t>(s & kHeadMask);
        if (tail <= head) return 0;
        return (tail > Capacity ? Capacity : tail) - head;
    }

    bool empty() const noexcept { return size() == 0; }

private:
    /// One word for both ends, so a fill and a drain never touch separate lines and the
    /// reset of one half is a single masked read-modify-write on the other's line.
    ALIGNED_CACHE std::atomic<uint64_t> state_;
    CACHE_PAD_TYPES(std::atomic<uint64_t>);
    /// Aligning the buffer pads `state_` out to its own line as a side effect.
    std::atomic<value_type> buffer_[Capacity];
};

} // namespace algo
