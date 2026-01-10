#pragma once
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdlib>      // for aligned_alloc, free
#include <cstdint>
#include <cstring>
#include <new>          // for placement new
#include <specs.hpp>
#include <bit>
#include <ILinkedSegment.hpp>
#include <OptionsPack.hpp>
#include <type_traits>

namespace queue::segment {

template<typename T, typename Proxy, typename Opt = meta::EmptyOptions, typename NextT = void>
class LinkedFAAArray:
    public base::ILinkedSegment<
        T,std::conditional_t<
            std::is_void_v<NextT>,
            LinkedFAAArray<T,Proxy,Opt,NextT>*,
            NextT
        >
    >
{
    static_assert(std::is_pointer_v<T>,"LinkedFAAArray requires T to be pointer type");
    using Next = std::conditional_t<std::is_void_v<NextT>,LinkedFAAArray<T,Proxy,Opt,NextT>*,NextT>;
    static_assert(std::is_trivially_default_constructible_v<Next>,"NextT field must be default trivially constructible");
    using Cell = std::atomic<uintptr_t>;
    friend Proxy;

    static constexpr Next NULL_NODE = Next{};
    static constexpr size_t MAX_PATIENCE = 4*1024;
    static constexpr bool optimized_alloc = true;

    static constexpr uintptr_t EMPTY    = 0;
    static constexpr uintptr_t SEEN     = 1;

    static inline bool reserved(uintptr_t item) {
        return item <= 1;
    }

    static inline bool reserved(T item) {
        return reserved(std::bit_cast<uintptr_t>(item));
    }

    // =========================================================================
    // 1. OPTIMIZED MEMORY LAYOUT
    // =========================================================================
    // We group all read-only/cold data at the very top.
    // This fits into the first cache line along with the vptr (8 bytes).

    const bool      owns_buffer_; // Memory ownership flag
    const uint64_t  offset;
    const size_t    size;
    Cell* buffer;

    // =========================================================================
    // 2. HOT DATA (Cache Line Separated)
    // =========================================================================

    // Tail on its own cache line (Enqueuer contention)
    ALIGNED_CACHE std::atomic_uint64_t tail;
    CACHE_PAD_TYPES(std::atomic_uint64_t);

    // Head on its own cache line (Dequeuer contention)
    ALIGNED_CACHE std::atomic_uint64_t head;
    CACHE_PAD_TYPES(std::atomic_uint64_t);

    // Next pointer on its own cache line (Structure modification)
    ALIGNED_CACHE std::atomic<Next> next_{};
    CACHE_PAD_TYPES(std::atomic<Next>);


    // =========================================================================
    // 3. INTERNAL MECHANICS
    // =========================================================================
    struct CoAllocTag {};

    static Cell* compute_buffer_addr(void* self) {
        return reinterpret_cast<Cell*>(reinterpret_cast<char*>(self) + sizeof(LinkedFAAArray));
    }

    // Helper to initialize buffer slots (avoids code duplication)
    void init_buffer_slots() {
        if constexpr (EMPTY == uintptr_t{0}) {
             // For atomics, memset to 0 is valid for relaxed/uninitialized state on most platforms,
             // but strictly speaking we should construct them.
             // Since we might be in raw memory, we use placement new.
             for(size_t i = 0 ; i < size; i++) {
                 new (&buffer[i]) Cell(EMPTY);
             }
        } else {
             for(size_t i = 0 ; i < size; i++) {
                 new (&buffer[i]) Cell(EMPTY);
             }
        }
    }

public:
    // =========================================================================
    // 4. FACTORY METHOD (Co-Allocation)
    // =========================================================================
    static LinkedFAAArray* create(size_t s, uint64_t start = 0) {
        assert(s != 0 && "Size must be non-null");

        // Calculate total bytes
        size_t total_bytes = sizeof(LinkedFAAArray) + (sizeof(Cell) * s);

        // Align to cache line
        if(total_bytes % CACHE_LINE != 0)
            total_bytes += CACHE_LINE - (total_bytes % CACHE_LINE);

        // Allocate single block
        void* mem = std::aligned_alloc(alignof(LinkedFAAArray), total_bytes);
        assert(mem != nullptr && "Failed aligned_alloc");

        // Construct using optimized constructor
        return ::new (mem) LinkedFAAArray(CoAllocTag{}, s, start);
    }

    // =========================================================================
    // 5. MEMORY OPERATORS
    // =========================================================================
    static void operator delete(void* ptr) {
        std::free(ptr);
    }

    static void* operator new(size_t size) {
        void* ptr = std::aligned_alloc(alignof(LinkedFAAArray), size);
        if (!ptr) throw std::bad_alloc();
        return ptr;
    }

    // =========================================================================
    // 6. CONSTRUCTORS
    // =========================================================================

    // Standard Constructor (Separate Allocation)
    explicit LinkedFAAArray(size_t s, uint64_t start = 0):
        offset{start}, size{s}, owns_buffer_{true}, tail{0}, head{0}
    {
        // Allocate buffer manually to ensure cache alignment
        size_t bytes = sizeof(Cell) * s;
        if(bytes % CACHE_LINE != 0) bytes += CACHE_LINE - (bytes % CACHE_LINE);

        buffer = static_cast<Cell*>(std::aligned_alloc(CACHE_LINE, bytes));
        assert(buffer != nullptr);

        init_buffer_slots();
    }

    // Standard Constructor with Item
    explicit LinkedFAAArray(T item, size_t s, uint64_t start = 0):
        offset{start}, size{s}, owns_buffer_{true}, tail{1}, head{0}
    {
        assert(!reserved(item) && "Cannot insert item EMPTY (*0) or SEEN (*1)");

        size_t bytes = sizeof(Cell) * s;
        if(bytes % CACHE_LINE != 0) bytes += CACHE_LINE - (bytes % CACHE_LINE);

        buffer = static_cast<Cell*>(std::aligned_alloc(CACHE_LINE, bytes));
        assert(buffer != nullptr);

        init_buffer_slots();
        buffer[0].store(std::bit_cast<uintptr_t>(item), std::memory_order_release);
    }

    // Destructor
    ~LinkedFAAArray() {
        if(owns_buffer_) {
            std::free(buffer);
        }
    }

private:
    // Optimized Constructor (Co-Allocated)
    LinkedFAAArray(CoAllocTag, size_t s, uint64_t start) :
        offset{start}, size{s}, buffer{compute_buffer_addr(this)}, owns_buffer_{false},
        tail{0}, head{0}
    {
        init_buffer_slots();
    }

public:
    // ... [Logic remains unchanged] ...

    bool enqueue(const T item, [[maybe_unused]] bool info = true) noexcept final override {
        assert(!reserved(item) && "Cannot enqueue EMPTY (*0) or SEEN (*1)");
        while(true) {
            uintptr_t empty = EMPTY;
            uint64_t t = tail.fetch_add(1,std::memory_order_acq_rel);
            if(t >= size) {
                return false;
            } if(buffer[t].compare_exchange_strong(empty,std::bit_cast<uintptr_t>(item),std::memory_order_release,std::memory_order_relaxed)) {
                return true;
            }
        }
    }

    bool dequeue(T& out, [[maybe_unused]] bool info = true) noexcept final override {
        uintptr_t item;
        while(true) {
            uint64_t h = head.fetch_add(1,std::memory_order_acq_rel);
            if(h >= size) {
                return false;
            }
            Cell& c = buffer[h];
            if( (c.load(std::memory_order_acquire) == EMPTY) &&
                h < head.load(std::memory_order_acquire)
            ) {
                //try not to invalidate the cell
                for (size_t i = 0; i < MAX_PATIENCE; ++i) {
                    if (c.load(std::memory_order_acquire) != EMPTY)
                        break;
                }
            }
            item = c.exchange(SEEN,std::memory_order_acq_rel);
            if(item != EMPTY) {
                out = reinterpret_cast<T>(item);
                return true;
            }
        }
    }

    inline Next getNext() const noexcept final override {
        return next_.load(std::memory_order_acquire);
    }


    inline bool close() noexcept final override {
        tail.fetch_add(size,std::memory_order_release);
        return true;
    }

    inline bool open() noexcept final override {
        head.store(0,std::memory_order_relaxed);
        tail.store(0,std::memory_order_release);
        return true;
    }

    inline bool isClosed() const noexcept final override {
        return (tail.load(std::memory_order_acquire)) >= size;
    }

    inline bool isOpened() const noexcept final override {
        return !isClosed();
    }
};

} //namespace queue::segment
