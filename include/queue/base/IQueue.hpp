// ==========================
// Base Queue Interface
// ==========================

#pragma once
#include <cstddef>
#include <type_traits>

namespace base {

/// @brief Generic queue interface with minimal contracts for enqueue/dequeue operations.
///
/// This interface defines the common queue operations, as well as lifecycle
/// contracts used in linked-segment based queues. 
/// 
/// @tparam T The element type. By default restricted to pointer types 
///           (e.g., `Node*`) for intrusive / lock-free use cases.
template <typename T>
class IQueue {
    // Restrict to pointer types (can be relaxed if needed)
    // static_assert(std::is_pointer_v<T>, "IQueue requires T to be a pointer type");

public:
    using value_type = T;

    /// @brief Virtual destructor to allow cleanup through base pointer.
    virtual ~IQueue() = default;

    // ==========================
    // Core Queue Operations
    // ==========================

    /// @brief Enqueues an item into the queue.
    ///
    /// @param item Pointer to the item being enqueued.
    /// @return true if the item was successfully added, 
    ///         false if the queue is full or closed.
    virtual bool enqueue(const T item) = 0;

    /// @brief Dequeues an item from the queue.
    ///
    /// @param container Reference where the dequeued pointer will be stored.
    /// @return true if an item was successfully removed, 
    ///         false if the queue is empty or closed.
    virtual bool dequeue(T& container) = 0;

    /// @brief Returns the maximum number of elements the queue can hold.
    ///
    /// For bounded queues this is a fixed number. For proxies or 
    /// dynamically linked queues, it may be an aggregate value.
    ///
    /// @return The capacity of the queue.
    virtual size_t capacity() const = 0;

    /// @brief Returns the current number of elements in the queue.
    ///
    /// For concurrent queues, this value may be approximate.
    ///
    /// @return The number of elements currently stored.
    virtual size_t size() = 0;

protected: 
    // Only accessible to friends (e.g. Proxy classes) or derived types (LinkedSegments).

    // ==========================
    // Lifecycle Control (Linked Queues)
    // ==========================

    /// @brief Marks the queue as closed.
    ///
    /// A closed queue will not accept new items (`enqueue` fails),
    /// but may still allow `dequeue` of remaining items.
    ///
    /// @return true if successfully closed, false otherwise.
    virtual bool close() = 0;

    /// @brief Marks the queue as open.
    ///
    /// An open queue can accept new items (`enqueue` succeeds if not full).
    ///
    /// @return true if successfully opened, false otherwise.
    virtual bool open() = 0;

    /// @brief Checks if the queue has been closed.
    ///
    /// @return true if closed, false otherwise.
    virtual bool isClosed() const = 0;

    /// @brief Checks if the queue is currently open.
    ///
    /// @return true if open, false otherwise.
    virtual bool isOpened() const = 0;
};

}   //namespace meta
