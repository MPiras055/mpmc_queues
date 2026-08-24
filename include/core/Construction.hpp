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
 */

/// Built as one block: `Q::create(capacity)`, released through mem::SingleBlock::destroy.
template <typename Q>
concept BlockAllocated = requires(std::size_t n) {
    { Q::create(n) } -> std::same_as<Q*>;
};

/**
 * @brief Built directly from a segment capacity alone. Every proxy.
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
 */
template <typename Q>
concept Constructible = BlockAllocated<Q> != DirectConstructed<Q>;

} // namespace core
