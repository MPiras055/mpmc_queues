#pragma once
#include <HeapStorage.hpp>
#include <atomic>
#include <cassert>
#include <type_traits>

namespace util::hazard {

/**
 * @brief A mutable pointer lookup table with atomic entries.
 *
 * Stores pointers in a HeapStorage of std::atomic<P>. Supports optional automatic
 * deletion of old pointers when replaced.
 *
 * @tparam P Pointer type to store.
 * @tparam forceDealloc If true, old pointers are deleted when replaced.
 */
template<typename P, bool forceDealloc = true>
class MutPtrLookup {
    static_assert(std::is_pointer_v<P>, "MutPtrLookup: P must be a pointer type");

public:
    /**
     * @brief Constructs a table of the given size, initializing all entries to nullptr.
     * @param size Number of entries in the table.
     */
    explicit MutPtrLookup(size_t size)
        : storage_(size)
    {
        for (size_t i = 0; i < storage_.capacity(); ++i) {
            storage_[i].store(nullptr, std::memory_order_relaxed);
        }
    }

    /**
     * @brief Constructs a table from an rvalue HeapStorage of pointers.
     *        Copies each pointer into atomic storage. Original HeapStorage
     *        is destroyed automatically when this constructor returns.
     * @param h Rvalue reference to HeapStorage<P> to consume.
     */
    explicit MutPtrLookup(util::memory::HeapStorage<P>&& h)
        : storage_(h.capacity())
    {
        for (size_t i = 0; i < h.capacity(); ++i) {
            storage_[i].store(h[i], std::memory_order_relaxed);
        }
        // h is automatically destroyed when out of scope
    }

    /**
     * @brief Move constructor.
     */
    MutPtrLookup(MutPtrLookup&& other) noexcept
        : storage_(std::move(other.storage_)) {}

    /**
     * @brief Move assignment operator.
     */
    MutPtrLookup& operator=(MutPtrLookup&& other) noexcept {
        if (this != &other) {
            storage_ = std::move(other.storage_);
        }
        return *this;
    }

    ~MutPtrLookup() {
        for (size_t i = 0; i < storage_.capacity(); i++) {
            delete storage_[i].load(std::memory_order_relaxed);
        }
    }

    /**
     * @brief Sets the pointer at the given index atomically.
     *        If forceDealloc is true, deletes the old pointer.
     * @param value New pointer value.
     * @param idx Index to set.
     */
    void setPtr(P value, size_t idx) {
        assert(idx < storage_.capacity());
        P old = storage_[idx].exchange(value, std::memory_order_release);
        if constexpr (forceDealloc) {
            delete old;
        }
    }

    /**
     * @brief Returns the pointer stored at the given index.
     * @param idx Index to query.
     * @return Stored pointer at the given index.
     */
    P getPtr(size_t idx) const {
        assert(idx < storage_.capacity());
        return storage_[idx].load(std::memory_order_acquire);
    }

private:
    util::memory::HeapStorage<std::atomic<P>> storage_;
};

/**
 * @brief Immutable pointer lookup table.
 *
 * Stores pointers in a HeapStorage<P> without atomic operations.
 *
 * @tparam P Pointer type to store.
 */
template<typename P>
class PtrLookup {
    static_assert(std::is_pointer_v<P>, "PtrLookup: P must be a pointer type");

public:
    /**
     * @brief Constructs a PtrLookup by moving a HeapStorage of pointers.
     * @param h Rvalue reference to HeapStorage<P> to consume.
     * 
     * @note Ownership of the storage is trasferred
     */
    explicit PtrLookup(util::memory::HeapStorage<P>&& h)
        : storage_(std::move(h)) {}

    /**
     * @brief Move constructor.
     */
    PtrLookup(PtrLookup&& other) noexcept
        : storage_(std::move(other.storage_)) {}

    /**
     * @brief Move assignment operator.
     */
    PtrLookup& operator=(PtrLookup&& other) noexcept {
        if (this != &other) {
            storage_ = std::move(other.storage_);
        }
        return *this;
    }

    ~PtrLookup(){
        for(size_t i = 0; i < storage_.capacity(); i++) {
            delete storage_[i];
        }
    }

    /**
     * @brief Returns the pointer stored at the given index.
     * @param idx Index to query.
     * @return Stored pointer at the given index.
     */
    inline P getPtr(size_t idx) const {
        return storage_[idx];
    }

private:
    util::memory::HeapStorage<P> storage_;
};

} // namespace util::hazard
