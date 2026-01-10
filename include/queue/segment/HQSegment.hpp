// #pragma once
// #include <atomic>
// #include <cassert>
// #include <cstddef>
// #include <cstdlib>
// #include <cstdint>
// #include <cstring>
// #include <specs.hpp>
// #include <bit>
// #include <ILinkedSegment.hpp>
// #include <OptionsPack.hpp>

// //
// // Add the option to flip the queue in case of an opening
// // Use the msb of head and tail to flip the meaning of SEEN and EMPTY
// // In this case we can devise a isEmpty check that checks if the msb is setted and return seen or empty in that case
// //
// // Any dequeue shall not wait for any pending (straggler enqueue). We should use exchange(SEEN) to determine whether
// // the enqueue was actually placed or not
// //
// namespace queue::segment {

// template<typename T, typename Proxy, typename Opt = meta::EmptyOptions, typename NextT = void>
// class LinkedHQ:
//     public base::ILinkedSegment<
//         T,std::conditional_t<
//             std::is_void_v<NextT>,
//             LinkedHQ<T,Proxy,Opt,NextT>*,
//             NextT
//         >
//     >
// {
//     static_assert(std::is_pointer_v<T>,"LinkedHQ requires T to be pointer type");
//     using Next = std::conditional_t<std::is_void_v<NextT>,LinkedHQ<T,Proxy,Opt,NextT>*,NextT>;
//     using Cell = std::atomic<uintptr_t>;
//     friend Proxy;

//     static constexpr Next NULL_NODE = Next{};   //the default next value is nullptr

//     static constexpr uintptr_t EMPTY    = 0;
//     static constexpr uintptr_t SEEN     = 1;   //T must be pointer type
//     static constexpr size_t MAX_PATIENCE = 4*1024; //max retries when waiting for a straggler

//     static inline bool reserved(uintptr_t item) {
//         return item <= 1;
//     }

//     //used to check wheter the item is EMPTY or SEEN
//     static inline bool reserved(T item) {
//         return reserved(std::bit_cast<uintptr_t>(item));
//     }

//     static Cell* init_storage(size_t s) {
//         assert(s != 0 && "Size must be non-null");
//         size_t bytes = sizeof(Cell) * s;
//         if(bytes % CACHE_LINE != 0)
//             bytes += CACHE_LINE - (bytes % CACHE_LINE);

//         Cell* buffer = static_cast<Cell*>(std::aligned_alloc(CACHE_LINE,bytes));
//         assert(buffer !=  nullptr && "Failed aligned_alloc");
//         if constexpr (EMPTY == uintptr_t{0}) {
//             buffer = static_cast<Cell*>(std::memset(buffer,'\0',bytes));
//         } else {
//             for(size_t i = 0 ; i < s; i++) buffer[i].store(EMPTY,std::memory_order_relaxed);

//         }
//         return buffer;
//     }

// public:
// explicit LinkedHQ(size_t s, uint64_t start = 0):
//     offset{start}, size{s}, tail{0}, head{0}, buffer{init_storage(s)} {}

// explicit LinkedHQ(T item, size_t s, uint64_t start = 0):
//     offset{start}, size{s}, tail{1}, head{0}, buffer{init_storage(s)} {
//         assert(!reserved(item) && "Cannot insert item EMPTY (*0) or SEEN (*1)");
//         buffer[0].store(std::bit_cast<uintptr_t>(item),std::memory_order_release);
//     }

//     ~LinkedHQ() {
//         std::free(buffer);
//     }

//     /**
//      * @brief installs an element inside the segment
//      *
//      * @tparam item: item to be installed
//      * @param bool info: hint [[UNUSED]]
//      *
//      * @warning: item has to be different than 0 or 1 (RESERVED)
//      *
//      * @note: FAA guarantees slots to be unique so only enqueue/dequeue races can
//      * happen (can use SWAP instead of CAS)
//      */
//     bool enqueue(const T item, [[maybe_unused]] bool info = false) noexcept final override {
//         assert(!reserved(item) && "Cannot enqueue EMPTY (*0) or SEEN (*1)");
//         uintptr_t empty = EMPTY;
//         while(true) {
//             uint64_t t = tail.fetch_add(1,std::memory_order_acq_rel);
//             if(t >= size) {
//                 return false;
//             } else if (buffer[t].compare_exchange_strong(empty,std::bit_cast<uintptr_t>(item))) {
//                 return true;
//             }
//         }
//     }

