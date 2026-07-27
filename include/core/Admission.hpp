#pragma once
#include <concepts>
#include <cstddef>

namespace core {

/**
 * @brief Whether a proxy will admit another item, and the bookkeeping to decide it.
 *
 * The only genuine difference between UnboundedProxy, BoundedCounterProxy and
 * BoundedChunkProxy was this predicate; everything else in those three files was the
 * same Michael-Scott traversal written out three times.
 *
 * Every policy is constructible as `A(total_capacity, segment_capacity)` so the proxy
 * can build one generically:
 *   - `admit::None`         ignores both (empty struct, costs nothing)
 *   - `admit::ItemCount`    bounds items:    total_capacity
 *   - `admit::SegmentCount` bounds segments: total_capacity / segment_capacity
 */
template <typename A>
concept AdmissionPolicy =
    std::constructible_from<A, std::size_t, std::size_t> && requires(A a, const A ca) {
        { A::bounded } -> std::convertible_to<bool>; ///< does this policy impose any ceiling?
        { ca.bound() } noexcept -> std::same_as<std::size_t>; ///< 0 means unbounded

        /**
         * Claim the right to admit one item. Where the policy can, this *reserves*
         * rather than merely testing: a plain `may_admit()` check followed by an
         * enqueue is a check-then-act race, and every producer that passes the test
         * before any of them commits will overshoot. Measured on the previous
         * check-then-act version with 4 producers against a bound of 256: peak
         * occupancy 257.
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
