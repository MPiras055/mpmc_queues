#pragma once
#include <cassert>
#include <type_traits>
#include <cstdlib>

namespace util::hazard::recycler {

/**
 * @brief A high-performance, immutable lookup table that owns a contiguous block of memory.
 *
 * This class allocates objects of type T contiguously to ensure maximum cache locality.
 * It provides pointer-arithmetic style access to these objects.
 *
 * @tparam T The value type of the objects stored (not the pointer type).
 */
template<typename T>
class ImmutablePtrLookup {
    // Basic sanity check
    static_assert(!std::is_pointer_v<T>, "ImmutablePtrLookup: T must be the value type, not a pointer");

public:
    /**
     * @brief Constructs the lookup table and initializes all objects in-place.
     *
     * Allocates a contiguous block of memory for 'size' elements and constructs
     * them using the provided arguments.
     *
     * @tparam Args Variadic types for the object constructor.
     * @param size Number of elements to allocate.
     * @param args Arguments forwarded to T's constructor for every element.
     */
    template<typename... Args>
    explicit ImmutablePtrLookup(size_t size, Args&&... args)
        : capacity_(size)
    {
        if (size == 0) {
            data_ = nullptr;
            return;
        }

        // Allocate raw memory
        // We use operator new[] to ensure we get raw bytes
        // Alignment relies on default allocator guarantees (max_align_t)
        void* raw_mem = ::operator new[](size * sizeof(T));
        data_ = static_cast<T*>(raw_mem);

        // Construct objects in-place (placement new)
        for (size_t i = 0; i < size; ++i) {
            new (&data_[i]) T(args...);
        }
    }

    /**
     * @brief Move constructor.
     * Transfers ownership of the memory block without copying.
     */
    ImmutablePtrLookup(ImmutablePtrLookup&& other) noexcept
        : data_(other.data_)
        , capacity_(other.capacity_)
    {
        other.data_ = nullptr;
        other.capacity_ = 0;
    }

    /**
     * @brief Move assignment.
     */
    ImmutablePtrLookup& operator=(ImmutablePtrLookup&& other) noexcept {
        if (this != &other) {
            cleanup(); // Destroy current contents
            data_ = other.data_;
            capacity_ = other.capacity_;
            other.data_ = nullptr;
            other.capacity_ = 0;
        }
        return *this;
    }

    // Disable copy to enforce unique ownership
    ImmutablePtrLookup(const ImmutablePtrLookup&) = delete;
    ImmutablePtrLookup& operator=(const ImmutablePtrLookup&) = delete;

    /**
     * @brief Destructor.
     * Manually calls destructors for all elements and frees raw memory.
     */
    ~ImmutablePtrLookup() {
        cleanup();
    }

    /**
     * @brief High-performance accessor behaving like pointer arithmetic.
     * * @param idx The index to access.
     * @return T* Pointer to the object at the given index.
     * * @note This function is marked const but returns a non-const pointer T*.
     * This implies the *structure* (pointers) is immutable, but the *objects* * pointed to might be mutable (internal mutability), which is common
     * in hazard pointer recycling chains.
     */
    [[nodiscard]] inline T* operator[](size_t idx) const noexcept {
        assert(idx < capacity_ && "PtrLookup: Index out of bounds");
        // Pure pointer arithmetic: base + offset
        return data_ + idx;
    }

    /**
     * @brief Returns the total capacity of the lookup table.
     */
    [[nodiscard]] inline size_t capacity() const noexcept {
        return capacity_;
    }

private:
    /**
     * @brief Internal cleanup helper.
     */
    void cleanup() {

    }

    T* __restrict data_; // __restrict hint (no aliasing)
    size_t capacity_;
};

} // namespace util::hazard::recycler
