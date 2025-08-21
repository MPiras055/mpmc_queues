#pragma once
#include <cstddef>
#include <IStoragePolicy.hpp>

/**
 * @brief Stack-based storage implementation for queue buffers.
 * 
 * Uses a fixed-size array allocated on the stack to store elements.
 * Implements the IStoragePolicy interface.
 * 
 * @tparam T Type of elements stored in the buffer.
 * @tparam N Size of the stack-allocated buffer.
 */
template<typename T, size_t N>
class StackStorage: public IStoragePolicy<T> {
public:
    /**
     * @brief Default constructor.
     * 
     * Initializes the stack-allocated buffer.
     */
    StackStorage() = default;

    /**
     * @brief Returns a pointer to the underlying buffer.
     * 
     * @return Pointer to the stack-allocated array.
     */
    T* data() override {
        return buffer_;
    }

    /**
     * @brief Returns the capacity of the storage buffer.
     * 
     * @return Maximum number of elements that can be stored (N).
     */
    size_t capacity() const override {
        return N;
    }

    /**
     * @brief Default destructor.
     */
    ~StackStorage() override = default;

private:
    T buffer_[N]; ///< Fixed-size stack-allocated buffer
};
