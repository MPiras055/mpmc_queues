#pragma once
/**
 * @file Segment.hpp
 * @brief The contracts for a linked segment and its optional capabilities.
 * @ingroup core
 */

#include <core/Queue.hpp>
#include <concepts>
#include <cstddef>

namespace core {

/**
 * @brief A bounded queue that can be chained into an unbounded one.
 *
 *  - **`next` has accessors.** `next()` / `link_next()` are part of the contract
 *      and allow for link a next segment and retrieve it regardless of the specialization
 *  - **The close hint is not in the base arity.** See HintedSegment below.
 *  - **`reopen()` reports failure.** A segment that cannot be recycled returns false
 *    rather than aborting inside open().
 *
 * @tparam S the segment type
 * @tparam T the element type
 */
template <typename S, typename T>
concept LinkedSegment = Queue<S, T> && requires(S s, const S cs, typename S::handle_type h,
                                                typename S::handle_type& href) {
    typename S::handle_type; /// templated type of the next field (see Handle.hpp)

    /// What a capacity request of n actually yields, before anything is constructed. Segments
    /// round their own size up -- SCQ and LFring always to a power of two -- and LinkedProxy
    /// has to know that to split a total across segments and still report a reachable capacity.
    { S::capacity_for(std::size_t{}) } noexcept -> std::same_as<std::size_t>;

    { s.close() } noexcept -> std::same_as<void>;   /// close the segment to future enqueues
    { cs.is_closed() } noexcept -> std::same_as<bool>;  /// check if the segment is closed
    { s.reopen() } noexcept -> std::same_as<bool>;  /// attempt to reopen a segment after a closure

    { cs.next() } noexcept -> std::same_as<typename S::handle_type>;    /// getter for the next segment linked to this one
    { s.link_next(h, href) } noexcept -> std::same_as<bool>;            /// link a new segment or return a reference to the one which is currently linked
};

/**
 * @brief Optional extension: enqueue that at accepts a "this segment may be closed" hint.
 *
 * Required only when segment_traits<S>::needs_close_hint is true.
 */
template <typename S, typename T>
concept HintedSegment = LinkedSegment<S, T> && requires(S s, T item) {
    { s.enqueue(item, bool{}) } noexcept -> std::same_as<bool>; /// enqueue which accepts a flag used to perform sanity checks
};

/**
 * @brief Optional extension: preparation required before a post-link dequeue retry.
 *
 * Required only when segment_traits<S>::needs_dequeue_prepare is true.
 */
template <typename S>
concept PreparableSegment = requires(S s) {
    { s.prepare_dequeue_after_link() } noexcept -> std::same_as<void>;  
};

/**
 * @brief Optional extension: the segment can report producers part-way through an insert.
 *
 * Required only when segment_traits<S>::needs_inflight_drain is true.
 * 
 * note: some segments may be closed to future enqueues but may need to complete enqueue operations which
 * are midway
 */
template <typename S>
concept DrainableSegment = requires(const S cs) {
    { cs.has_inflight() } noexcept -> std::same_as<bool>;
};

} // namespace core