//     bool dequeue(T& item, [[maybe_unused]] bool info = false) noexcept final override {
//         return getNext() != NULL_NODE?
//             fastDequeue(item) :
//             slowDequeue(item);
//     }

//     inline Next getNext() const noexcept final override {
//         return next_.load(std::memory_order_acquire);
//     }

//     inline bool close() noexcept final override {
//         tail.fetch_add(size,std::memory_order_release);
//         return true;
//     }

//     inline bool open() noexcept final override {
//         head.store(0,std::memory_order_relaxed);
//         tail.store(0,std::memory_order_release);
//         return true;
//     }

//     inline bool isClosed() const noexcept final override {
//         return (tail.load(std::memory_order_acquire)) >= size;
//     }

//     inline bool isOpened() const noexcept final override {
//         return !isClosed();
//     }


// private:
//     bool fastDequeue(T& out) {
//         while(true) {
//             uint64_t h = head.fetch_add(1,std::memory_order_acq_rel);
//             if(h >= size) {
//                 return false;
//             }
//             size_t i = 0;

//             //wait for any straggler
//             for(;i++ < MAX_PATIENCE && buffer[h].load(std::memory_order_relaxed) == EMPTY;);
//             uintptr_t cp = buffer[h].exchange(SEEN,std::memory_order_release);
//             if(!reserved(cp)) {
//                 out = reinterpret_cast<T>(cp);
//                 return true;
//             }
//         }
//     }

//     bool slowDequeue(T& out) {
//         while(true) {
// start:      uint64_t h,t;
//             h = head.load(std::memory_order_relaxed);
//             if(h >= size)   //out of range
//                 return false;
//             uintptr_t item = buffer[h].load(std::memory_order_acquire);
//             t = tail.load(std::memory_order_acquire);

//             if(h != head.load(std::memory_order_acquire))
//                 continue;
//             else if(h == t) //segment actually empty
//                 return false;
//             if(item == SEEN) {  //item already consumed
//                 (void) head.compare_exchange_weak(h,h+1,std::memory_order_relaxed);
//                 continue;
//             }

//             if(item == EMPTY) { //item not yet installed
//                 for(size_t i= 0; i< MAX_PATIENCE; i++) {
//                     //wait for stragglers
//                     item = buffer[h].load(std::memory_order_acquire);
//                     if(item == SEEN) {
//                         (void) head.compare_exchange_weak(h,h+1,std::memory_order_relaxed);
//                         goto start;
//                     }
//                     else if(item != EMPTY)
//                         break;
//                 }
//             }
//             //item installed or MAX_PATIENCE reached

//             // try to get the content of the buffer (race with one enqueuer and potentially all dequeuers)
//             // potentially invalidates a slot for an enqueuer (makes the method obstruction-free)
//             item = buffer[h].exchange(SEEN,std::memory_order_relaxed);
//             // at this point item can either be EMPTY | SEEN | CONSUMABLE

//             // help advance head
//             (void) head.compare_exchange_weak(h,h+1,std::memory_order_relaxed);
//             if(!reserved(item)) {
//                 out = reinterpret_cast<T>(item);
//                 return true;
//             }
//         }
//     }


//     const uint64_t  offset;
//     const size_t    size;
//     ALIGNED_CACHE std::atomic_uint64_t tail;
//     CACHE_PAD_TYPES(std::atomic_uint64_t);
//     ALIGNED_CACHE std::atomic_uint64_t head;
//     CACHE_PAD_TYPES(std::atomic_uint64_t);
//     ALIGNED_CACHE std::atomic<Next> next_{};
//     CACHE_PAD_TYPES(std::atomic<Next>);
//     Cell* buffer;
// };

// } //namespace queue
//
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

