#pragma once
#include <concepts>
#include <cstddef>
#include <optional>

namespace core {

/**
 * @brief Where a proxy gets segments from, and where retired ones go back to.
 *
 * This is the abstraction that collapses BoundedMemProxy out of existence.
 *
 * That proxy was a separate ~290-line file only because its bound was enforced
 * differently. But its bound *is its pool size*: the recycler pre-allocates
 * `Capacity` segments and enqueue fails exactly when no index is free. That is not an
 * admission policy — it is a source that can run out. Once `acquire()` returns an
 * optional, the memory bound is expressed by the source, and the memory-bounded proxy
 * is just `admit::None` over a pooled source.
 *
 * Two models:
 *
 * | | source::Hazard | source::Pool |
 * |---|---|---|
 * | `handle`    | `S*`                        | `VersionedIndex` |
 * | `acquire()` | allocates, always succeeds  | draws from pool, nullopt when exhausted |
 * | `discard()` | destroy immediately         | return to free bucket immediately |
 * | `retire()`  | hazard-scan, then destroy   | defer to epoch, then reuse |
 * | `guard`     | hazard-pointer slot         | epoch pin |
 *
 * @note `discard` vs `retire` is a real distinction the old proxies drew implicitly:
 *       a losing new tail was `delete`d directly with no hazard scan, because no other
 *       thread had ever seen it. Naming the operation stops that being an accident.
 *
 * @note Per-thread proxy bookkeeping is *not* part of this contract. The source only
 *       exposes `ticket()` / `max_threads()`, and the proxy owns its own per-thread
 *       array. That keeps the two concerns from being welded together, which is what
 *       made HazardVector and Recycler non-interchangeable in the first place.
 */
template <typename Src, typename S>
concept SegmentSource = requires(Src src, const Src csrc, typename Src::handle h,
                                 typename Src::guard& g) {
    typename Src::handle; ///< S* or VersionedIndex
    typename Src::guard;  ///< RAII protection scope; releases on destruction

    /// True if acquire() can hand back a segment that previously held items, and which
    /// therefore must be reopen()ed before reuse. False for allocate-on-demand sources.
    { Src::recycles } -> std::convertible_to<bool>;

    { Src::nil() } noexcept -> std::same_as<typename Src::handle>;

    { src.pin() } noexcept -> std::same_as<typename Src::guard>;
    { src.protect(g, h) } noexcept -> std::same_as<typename Src::handle>;
    { src.deref(h) } noexcept -> std::same_as<S*>;

    { src.acquire() } -> std::same_as<std::optional<typename Src::handle>>;
    { src.discard(h) } noexcept -> std::same_as<void>;
    { src.retire(h) } noexcept -> std::same_as<void>;

    { src.register_thread() } noexcept -> std::same_as<bool>;
    { src.unregister_thread() } noexcept -> std::same_as<void>;
    { src.ticket() } noexcept -> std::same_as<std::size_t>;
    { csrc.max_threads() } noexcept -> std::same_as<std::size_t>;
};

} // namespace core
