#pragma once
#include <IStoragePolicy.hpp>
#include <stdexcept>

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
class HeapStorage: public IStoragePolicy<T> {
public:
    /**
     * @brief Constructs a heap storage buffer with the specified capacity.
     * 
     * Allocates a dynamic array of size @p capacity on the heap.
     * 
     * @param capacity Number of elements the buffer can hold.
     * 
     * @throws std::invalid_argument If @p capacity is zero.
     * @throws std::bad_alloc If memory allocation fails.
     */
    explicit HeapStorage(size_t capacity)
        : capacity_(capacity), buffer_(new T[capacity]){
            if (capacity_ == 0)
                throw std::invalid_argument("HeapStorage requires non-null capacity"); 
        }

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

    /**
     * @brief Provides non-const access to an element by index.
     * 
     * @param index Position of the element in the buffer.
     * @return Reference to the element at the given index.
     * 
     * @throws std::out_of_range If @p index is greater or equal to capacity.
     */
    T& operator[](size_t index) {
#ifndef NDEBUG
        if (index >= capacity_)
            throw std::out_of_range("HeapStorage index out of range");
#endif
        return buffer_[index];
    }

    /**
     * @brief Provides const access to an element by index.
     * 
     * @param index Position of the element in the buffer.
     * @return Const reference to the element at the given index.
     * 
     * @throws std::out_of_range If @p index is greater or equal to capacity.
     */
    const T& operator[](size_t index) const {
#ifndef NDEBUG
        if (index >= capacity_)
            throw std::out_of_range("HeapStorage index out of range");
#endif
        return buffer_[index];
    }

private:
    const size_t capacity_; ///< Maximum number of elements the buffer can hold
    T* buffer_;             ///< Pointer to the dynamically allocated buffer
};

} //namespace util::memory
