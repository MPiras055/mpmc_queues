#pragma once
#include <core/Queue.hpp>
#include <concepts>

namespace core {

/**
 * @brief A bounded queue that can be chained into an unbounded one.
 *
 * Replaces base::ILinkedSegment<T, NextT>. Three things differ from that interface:
 *
 *  - **`next` has accessors.** `next()` / `link_next()` are part of the contract, so a
 *    proxy no longer reaches into a protected data member to maintain the list. This is
 *    also what lets a segment choose its own representation: LinkedHQ already exposed
 *    getNext() over a private next_, and was unusable only because the proxies assumed
 *    the member directly.
 *  - **The close hint is not in the base arity.** See HintedSegment below.
 *  - **`reopen()` reports failure.** A segment that cannot be recycled returns false
 *    rather than aborting inside open().
 *
 * @tparam S the segment type
 * @tparam T the element type
 */
template <typename S, typename T>
concept LinkedSegment = Queue<S, T> && requires(S s, const S cs, typename S::handle_type h) {
    typename S::handle_type; ///< S* under a pointer source, VersionedIndex under a pooled one

    { s.close() } noexcept -> std::same_as<void>;
    { cs.is_closed() } noexcept -> std::same_as<bool>;
    { s.reopen() } noexcept -> std::same_as<bool>;

    { cs.next() } noexcept -> std::same_as<typename S::handle_type>;
    { s.link_next(h) } noexcept -> std::same_as<bool>; ///< CAS nil -> h, succeeds once
};

/**
 * @brief Optional extension: enqueue that accepts a "this segment may be closed" hint.
 *
 * Required only when segment_traits<S>::needs_close_hint is true. Under the old
 * interface every segment carried this second parameter whether or not it used it,
 * with a default that differed per implementation (true for Vyukov and SCQ, false for
 * PRQ and FAAArray), so omitting the argument silently meant different things.
 */
template <typename S, typename T>
concept HintedSegment = LinkedSegment<S, T> && requires(S s, T item) {
    { s.enqueue(item, bool{}) } noexcept -> std::same_as<bool>;
};

/**
 * @brief Optional extension: preparation required before a post-link dequeue retry.
 *
 * Required only when segment_traits<S>::needs_dequeue_prepare is true. SCQ is built
 * from two rings that keep a threshold counter to make the empty-check cheap; once a
 * successor has been linked, the head segment must be drained exhaustively before it
 * can be unlinked, so the threshold has to be reset first or the retry reports empty
 * while items remain.
 */
template <typename S>
concept PreparableSegment = requires(S s) {
    { s.prepare_dequeue_after_link() } noexcept -> std::same_as<void>;
};

/**
 * @brief Optional extension: the segment can report producers part-way through an insert.
 *
 * Required only when segment_traits<S>::needs_inflight_drain is true. Most segments
 * publish an item with a single atomic step, so a producer is either visible or has not
 * started. SCQ is not one of them: it claims a free index, writes the payload, then
 * publishes the index, and in between the item exists but cannot be found. A proxy that
 * unlinks the segment during that window loses it.
 */
template <typename S>
concept DrainableSegment = requires(const S cs) {
    { cs.has_inflight() } noexcept -> std::same_as<bool>;
};

} // namespace core
