#pragma once
#include <type_traits>

namespace core {

/**
 * @brief Per-segment capability block.
 *
 * @warning The primary template is deliberately left **undefined**. A segment with no
 *          specialization fails to compile at first use, naming the missing
 *          specialization. This replaces three mechanisms that all failed *silently*:
 *
 *          - `ILinkedSegment::info_required`, inherited with a `false` default, so a
 *            segment that needed the hint and forgot simply got the slow path — or the
 *            livelock the flag exists to prevent.
 *          - a `requires(Segment s){ s.prepareDequeueAfterNextLinked(); }` probe,
 *            copy-pasted into all four proxies, which silently did nothing if the
 *            method was ever renamed.
 *          - `void_t` tag traits, which answered `false` on a misspelled tag.
 *
 * Every flag here must carry the reason it exists, next to the flag.
 */
template <typename S>
struct segment_traits;

// ---------------------------------------------------------------------------
// Detection helper: has a specialization been provided?
// ---------------------------------------------------------------------------
namespace detail {
template <typename S, typename = void>
struct has_segment_traits : std::false_type {};

template <typename S>
struct has_segment_traits<S, std::void_t<decltype(segment_traits<S>::recyclable)>>
    : std::true_type {};
} // namespace detail

template <typename S>
inline constexpr bool has_segment_traits_v = detail::has_segment_traits<S>::value;

} // namespace core
