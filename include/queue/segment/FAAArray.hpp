#pragma once
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <cstdint>
#include <cstring>
#include <specs.hpp>
#include <bit>
#include <ILinkedSegment.hpp>
#include <OptionsPack.hpp>
#include <type_traits>

//
// Add the option to flip the queue in case of an opening
// Use the msb of head and tail to flip the meaning of SEEN and EMPTY
// In this case we can devise a isEmpty check that checks if the msb is setted and return seen or empty in that case
//
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
    static_assert(std::is_pointer_v<T>,"LinkedHQ requires T to be pointer type"); //compatibility with uintptr_t
    using Next = std::conditional_t<std::is_void_v<NextT>,LinkedFAAArray<T,Proxy,Opt,NextT>*,NextT>;
    using Cell = std::atomic<uintptr_t>;
    friend Proxy;

    static constexpr Next NULL_NODE = Next{};       //the default next value is nullptr
    static constexpr size_t MAX_PATIENCE = 4*1024;  //max retries when waiting for a straggler

    static constexpr uintptr_t EMPTY    = 0;
    static constexpr uintptr_t SEEN     = 1;


    static inline bool reserved(uintptr_t item) {
        return item <= 1;
    }

    static inline bool reserved(T item) {
        return reserved(std::bit_cast<uintptr_t>(item));
    }

    static Cell* init_storage(size_t s) {
        assert(s != 0 && "Size must be non-null");
        size_t bytes = sizeof(Cell) * s;
        if(bytes % CACHE_LINE != 0)
            bytes += CACHE_LINE - (bytes % CACHE_LINE);

        Cell* buffer = static_cast<Cell*>(std::aligned_alloc(CACHE_LINE,bytes));
        assert(buffer !=  nullptr && "Failed aligned_alloc");
        if constexpr (EMPTY == uintptr_t{0}) {
            buffer = static_cast<Cell*>(std::memset(buffer,'\0',bytes));
        } else {
            for(size_t i = 0 ; i < s; i++) buffer[i].store(EMPTY,std::memory_order_relaxed);

        }
        return buffer;
    }

public:
explicit LinkedFAAArray(size_t s, uint64_t start = 0):
    offset{start}, size{s}, tail{0}, head{0}, buffer{init_storage(s)}
    {}

explicit LinkedFAAArray(T item, size_t s, uint64_t start = 0):
    offset{start}, size{s}, tail{1}, head{0}, buffer{init_storage(s)} {
        //store the item
        assert(!reserved(item) && "Cannot insert item EMPTY (*0) or SEEN (*1)");
        buffer[0].store(std::bit_cast<uintptr_t>(item), std::memory_order_release);
    }

    ~LinkedFAAArray() {
        std::free(buffer);
    }

    bool enqueue(const T item, [[maybe_unused]] bool info = true) noexcept final override {
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


private:
    const uint64_t  offset;
    const size_t    size;
    ALIGNED_CACHE std::atomic_uint64_t tail;
    CACHE_PAD_TYPES(std::atomic_uint64_t);
    ALIGNED_CACHE std::atomic_uint64_t head;
    CACHE_PAD_TYPES(std::atomic_uint64_t);
    ALIGNED_CACHE std::atomic<Next> next_{};
    CACHE_PAD_TYPES(std::atomic<Next>);
    Cell* buffer;
};

} //namespace queue
