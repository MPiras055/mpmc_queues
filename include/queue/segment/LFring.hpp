#pragma once
#include <IQueue.hpp>
#include <bit.hpp>
#include <type_traits>
#include <cstdlib>
#include <cstdio>
#include <cassert>
#include <new>
#include <specs.hpp>
#include <cstring>


extern "C" {
#include "lfring.h"
#include "lfring_cas1.h"
}

namespace queue {

    template<typename T>
    class alignas(CACHE_LINE) LFring : public base::IQueue<T> {
    public:
        static_assert(std::is_convertible_v<T, size_t>, "T must be convertible to size_t");
        using lfring_legacy = struct lfring;

        // =========================================================================
        // 1. MEMORY MANAGEMENT
        // =========================================================================

        static void operator delete(void* ptr) {
            std::free(ptr);
        }

        static void* operator new(size_t size) {
            void* ptr = std::aligned_alloc(CACHE_LINE, size);
            if (!ptr) throw std::bad_alloc();
            return ptr;
        }

        static void* operator new(size_t, void* ptr) noexcept {
            return ptr;
        }

        // =========================================================================
        // 2. FACTORY & ALIGNMENT LOGIC
        // =========================================================================

        static size_t bytes_needed(size_t size) {
            size_t order = order_(size);
            // We reserve exactly one CACHE_LINE for the C++ object metadata
            // and append the ring buffer immediately after.
            return CACHE_LINE + LFRING_SIZE(order);
        }

        static LFring* create(size_t size, void* memory = nullptr) {
            if (memory) {
                return ::new (memory) LFring(size, PlacementTag{});
            } else {
                size_t bytes = bytes_needed(size);
                void* block = std::aligned_alloc(CACHE_LINE, bytes);
                if (!block) throw std::bad_alloc();
                // Nikolaev's lfring relies on clean memory for some atomic states
                std::memset(block, 0, bytes);
                return ::new (block) LFring(size, PlacementTag{});
            }
        }

    private:
        // Helper to resolve the ring pointer based on allocation type
        inline lfring_legacy* get_ring() const {
            if (owns_buffer_) {
                return separate_ring_ptr_;
            }
            // Optimized/Slab: The ring starts exactly one cache line after 'this'
            return reinterpret_cast<lfring_legacy*>(
                reinterpret_cast<uintptr_t>(this) + CACHE_LINE
            );
        }

    public:
        // =========================================================================
        // 3. CONSTRUCTORS
        // =========================================================================

        struct PlacementTag {};

        // Standard Constructor (Stack/Heap)
        LFring(size_t size) :
            scq_order{order_(size)}, owns_buffer_{true}
        {
            size_t bytes = LFRING_SIZE(scq_order);
            void* buf = std::aligned_alloc(CACHE_LINE, bytes);
            if (!buf) throw std::bad_alloc();
            std::memset(buf, 0, bytes);

            separate_ring_ptr_ = static_cast<lfring_legacy*>(buf);
            lfring_init_empty(separate_ring_ptr_, scq_order);
        }

        // Placement Constructor (Optimized/Slab)
        LFring(size_t size, PlacementTag) :
            scq_order{order_(size)}, owns_buffer_{false}, separate_ring_ptr_{nullptr}
        {
            // get_ring() will correctly calculate the CACHE_LINE offset
            lfring_init_empty(get_ring(), scq_order);
        }

        ~LFring() override {
            if (owns_buffer_ && separate_ring_ptr_) {
                std::free(separate_ring_ptr_);
            }
        }

        // =========================================================================
        // 4. INTERFACE
        // =========================================================================

        bool enqueue(size_t item) noexcept final override {
            return lfring_enqueue(get_ring(), scq_order, item, false);
        }

        bool dequeue(size_t& item) noexcept final override {
            size_t i = lfring_dequeue(get_ring(), scq_order, false);
            if (i == LFRING_EMPTY) return false;
            item = i;
            return true;
        }

        size_t capacity() const noexcept final override {
            return (size_t)1 << scq_order;
        }

        size_t size() const final override {
            auto* r = get_ring();
            // Note: Ruslan's library uses internal head/tail accessors
            // This is a rough estimation as the header doesn't export a simple 'size'
            return 0; // Or implement via lfring_get_head/tail if available
        }

    private:
        static size_t order_(size_t s) {
            size_t o = bit::log2(s);
            // Ensure we round up if s is not a power of 2
            if (((size_t)1 << o) < s) o++;
            return o >= LFRING_MIN_ORDER ? o : LFRING_MIN_ORDER;
        }

        const size_t scq_order;
        bool owns_buffer_;
        lfring_legacy* separate_ring_ptr_; // Only used if owns_buffer_ == true
    };


/**
 * Optimization for Garbage Collection need of multiple queue
 *
 * With this we're able to perform a single memory allocation
 * for a variable number of queues
 */
template<typename T>
class LFringSlab {
public:
    /**
     * @brief Allocates a massive block for 'count' queues.
     * Each queue is guaranteed to be on a separate cache line.
     */
    LFringSlab(size_t count, size_t size_per_queue) : count_(count) {
        // 1. Get stride (Header + Buffer + Padding)
        stride_ = LFring<T>::bytes_needed(size_per_queue);

        // 2. Allocate Slab
        size_t total_bytes = stride_ * count_;
        memory_ = std::aligned_alloc(CACHE_LINE, total_bytes);
        if(!memory_) throw std::bad_alloc();

        // 3. Construct via Unified Factory (In-Place)
        uint8_t* cursor = static_cast<uint8_t*>(memory_);
        for(size_t i = 0; i < count_; ++i) {
            // Passing 'cursor' forces Case A (Placement New) inside create()
            LFring<T>::create(size_per_queue, cursor);
            cursor += stride_;
        }
    }

    ~LFringSlab() {
        // 1. Manually call destructors (since we used placement new)
        uint8_t* cursor = static_cast<uint8_t*>(memory_);
        for(size_t i = 0; i < count_; ++i) {
            reinterpret_cast<LFring<T>*>(cursor)->~LFring();
            cursor += stride_;
        }
        // 2. Free the slab memory
        std::free(memory_);
    }

    /**
     * @brief Get pointer to the i-th queue.
     * @warning DO NOT call 'delete' on this pointer. The Slab owns it.
     */
    LFring<T>* get(size_t index) {
        assert(index < count_);
        uint8_t* ptr = static_cast<uint8_t*>(memory_) + (index * stride_);
        return reinterpret_cast<LFring<T>*>(ptr);
    }

    size_t count() const { return count_; }

private:
    size_t count_;
    size_t stride_;
    void* memory_;
};


} // namespace queue
