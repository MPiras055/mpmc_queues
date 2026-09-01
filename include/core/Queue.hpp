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
    /**
     * @name The blocking pair
     *
     * "Blocking" only in the sense that an implementation is *allowed* to wait here. Every
     * lock-free queue in this tree returns immediately and forwards to the try-version below;
     * algo::Mutex genuinely parks until there is room, an item, or a close().
     * @{
     */
    { q.enqueue(item) } noexcept -> std::same_as<bool>;
    { q.dequeue(out) } noexcept -> std::same_as<bool>;
    /// @}

    /**
     * @name The non-blocking pair
     *
     * Never waits: refuses a full queue and reports an empty one. This is what generic code
     * must use to *drain*, because a blocking dequeue on an empty queue only returns once
     * somebody closes it -- and generic code is in no position to decide that.
     * @{
     */
    { q.try_enqueue(item) } noexcept -> std::same_as<bool>;
    { q.try_dequeue(out) } noexcept -> std::same_as<bool>;
    /// @}
    { cq.size() } noexcept -> std::same_as<std::size_t>;
    { cq.capacity() } noexcept -> std::same_as<std::size_t>;
};

} // namespace core