namespace queue::segment {

template<typename T, typename Proxy, typename Opt = meta::EmptyOptions, typename NextT = void>
class LinkedHQ:
    public base::ILinkedSegment<
        T,std::conditional_t<
            std::is_void_v<NextT>,
            LinkedHQ<T,Proxy,Opt,NextT>*,
            NextT
        >
    >
{
    static_assert(std::is_pointer_v<T>,"LinkedHQ requires T to be pointer type");
    using Next = std::conditional_t<std::is_void_v<NextT>,LinkedHQ<T,Proxy,Opt,NextT>*,NextT>;
    using Cell = std::atomic<uintptr_t>;
    friend Proxy;

    static constexpr Next NULL_NODE = Next{};
    static constexpr uintptr_t EMPTY    = 0;
    static constexpr uintptr_t SEEN     = 1;
    static constexpr size_t MAX_PATIENCE = 4*1024;
    static constexpr bool optimized_alloc = true;

    static inline bool reserved(uintptr_t item) {
        return item <= 1;
    }

    static inline bool reserved(T item) {
        return reserved(std::bit_cast<uintptr_t>(item));
    }

    // =========================================================================
    // 1. OPTIMIZED MEMORY LAYOUT
    // =========================================================================
    // Pack read-only/cold data at the top to share the first cache line with vptr.

    const uint64_t  offset;
    const size_t    size;
    Cell* buffer;
    const bool      owns_buffer_; // Ownership flag

    // =========================================================================
    // 2. HOT DATA (Cache Line Separated)
    // =========================================================================

    // Tail: Modified by enqueuers
    ALIGNED_CACHE std::atomic_uint64_t tail;
    CACHE_PAD_TYPES(std::atomic_uint64_t);

    // Head: Modified by dequeuers
    ALIGNED_CACHE std::atomic_uint64_t head;
    CACHE_PAD_TYPES(std::atomic_uint64_t);

    // Next: Modified only when linking new segments
    ALIGNED_CACHE std::atomic<Next> next_{};
    CACHE_PAD_TYPES(std::atomic<Next>);

    // =========================================================================
    // 3. INTERNAL MECHANICS
    // =========================================================================
    struct CoAllocTag {};

    static Cell* compute_buffer_addr(void* self) {
        return reinterpret_cast<Cell*>(reinterpret_cast<char*>(self) + sizeof(LinkedHQ));
    }

    // Helper to initialize buffer slots (replaces init_storage)
    void init_buffer_slots() {
        assert(size != 0 && "Size must be non-null");

        // Note: For raw memory from aligned_alloc, we technically should use placement new
        // to start the lifetime of atomic objects, even if treating them as POD often works.
        if constexpr (EMPTY == uintptr_t{0}) {
             // Zeroing memory is equivalent to initializing atomics to 0 (EMPTY)
             std::memset(buffer, 0, sizeof(Cell) * size);
        } else {
             for(size_t i = 0 ; i < size; i++) {
                 // Construct the atomic in-place with the EMPTY value
                 new (&buffer[i]) Cell(EMPTY);
             }
        }
    }

public:
    // =========================================================================
    // 4. FACTORY METHOD (Co-Allocation)
    // =========================================================================
    static LinkedHQ* create(size_t s, uint64_t start = 0) {
        assert(s != 0 && "Size must be non-null");

        size_t total_bytes = sizeof(LinkedHQ) + (sizeof(Cell) * s);

        // Cache Line Alignment
        if(total_bytes % CACHE_LINE != 0)
            total_bytes += CACHE_LINE - (total_bytes % CACHE_LINE);

        void* mem = std::aligned_alloc(alignof(LinkedHQ), total_bytes);
        assert(mem != nullptr && "Failed aligned_alloc");

        return ::new (mem) LinkedHQ(CoAllocTag{}, s, start);
    }

    // =========================================================================
    // 5. MEMORY OPERATORS
    // =========================================================================
    static void operator delete(void* ptr) {
        std::free(ptr);
    }

    static void* operator new(size_t size) {
        void* ptr = std::aligned_alloc(alignof(LinkedHQ), size);
        if (!ptr) throw std::bad_alloc();
        return ptr;
    }

    // =========================================================================
    // 6. CONSTRUCTORS
    // =========================================================================

    // Standard Constructor
    explicit LinkedHQ(size_t s, uint64_t start = 0):
        offset{start}, size{s}, owns_buffer_{true}, tail{0}, head{0}
    {
        // Allocate separate buffer
        size_t bytes = sizeof(Cell) * s;
        if(bytes % CACHE_LINE != 0) bytes += CACHE_LINE - (bytes % CACHE_LINE);

        buffer = static_cast<Cell*>(std::aligned_alloc(CACHE_LINE, bytes));
        assert(buffer != nullptr);

        init_buffer_slots();
    }

    // Standard Constructor with Item
    explicit LinkedHQ(T item, size_t s, uint64_t start = 0):
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

    ~LinkedHQ() {
        if(owns_buffer_) {
            std::free(buffer);
        }
    }

private:
    // Optimized Constructor (Co-Allocated)
    LinkedHQ(CoAllocTag, size_t s, uint64_t start) :
        offset{start}, size{s}, buffer{compute_buffer_addr(this)}, owns_buffer_{false},
        tail{0}, head{0}
    {
        init_buffer_slots();
    }

public:
    // ... [Logic methods remain unchanged] ...

    bool enqueue(const T item, [[maybe_unused]] bool info = false) noexcept final override {
        assert(!reserved(item) && "Cannot enqueue EMPTY (*0) or SEEN (*1)");
        uintptr_t empty = EMPTY;
        while(true) {
            uint64_t t = tail.fetch_add(1,std::memory_order_acq_rel);
            if(t >= size) {
                return false;
            } else if (buffer[t].compare_exchange_strong(empty,std::bit_cast<uintptr_t>(item))) {
                return true;
            }
        }
    }

    bool dequeue(T& item, [[maybe_unused]] bool info = false) noexcept final override {
        return getNext() != NULL_NODE?
            fastDequeue(item) :
            slowDequeue(item);
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

private:
    bool fastDequeue(T& out) {
        while(true) {
            uint64_t h = head.fetch_add(1,std::memory_order_acq_rel);
            if(h >= size) {
                return false;
            }
            size_t i = 0;

            //wait for any straggler
            for(;i++ < MAX_PATIENCE && buffer[h].load(std::memory_order_relaxed) == EMPTY;);
            uintptr_t cp = buffer[h].exchange(SEEN,std::memory_order_release);
            if(!reserved(cp)) {
                out = reinterpret_cast<T>(cp);
                return true;
            }
        }
    }

    bool slowDequeue(T& out) {
        while(true) {
start:      uint64_t h,t;
            h = head.load(std::memory_order_relaxed);
            if(h >= size)   //out of range
                return false;
            uintptr_t item = buffer[h].load(std::memory_order_acquire);
            t = tail.load(std::memory_order_acquire);

            if(h != head.load(std::memory_order_acquire))
                continue;
            else if(h == t) //segment actually empty
                return false;
            if(item == SEEN) {  //item already consumed
                (void) head.compare_exchange_weak(h,h+1,std::memory_order_relaxed);
                continue;
            }

            if(item == EMPTY) { //item not yet installed
                for(size_t i= 0; i< MAX_PATIENCE; i++) {
                    //wait for stragglers
                    item = buffer[h].load(std::memory_order_acquire);
                    if(item == SEEN) {
                        (void) head.compare_exchange_weak(h,h+1,std::memory_order_relaxed);
                        goto start;
                    }
                    else if(item != EMPTY)
                        break;
                }
            }
            //item installed or MAX_PATIENCE reached

            // try to get the content of the buffer (race with one enqueuer and potentially all dequeuers)
            // potentially invalidates a slot for an enqueuer (makes the method obstruction-free)
            item = buffer[h].exchange(SEEN,std::memory_order_relaxed);
            // at this point item can either be EMPTY | SEEN | CONSUMABLE

            // help advance head
            (void) head.compare_exchange_weak(h,h+1,std::memory_order_relaxed);
            if(!reserved(item)) {
                out = reinterpret_cast<T>(item);
                return true;
            }
        }
    }
};

} //namespace queue::segment
