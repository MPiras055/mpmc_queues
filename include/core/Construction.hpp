#pragma once
#include <concepts>
#include <cstddef>

namespace core {

/**
 * @file Construction.hpp
 * @brief How a queue is built, and whether it needs a thread ticket — as contracts.
 *
 * Generic code has to build both standalone queues and proxies, and they are constructed
 * differently. That difference used to be discovered by probing inline:
 *
 * @code
 * static constexpr bool single_block = requires(std::size_t n) { Q::create(n); };
 * if constexpr (requires { q.acquire(); }) (void)q.acquire();
 * @endcode
 *
 * which is the capability-detection-by-guessing this project spent a refactor removing.
 * Its failure mode is silence: a standalone queue that lost its `create()` does not
 * report anything, it just falls into the proxy branch and fails later with a
 * constructor error pointing at the wrong place.
 *
 * Naming the shapes lets the ambiguous and the unsupported cases be diagnosed instead.
 */

/// Built as one block: `Q::create(capacity)`, released through mem::SingleBlock::destroy.
template <typename Q>
concept BlockAllocated = requires(std::size_t n) {
    { Q::create(n) } -> std::same_as<Q*>;
};

/// Built directly: `Q(segment_capacity, max_threads)`. Every proxy.
template <typename Q>
concept DirectConstructed = std::constructible_from<Q, std::size_t, std::size_t>;

/// Requires a per-thread ticket before use, and releases it after.
template <typename Q>
concept Ticketed = requires(Q q) {
    { q.acquire() } noexcept -> std::same_as<bool>;
    { q.release() } noexcept -> std::same_as<void>;
};

/**
 * @brief Exactly one construction shape applies.
 *
 * Both would be ambiguous and neither is unsupported; either way generic code should say
 * so rather than pick a branch.
 */
template <typename Q>
concept Constructible = BlockAllocated<Q> != DirectConstructed<Q>;

} // namespace core
