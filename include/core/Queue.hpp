#pragma once
#include <concepts>
#include <cstddef>

/**
 * @file Queue.hpp
 * @brief The base queue contract, as a concept.
 *
 * Replaces base::IQueue<T>. Nothing in core/ names an implementation, and nothing
 * here introduces a virtual function: satisfaction is checked at compile time and
 * every call through these contracts is statically bound.
 */
namespace core {

/**
 * @brief Minimal contract shared by every queue, bounded or unbounded.
 *
 * @tparam Q the queue type
 * @tparam T the element type
 *
 * @note `size()` is part of the contract for *every* queue. Under the old
 *       IQueue/ILinkedSegment split it was declared on one base but not the
 *       other, so the same call meant different things depending on the segment.
 */
template <typename Q, typename T>
concept Queue = requires(Q q, const Q cq, T item, T& out) {
    { q.enqueue(item) } noexcept -> std::same_as<bool>;
    { q.dequeue(out) } noexcept -> std::same_as<bool>;
    { cq.size() } noexcept -> std::same_as<std::size_t>;
    { cq.capacity() } noexcept -> std::same_as<std::size_t>;
};

} // namespace core
