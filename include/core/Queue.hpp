#pragma once
#include <concepts>
#include <cstddef>

/**
 * @file Queue.hpp
 * @brief The base queue contract, as a concept.
 */
namespace core {

/**
 * @brief Minimal contract shared by every queue, bounded or unbounded.
 *
 * @tparam Q the queue type
 * @tparam T the element typeù
 */
template <typename Q, typename T>
concept Queue = requires(Q q, const Q cq, T item, T& out) {
    { q.enqueue(item) } noexcept -> std::same_as<bool>;
    { q.dequeue(out) } noexcept -> std::same_as<bool>;
    { cq.size() } noexcept -> std::same_as<std::size_t>;
    { cq.capacity() } noexcept -> std::same_as<std::size_t>;
};

} // namespace core
