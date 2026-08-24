#pragma once
/**
 * @file SegmentTraits.hpp
 * @brief Per-segment capability declarations.
 * @ingroup core
 */

#include <concepts>
#include <type_traits>

namespace core {

/**
 * @brief Per-segment capability block.
 *
 * @warning The primary template is deliberately left **undefined**. A segment with no
 *          specialization fails to compile at first use
 */
template <typename S>
struct segment_traits;

/**
 * @brief A specialization that declares every flag the proxy consults.
 *
 * Checking for one flag as a proxy for "complete" is not enough. When
 * `needs_inflight_drain` was added for SCQ's non-atomic insert, six specializations had to
 * be edited by hand, and a specialization that had missed it would have compiled here and
 * failed later at whichever use site first read the flag — a diagnostic pointing at the
 * proxy rather than at the segment that was actually incomplete.
 *
 * `static_assert`ing this beside each specialization moves the error back to the thing
 * that is wrong, and makes adding a seventh flag a mechanical, compiler-guided edit.
 */
template <typename S>
concept CompleteSegmentTraits = requires {
    /// Does enqueue take a "you may already be closed" hint? (PRQ, SCQ)
    { segment_traits<S>::needs_close_hint } -> std::convertible_to<bool>;
    /// Must the segment be prepared before the post-link drain retry? (SCQ's threshold)
    { segment_traits<S>::needs_dequeue_prepare } -> std::convertible_to<bool>;
    /// Can a producer be part-way through a non-atomic insert? (SCQ)
    { segment_traits<S>::needs_inflight_drain } -> std::convertible_to<bool>;
    /// Can a drained segment be reopened and reused? (false for write-once arrays)
    { segment_traits<S>::recyclable } -> std::convertible_to<bool>;
    /// Can the payload be a null pointer, or does the tagging scheme reserve it?
    { segment_traits<S>::can_store_null } -> std::convertible_to<bool>;
};

/**
 * @brief Assert a specialization is complete, right where it is written.
 *
 * Put this immediately after each `segment_traits` specialization:
 * @code
 * template <typename T, typename Opt, typename Link>
 * struct core::segment_traits<algo::Vyukov<T, Opt, Link>> { ... };
 * MPMC_ASSERT_SEGMENT_TRAITS(algo::Vyukov<int*, meta::EmptyOptions, linkage::None>);
 * @endcode
 */
#define MPMC_ASSERT_SEGMENT_TRAITS(...)                                                    \
    static_assert(::core::CompleteSegmentTraits<__VA_ARGS__>,                              \
                  "incomplete core::segment_traits specialization for " #__VA_ARGS__       \
                  ": every flag named by core::CompleteSegmentTraits must be declared")

} // namespace core
