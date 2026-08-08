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
 * @note **The source carries the proxy's per-thread state.** `thread_payload` is whatever
 *       the proxy needs per thread, and the source stores it alongside its own in the same
 *       registry node, so `guard::payload()` reaches it with the thread-local lookup
 *       `pin()` already did.
 *
 *       This is a deliberate reversal. The pre-refactor reclamation classes each took the
 *       proxy's metadata as a template parameter, which made them non-interchangeable — a
 *       proxy written against one could not move to the other — and the refactor split
 *       them apart, with the source handing out a dense `ticket()` the proxy used to index
 *       its own array. Making the registry unbounded removed the dense index, so the
 *       choice became "two thread-local lookups per operation" or "one, with the payload
 *       in the node". The measured path won. What keeps it interchangeable this time is
 *       that `thread_payload` is a plain template parameter with an empty default: the
 *       source never names a proxy type, and a source used without a proxy pays nothing
 *       for it under `[[no_unique_address]]`.
 */
template <typename Src, typename S>
concept SegmentSource = requires(Src src, const Src csrc, typename Src::handle h,
                                 typename Src::guard& g) {
    typename Src::handle;         ///< S* or VersionedIndex
    typename Src::guard;          ///< RAII protection scope; releases on destruction
    typename Src::thread_payload; ///< the caller's per-thread state, stored in the node

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

    typename Src::session; ///< RAII thread registration; detaches on destruction
    { src.join() } -> std::same_as<typename Src::session>;

    /// This thread's caller-owned state, reached through the pin the caller already holds.
    { g.payload() } noexcept -> std::same_as<typename Src::thread_payload&>;
};

} // namespace core
