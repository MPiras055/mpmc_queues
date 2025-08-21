#pragma once

/**
 * @brief Abstract base class for storage policies.
 * 
 * This interface defines the contract for storage policies used
 * by queues. Storage policies abstract how elements are stored
 * (e.g., heap-allocated or stack-allocated buffers).
 * 
 * @tparam T Type of elements stored in the buffer.
 */
template <typename T>
class IStoragePolicy {
public:

    /**
     * @brief Returns a pointer to the underlying storage buffer.
     * 
     * @return Pointer to the array of elements.
     */
    virtual T* data() = 0;

    /**
     * @brief Returns the total capacity of the storage buffer.
     * 
     * @return Maximum number of elements the buffer can hold.
     */
    virtual size_t capacity() const = 0;

    /**
     * @brief Virtual destructor for proper cleanup of derived classes.
     */
    virtual ~IStoragePolicy() = default;
};
