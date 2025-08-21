#pragma once
#include <IStoragePolicy.hpp>

/**
 * @brief Heap-based storage implementation for queue buffers.
 * 
 * Allocates a dynamic buffer on the heap to store elements.
 * Implements the IStoragePolicy interface.
 * 
 * @tparam T Type of elements stored in the buffer.
 */
template<typename T>
class HeapStorage: public IStoragePolicy<T> {
public:
    /**
     * @brief Constructs a heap storage buffer with the specified capacity.
     * 
     * @param capacity Number of elements the buffer can hold.
     */
    explicit HeapStorage(size_t capacity)
        : capacity_(capacity), buffer_(new T[capacity]) {}

    /**
     * @brief Destructor that deallocates the heap buffer.
     */
    ~HeapStorage() override {
        delete[] buffer_;
    }

    /**
     * @brief Returns a pointer to the underlying buffer.
     * 
     * @return Pointer to the dynamically allocated buffer.
     */
    T* data() override {
        return buffer_;
    }

    /**
     * @brief Returns the capacity of the storage buffer.
     * 
     * @return Maximum number of elements that can be stored.
     */
    size_t capacity() const override {
        return capacity_;
    }

private:
    const size_t capacity_; ///< Maximum number of elements the buffer can hold
    T* buffer_;             ///< Pointer to the dynamically allocated buffer
};
