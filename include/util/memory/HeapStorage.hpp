#pragma once
#include <IStoragePolicy.hpp>
#include <stdexcept>
#include <new>
#include <utility>
#include <type_traits>

namespace util::memory {

/**
 * @brief Heap-based storage implementation for queue buffers.
 * 
 * Allocates a dynamic buffer on the heap to store elements.
 * Implements the IStoragePolicy interface.
 * 
 * @tparam T Type of elements stored in the buffer.
 */
template<typename T>
class HeapStorage : public IStoragePolicy<T> {
public:
    /**
     * @brief Constructs a heap storage buffer with the specified capacity.
     * 
     * Allocates raw memory for @p capacity elements and constructs each element
     * with the forwarded arguments.
     * 
     * @tparam Args Constructor arguments for T
     * @param capacity Number of elements
     * @param args Arguments to forward to T constructor
     * 
     * @throws std::invalid_argument If capacity is zero
     * @throws std::bad_alloc If allocation fails
     */
    template<typename... Args>
    explicit HeapStorage(size_t capacity, Args&&... args)
        : capacity_(capacity), buffer_(nullptr) {
        if (capacity_ == 0)
            throw std::invalid_argument("HeapStorage requires non-zero capacity");

        // Allocate aligned raw memory
        buffer_ = static_cast<T*>(
            ::operator new(sizeof(T) * capacity_, std::align_val_t(alignof(T)))
        );

        // Construct each element with forwarded arguments
        try {
            for (size_t i = 0; i < capacity_; ++i) {
                new (&buffer_[i]) T(std::forward<Args>(args)...);
            }
        } catch (...) {
            // If constructor throws, destroy constructed elements and free memory
            for (size_t i = 0; i < capacity_; ++i)
                buffer_[i].~T();
            ::operator delete(buffer_, std::align_val_t(alignof(T)));
            throw;
        }
    }

    // Disable copy
    HeapStorage(const HeapStorage&) = delete;
    HeapStorage& operator=(const HeapStorage&) = delete;

    // Enable move
    HeapStorage(HeapStorage&& other) noexcept
        : capacity_(other.capacity_), buffer_(other.buffer_) {
        other.buffer_ = nullptr;
    }

    HeapStorage& operator=(HeapStorage&& other) noexcept {
        if (this != &other) {
            destroy_buffer();
            buffer_ = other.buffer_;
            other.buffer_ = nullptr;
        }
        return *this;
    }

    ~HeapStorage() override {
        destroy_buffer();
    }

    T* data() override { return buffer_; }
    size_t capacity() const override { return capacity_; }

    T& operator[](size_t index) {
#ifndef NDEBUG
        if (index >= capacity_)
            throw std::out_of_range("HeapStorage index out of range");
#endif
        return buffer_[index];
    }

    const T& operator[](size_t index) const {
#ifndef NDEBUG
        if (index >= capacity_)
            throw std::out_of_range("HeapStorage index out of range");
#endif
        return buffer_[index];
    }

private:
    void destroy_buffer() noexcept {
        if (buffer_) {
            for (size_t i = 0; i < capacity_; ++i) {
                buffer_[i].~T();
            }
            ::operator delete(buffer_, std::align_val_t(alignof(T)));
            buffer_ = nullptr;
        }
    }

    const size_t capacity_;
    T* buffer_;
};

} // namespace util::memory
