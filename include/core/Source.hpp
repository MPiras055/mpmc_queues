#pragma once
/**
 * @file Source.hpp
 * @brief The contract for where segments come from and where retired ones go.
 * @ingroup core
 */

#include <concepts>
#include <cstddef>
#include <optional>

namespace core {

/**
 * @brief Where a proxy gets segments from, and where retired ones go back to.
 * Two models:
 *
 * | | source::Hazard | source::Pool |
 * |---|---|---|
 * | `handle`    | `S*`                        | `VersionedIndex` |
 * | `acquire()` | allocates, always succeeds  | draws from pool, nullopt when exhausted |
 * | `discard()` | destroy immediately         | return to cache if enabled or retired |
 * | `retire()`  | hazard-scan, then destroy   | defer to epoch, then reuse |
 * | `guard`     | hazard-pointer slot         | epoch pin |
 *
 * @note **The source carries the proxy's per-thread state.** `thread_payload` is whatever
 *       the proxy needs per thread, and the source stores it alongside its own in the same
 *       registry node, so `guard::payload()` reaches it with the thread-local lookup
 *       `pin()` already did.
 */
template <typename Src, typename S>
concept SegmentSource = requires(Src src, const Src csrc, typename Src::handle h,
                                 typename Src::guard& g) {
    typename Src::handle;         ///< type used by the source to refer to segments (allows dereferencing)
    typename Src::guard;          ///< RAII protection scope; releases on destruction
    typename Src::thread_payload; ///< the caller's per-thread state, stored in the node

    /// True if acquire() can hand back a segment that previously held items, and which
    /// therefore must be reopen()ed before reuse. False for allocate-on-demand sources.
    { Src::recycles } -> std::convertible_to<bool>;

    /// Return a null handle which can be used for atomic comparisons
    { Src::nil() } noexcept -> std::same_as<typename Src::handle>;

    /**
     * How many segments this source can have outstanding, or **0 when unbounded**.
     *
     * A pooled source answers with its slot count; an allocating one answers 0. Pairs with
     * AdmissionPolicy::live_segments as the capacity-splitting divisor.
     */
    { Src::live_segments() } noexcept -> std::same_as<std::size_t>;
    /// announce that the caller is about to access shared memory
    { src.pin() } noexcept -> std::same_as<typename Src::guard>;
    /// retrieve the payload specific to the thread holding the guard
    { g.payload() } noexcept -> std::same_as<typename Src::thread_payload&>;
    /// protect a specific handle in shared memory
    { src.protect(g, h) } noexcept -> std::same_as<typename Src::handle>;

    /**
     * @brief Move this thread's protection forward, releasing anything protected earlier.
     *
     * @pre The caller will not dereference any handle it obtained before this call. Whatever
     *      it still needs must be re-read from a shared anchor *after* renewing, because a
     *      source that reclaims by epoch may free anything retired under the old one.
     *
     * A no-op where protection is per-handle and nothing is being held back -- source::Hazard
     * publishes a hazard pointer and has no epoch to advance. For source::Pool it republishes
     * the pin, which is what stops one long traversal from blocking every other thread's
     * rotation for its whole duration.
     *
     * Deliberately separate from protect(): protect() never invalidates anything, so a source
     * that genuinely needs two live protections at once remains expressible.
     *
     * @return true if protection actually moved, and therefore if handles obtained before the
     *         call are now void. False means the call was a no-op and the caller's existing
     *         handles are still good -- which lets a caller skip the re-read, and lets it tell
     *         "I was holding the reclaimer back" apart from "there is genuinely nothing free".
     *         Hazard always returns false; Pool returns false when it was already current.
     */
    { src.renew(g) } noexcept -> std::same_as<bool>;
    /// Dereference a handle in order to obtain a segment pointer
    { src.deref(h) } noexcept -> std::same_as<S*>;

    /// attempt to acquire a new handle which (if got) is private to the caller
    { src.acquire() } -> std::same_as<std::optional<typename Src::handle>>;

    /// as previous: provide a hint lambda which represent a lightweight check to determine if the handle 
    /// is still needed (handy if getting a new handle incurs in synchronization)
    { src.acquire([]() noexcept { return false; }) } -> std::same_as<std::optional<typename Src::handle>>;
    /// discard a segment which is private to the caller and won't be referenced again
    { src.discard(h) } noexcept -> std::same_as<void>;
    /// discard a segment which was shared to other threads (involve shared memory reclamation)
    { src.retire(h) } noexcept -> std::same_as<void>;

    /// join the source for a lifetime
    typename Src::session; ///< RAII thread registration; detaches on destruction
    { src.join() } -> std::same_as<typename Src::session>;
};

} // namespace core
