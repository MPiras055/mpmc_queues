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

/**
 * @brief Built from a capacity **and** the thread shape: `Q::create(capacity, producers,
 *        consumers)`, released through `Q::destroy`.
 *
 * For the shard blocks (block::AllToAll behind queue::ShardQueue), whose matrix is sized by the
 * number of producers and consumers that will use it.
 *
 * Defined as *not* BlockAllocated, and that is not redundant. mem::SingleBlock's
 * `create(n, Args&&...)` is variadic, so every standalone queue also answers
 * `create(n, n, n)` in an unevaluated probe -- the failure only shows inside the body. Arity
 * alone cannot tell the two apart; what can is that a genuinely shaped create does **not**
 * answer `create(n)`.
 */
template <typename Q>
concept ShapeConstructed = !BlockAllocated<Q> && requires(std::size_t n, Q* q) {
    { Q::create(n, n, n) } -> std::same_as<Q*>;
    Q::destroy(q);
};

/// Threads must join before use; the returned scope releases them.
template <typename Q>
concept Joinable = requires(Q q) {
    typename Q::session;
    { q.join() } -> std::same_as<typename Q::session>;
};

/**
 * @brief Exactly one construction shape applies.
 *
 * "Exactly one of three" rather than the XOR of two it used to be. ShapeConstructed excludes
 * BlockAllocated by definition (see there), so the count still rejects a type that is both
 * directly constructible and shaped.
 */
template <typename Q>
concept Constructible =
    (int{BlockAllocated<Q>} + int{DirectConstructed<Q>} + int{ShapeConstructed<Q>}) == 1;

} // namespace core
