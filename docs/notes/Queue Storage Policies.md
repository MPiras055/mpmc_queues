
# Overview

In this design, we provide flexible support for both **stack-allocated** and **heap-allocated** storage for queues. The storage policy is abstracted using an interface `IStoragePolicy`, which allows different storage implementations to be plugged into the queue. The `BoundedQueue` constructor will receive a pointer to the specified storage policy, which is initialized elsewhere.

This design supports:
- **Heap Storage**: Dynamically allocated buffer.
- **Stack Storage**: Fixed-size buffer allocated on the stack.


## Storage Policy Interface

The `IStoragePolicy` interface defines the essential methods that any storage implementation must provide. It is used to abstract the underlying storage mechanism for the queue.

```cpp
// IStoragePolicy.hpp
#pragma once
#include <cstddef>

/// @brief Abstract base class for all storage policies.
template <typename T>
class IStoragePolicy {
public:
    /// @brief Returns a pointer to the underlying data buffer.
    virtual T* data() = 0;
    
    /// @brief Returns the capacity of the storage.
    virtual size_t capacity() const = 0;

    /// @brief Destructor for the storage policy.
    virtual ~IStoragePolicy() = default;
};
```

### Explanation:

- `data()`: Returns a pointer to the underlying data buffer.
- `capacity()`: Returns the size or capacity of the storage buffer.
- `virtual ~IStoragePolicy() = default`: Ensures proper cleanup when derived classes are destroyed.

## Heap Storage

Heap-based storage allocates memory dynamically on the heap. The `HeapStorage` class implements the `IStoragePolicy` interface, providing a dynamically allocated buffer.
```cpp
// HeapStorage.hpp
#include "IStoragePolicy.hpp"
#include <cstddef>

/// @brief Heap-based storage implementation for queue buffers.
template<typename T>
class HeapStorage : public IStoragePolicy<T> {
public:
    /// @brief Constructs heap storage with the given capacity.
    /// @param capacity The number of elements that can be stored.
    explicit HeapStorage(size_t capacity)
        : capacity_(capacity), buffer_(new T[capacity]) {}

    /// @brief Destructor that cleans up the allocated buffer.
    ~HeapStorage() override {
        delete[] buffer_;
    }

    /// @brief Returns a pointer to the underlying buffer.
    T* data() override {
        return buffer_;
    }

    /// @brief Returns the capacity of the storage.
    size_t capacity() const override {
        return capacity_;
    }

private:
    const size_t capacity_; ///< The number of elements the storage can hold.
    T* buffer_; ///< The dynamically allocated buffer.
};
```

### Explanation:

- The constructor dynamically allocates memory for the buffer on the heap.
- The destructor frees the memory using `delete[]`.
- `data()` returns a pointer to the allocated buffer.
- `capacity()` returns the total size of the buffer.

## Stack Storage

Stack-based storage uses a fixed-size buffer, which is allocated on the stack. The `StackStorage` class implements the `IStoragePolicy` interface using a `std::array` to hold the buffer.

```cpp
// StackStorage.hpp
#pragma once
#include "IStoragePolicy.hpp"
#include <array>
#include <cstddef>

/// @brief Stack-based storage implementation for queue buffers.
template <typename T, size_t N>
class StackStorage : public IStoragePolicy<T> {
public:
    /// @brief Default constructor.
    StackStorage() = default;

    /// @brief Returns a pointer to the underlying buffer.
    T* data() override {
        return buffer_.data();
    }

    /// @brief Returns the capacity of the storage.
    size_t capacity() const override {
        return N;
    }

private:
    std::array<T, N> buffer_; ///< The fixed-size buffer.
};
```

### Explanation:
- The `std::array` is used to allocate a fixed-size buffer at compile time.
- `data()` returns a pointer to the underlying array.
- `capacity()` returns the size of the array.

## Bounded Queue Using Storage Policies

The `BoundedQueue` class can be constructed with any storage policy that implements the `IStoragePolicy` interface. This allows the queue to use either heap-allocated or stack-allocated storage.

```cpp
// BoundedQueue.hpp
#pragma once
#include "IStoragePolicy.hpp"
#include <memory>

/// @brief A bounded queue that uses a custom storage policy.
template <typename T>
class BoundedQueue {
public:
    /// @brief Constructs the queue with the specified storage policy.
    /// @param storage A unique pointer to the storage policy.
    explicit BoundedQueue(std::unique_ptr<IStoragePolicy<T>> storage)
        : storage_(std::move(storage)) {}

private:
    std::unique_ptr<IStoragePolicy<T>> storage_; ///< The storage policy for the queue.
};

```

### Explanation:

- The constructor accepts a `std::unique_ptr` to an `IStoragePolicy`, allowing for flexible choice of storage (stack or heap).
- The `storage_` member stores the given storage policy.

## Example Usage

### Heap Storage Example

In this example, we create a `BoundedQueue` with heap-based storage using the `HeapStorage` class.

```cpp
// main.cpp
#include "BoundedQueue.hpp"
#include "HeapStorage.hpp"

int main() {
    // Create a heap storage with capacity for 1024 integers
    auto heapStorage = std::make_unique<HeapStorage<int>>(1024);

    // Create the bounded queue with the heap storage
    BoundedQueue<int> queue(std::move(heapStorage));

    // The queue is now using heap-allocated storage
}

```

### Stack Storage Example

In this example, we create a `BoundedQueue` with stack-based storage using the `StackStorage` class.

```cpp
// main.cpp
#include "BoundedQueue.hpp"
#include "StackStorage.hpp"

int main() {
    // Create a stack storage with capacity for 1024 integers
    auto stackStorage = std::make_unique<StackStorage<int, 1024>>();

    // Create the bounded queue with the stack storage
    BoundedQueue<int> queue(std::move(stackStorage));

    // The queue is now using stack-allocated storage
}

```

### Explanation:

- **Heap Storage**: Uses dynamic memory (`new`/`delete`).
    
- **Stack Storage**: Uses fixed-size, stack-allocated memory (`std::array`).
    

In both examples, the `BoundedQueue` is constructed with the appropriate storage policy, and the cleanup is handled automatically using `std::unique_ptr`.
