#pragma once
/**
 * @file Admission.hpp
 * @brief The contract for what stops a proxy admitting another item.
 * @ingroup core
 */

#include <concepts>
#include <cstddef>

namespace core {

/**
 * @brief When the proxy must ask a policy, which is the main thing that distinguishes them.
 *
 * A policy that counts *items* must be asked before anything is committed, because the answer
 * depends on the item itself. A policy that counts *segments* cannot answer usefully at that
 * point: whether this enqueue causes a segment to be linked is not known until the tail refuses
 * the item, and most enqueues link nothing. Asking it up front makes it refuse while the tail
 * still has free slots.
 *
 * That was not hypothetical -- with `SegmentCount` asked at `Enqueue`, a chunk-bounded queue
 * reached only `(bound - 1) * segment + 1` items, and at `chunks == 1` it held **zero** while
 * advertising a full segment's worth.
 */
enum class AdmitPoint {
    /// At the top of every enqueue, before the traversal. For policies that must *reserve*.
    Enqueue,
    /// Only when the tail has refused and a new segment is about to be acquired and linked.
    SegmentLink,
};

/**
 * @brief Whether a proxy will admit another item, and the bookkeeping to decide it.
 *
 * The only genuine difference between UnboundedProxy, BoundedCounterProxy and
 * BoundedChunkProxy was this predicate; everything else in those three files was the
 * same Michael-Scott traversal written out three times.
 *
 * @note A policy configures *itself*. The proxy hands over the two numbers it knows —
 *       segment capacity and a chunk count — and the policy decides what they mean to
 *       it. Previously the proxy did that arithmetic, so it had to know that ItemCount
 *       wanted `segment_capacity * chunks` while SegmentCount wanted the divisor, and
 *       `capacity()` branched on whether the policy was bounded at all. That is the
 *       policy's business, not the traversal's.
 *
 * @note Config is passed by value rather than the policy being returned from a factory:
 *       these hold atomics, so they are not movable and must be constructed in place.
 */
template <typename A>
concept AdmissionPolicy = requires(A a, const A ca, std::size_t n) {
    typename A::Config;

    /// Derive this policy's configuration from what the proxy knows.
    { A::config(n, n) } -> std::same_as<typename A::Config>;
    requires std::constructible_from<A, typename A::Config>;

    { A::bounded } -> std::convertible_to<bool>; ///< does this policy impose any ceiling?

    /// Where the proxy asks. Resolved with `if constexpr`, so a policy pays for exactly one
    /// call site and the other compiles away entirely.
    { A::admit_point } -> std::convertible_to<AdmitPoint>;

    /**
     * How many segments this policy allows to be live at once, given the chunk count.
     *
     * **0 means "this policy does not bound the segment count"**, which is the answer for a
     * policy that counts items or nothing at all. LinkedProxy uses it as the divisor when
     * splitting a total capacity across the segments that will exist, taking the smaller
     * non-zero of this and the source's own limit.
     */
    { A::live_segments(n) } noexcept -> std::same_as<std::size_t>;

    /// The item ceiling, given the segment capacity. Lets the proxy report a capacity
    /// without knowing whether the bound counts items, segments, or nothing at all.
    { ca.capacity(n) } noexcept -> std::same_as<std::size_t>;

    /// The bound in the policy's own unit (items, segments, ...). For diagnostics.
    { ca.bound() } noexcept -> std::same_as<std::size_t>;

    /**
     * Claim the right to admit one item. Where the policy can, this *reserves* rather
     * than merely testing: a plain `may_admit()` check followed by an enqueue is a
     * check-then-act race, and every producer that passes the test before any of them
     * commits will overshoot. Measured on the previous check-then-act version with 4
     * producers against a bound of 256: peak occupancy 257.
     */
    { a.try_admit() } noexcept -> std::same_as<bool>;

    /// Release a claim taken by try_admit() when the enqueue could not go through.
    { a.cancel_admit() } noexcept -> std::same_as<void>;

    { a.on_enqueue() } noexcept -> std::same_as<void>;
    { a.on_dequeue() } noexcept -> std::same_as<void>;
    { a.on_segment_linked() } noexcept -> std::same_as<void>;
    { a.on_segment_retired() } noexcept -> std::same_as<void>;
};

} // namespace core
