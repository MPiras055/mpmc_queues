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
 * if constexpr (requires { q.join(); }) (void)q.join();
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

/**
 * @brief Built directly from a segment capacity alone. Every proxy.
 *
 * @note One argument, deliberately. A proxy's second constructor parameter is now `chunks`
 *       with a default, so a two-argument form would still match here and generic code would
 *       quietly bind a thread count to a bound-in-segments. Narrowing this makes a stale
 *       two-argument construction a compile error instead of a change of meaning. Standalone
 *       queues take `(capacity, mem::Blocks)` and so still fail it, which is what keeps
 *       Constructible discriminating.
 */
template <typename Q>
concept DirectConstructed = std::constructible_from<Q, std::size_t>;

/// Threads must join before use; the returned scope releases them.
template <typename Q>
concept Joinable = requires(Q q) {
    typename Q::session;
    { q.join() } -> std::same_as<typename Q::session>;
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
