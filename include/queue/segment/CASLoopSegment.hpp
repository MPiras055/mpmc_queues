#pragma once
#include <atomic>
#include <cassert>
#include <cstdlib>      // for aligned_alloc, free
#include <new>          // for placement new
#include <specs.hpp>    // padding and compatibility def
#include <IQueue.hpp>   // base queue interface
#include <ILinkedSegment.hpp> // base linked segment interface
#include <SequencedCell.hpp>  // cell definition
#include <bit.hpp>      // bit manipulation utilities
#include <OptionsPack.hpp>    // options

namespace queue {

/// Options for the queue
struct CASLoopOption {
  struct Pow2Size{};
  struct DisableCellPadding{};
  struct DisableAutoClose{};
};

// Forward declaration
template<typename T, typename Proxy, typename Opt, typename NextT>
class LinkedCASLoop;

/**
 * @brief Lock-free queue implementation using a compare-and-swap loop.
 *
 * @tparam T Type of elements stored in the queue.
 * @tparam OptionsPack<> list of options to customize the queue
 * @tparam Derived Type of the derived segment (CRTP) default void
 */
template<typename T, typename Opt = meta::EmptyOptions, typename Derived = void>
class CASLoopQueue: public base::IQueue<T> {
    static_assert(std::is_pointer_v<T>, "CASLoopQueue: non-pointer item type");

    using Effective = std::conditional_t<std::is_void_v<Derived>, CASLoopQueue, Derived>;
    static constexpr bool AUTO_CLOSE    = !Opt::template has<CASLoopOption::DisableAutoClose> &&
        base::is_linked_segment_v<Effective>;

    static constexpr bool PAD_CELL      = !Opt::template has<CASLoopOption::DisableCellPadding>;

protected:
    static constexpr bool POW2          = Opt::template has<CASLoopOption::Pow2Size>;
    using Cell = cell::SequencedCell<T,PAD_CELL>; ///< Internal buffer cell (value + sequence counter).

    // =========================================================================
    // 1. OWNERSHIP FLAG
    // =========================================================================
    const bool owns_buffer_;
    const size_t size_;
    const size_t mask_;
    Cell* array_; ///< Underlying circular buffer storage.

    inline size_t mod(uint64_t i) const noexcept{
        if constexpr (POW2) {
            return i & (mask_);
        } else {
            return i % size_;
        }
    }

public:
    // =========================================================================
    // 2. STANDARD CONSTRUCTOR (Allocates separate buffer)
    // =========================================================================
    CASLoopQueue(size_t size, uint64_t start = 0):
        owns_buffer_{true}, // We own the memory
        size_(POW2 && !bit::is_pow2(size)? bit::next_pow2(size) : size),
        mask_(POW2 && size_ != 1? (size_ - 1) : 0),
        array_{new Cell[size_]}
    {
        assert(size_ != 0 && "CASLoopQueue: null capacity");
        assert(!POW2 || mask_ != 0 && "CASLoopQueue: null bitmask");
        init_slots(start);
    }

    // Standard Constructor with item
    CASLoopQueue(T item, size_t size, uint64_t start = 0):
        CASLoopQueue(size, start)
    {
        // Inject initial item
        array_[mod(0)].val.store(item, std::memory_order_relaxed);
        array_[mod(0)].seq.store(start + 1,std::memory_order_relaxed);
        tail_.fetch_add(1,std::memory_order_release);
    }

    // =========================================================================
    // 3. DESTRUCTOR (Conditional Free)
    // =========================================================================
    ~CASLoopQueue() override {
        if(owns_buffer_) {
            delete[] array_;
        }
    }

protected:
    // =========================================================================
    // 4. CO-ALLOCATION CONSTRUCTOR (Uses injected buffer)
    // =========================================================================
    CASLoopQueue(size_t size, uint64_t start, Cell* raw_buffer) :
        owns_buffer_{false}, // We do NOT own the memory
        size_(POW2 && !bit::is_pow2(size)? bit::next_pow2(size) : size),
        mask_(POW2 && size_ != 1? (size_ - 1) : 0),
        array_{raw_buffer}
    {
        assert(size_ != 0 && "CASLoopQueue: null capacity");

        // Bless the raw memory as Cell objects
        for(size_t i = 0; i < size_; ++i) {
            new (&array_[i]) Cell();
        }
        init_slots(start);
    }

    void init_slots(uint64_t start) {
         for(uint64_t i = start; i < start + size_; i++) {
            array_[mod(i)].seq.store(i, std::memory_order_relaxed);
            // Ensure values are null (important for raw memory)
            array_[mod(i)].val.store(nullptr, std::memory_order_relaxed);
        }
        head_.store(start, std::memory_order_relaxed);
        tail_.store(start, std::memory_order_relaxed);
    }

public:
    // ... [Enqueue/Dequeue logic UNCHANGED] ...

    bool enqueue(T item) noexcept final override {
        uint64_t tailTicket, seq;
        size_t index;

        do {
            tailTicket = tail_.load(std::memory_order_relaxed);

            if constexpr (AUTO_CLOSE) {
                if (static_cast<Effective*>(this)->is_closed_(tailTicket)) {
                    return false;   //tail is closed
                }
            }

            index = mod(tailTicket);

            Cell& node = array_[index];
            seq = node.seq.load(std::memory_order_acquire);

            if (tailTicket == seq) {
                bool success = tail_.compare_exchange_weak(
                    tailTicket, tailTicket + 1,
                    std::memory_order_relaxed);
                //if cas was successful then update the entry
                if (success) {
                    node.val.store(item,std::memory_order_relaxed);
                    node.seq.store(seq + 1, std::memory_order_release);
                    return true;
                }

            } else if (tailTicket > seq) {
                if constexpr (AUTO_CLOSE) {     //attempt closing the current segment
                    if(static_cast<Effective*>(this)->close())
                        return false;
                }
            }
            //CAS failed: retry
        } while (true);
    }

    bool dequeue(T& container) noexcept final override {
        uint64_t headTicket, seq;
        size_t index;
        do {
            headTicket = head_.load(std::memory_order_relaxed);

            index = mod(headTicket);
            Cell& node = (array_[index]);
            seq  = node.seq.load(std::memory_order_acquire);

            int64_t diff = static_cast<int64_t>(seq) - static_cast<int64_t>(headTicket + 1);

            if(diff == 0) {
                if(head_.compare_exchange_weak(
                    headTicket, headTicket + 1,
                    std::memory_order_relaxed)) {
                    container = node.val.load(std::memory_order_acquire);
                    node.seq.store(headTicket + size_, std::memory_order_release);
                    return true;
                }
            } else if(diff < 0 && (size() == 0)){
                return false;
            }

        } while(true);
    }

    size_t capacity() const noexcept override { return size_; }

    size_t size() const noexcept override {
        return bit::clear_msb(tail_.load(std::memory_order_acquire)) - head_.load(std::memory_order_acquire);
    }

protected:
    ALIGNED_CACHE std::atomic_uint64_t head_; ///< Head ticket index for dequeue.
    CACHE_PAD_TYPES(std::atomic_uint64_t);
    ALIGNED_CACHE std::atomic_uint64_t tail_; ///< Tail ticket index for enqueue.
    CACHE_PAD_TYPES(std::atomic_uint64_t);

};


namespace segment {
/**
 * @brief Linked segment extension of CASLoopQueue.
 *
 * This class enables chaining multiple fixed-size CASLoopQueues into
 * a larger, virtually unbounded lock-free queue.
 *
 * @tparam T Type of elements stored in the queue.
 * @tparam Proxy Friend class allowed to access private members (e.g., higher-level queue).
 */
template<typename T, typename Proxy, typename Opt = meta::EmptyOptions, typename NextT = void>
class LinkedCASLoop:
    public CASLoopQueue<
        T,Opt,LinkedCASLoop<T,Proxy,Opt,NextT>
    >,
    public base::ILinkedSegment<
        T, std::conditional_t<
            std::is_void_v<NextT>,
            LinkedCASLoop<T,Proxy,Opt,NextT>*,
            NextT
        >
    >
{
    using Base = CASLoopQueue<T,Opt,LinkedCASLoop<T,Proxy,Opt,NextT>>;
    using Next = std::conditional_t<std::is_void_v<NextT>,LinkedCASLoop<T,Proxy,Opt,NextT>*,NextT>;
    using Cell = typename Base::Cell; // Import Cell type

    friend Base;    ///< Base class can access lifecycle methods
    friend Proxy;   ///< Proxy class can access private methods.

    static_assert(!Opt::template has<CASLoopOption::DisableAutoClose>,"LinkedCASLoop: AutoClose disabled");

    // =========================================================================
    // 5. INTERNAL MECHANICS FOR CO-ALLOCATION
    // =========================================================================
    struct CoAllocTag {};

    static Cell* compute_buffer_addr(void* self) {
        return reinterpret_cast<Cell*>(reinterpret_cast<char*>(self) + sizeof(LinkedCASLoop));
    }

public:
    static constexpr bool info_required = false; // Note: CASLoop usually doesn't need info hint, updated based on original
    static constexpr bool optimized_alloc = true;//proxies may want to optimize allocation as a single memory block
    // =========================================================================
    // 6. FACTORY METHOD (The "Smart" Allocator)
    // =========================================================================
    static LinkedCASLoop* create(size_t s, uint64_t start = 0) {
        // Size calculation logic
        size_t real_size = s;
        if constexpr (Base::POW2) {
             if(!bit::is_pow2(s)) real_size = bit::next_pow2(s);
        }

        size_t total_bytes = sizeof(LinkedCASLoop) + (sizeof(Cell) * real_size);

        // Cache Line Alignment
        if(total_bytes % CACHE_LINE != 0)
            total_bytes += CACHE_LINE - (total_bytes % CACHE_LINE);

        void* mem = std::aligned_alloc(alignof(LinkedCASLoop), total_bytes);
        if(!mem) throw std::bad_alloc();

        // Use global placement new with the Tagged Constructor
        return ::new (mem) LinkedCASLoop(CoAllocTag{}, real_size, start);
    }

    // =========================================================================
    // 7. MEMORY OPERATOR OVERRIDES
    // =========================================================================
    static void operator delete(void* ptr) {
        std::free(ptr);
    }

    static void* operator new(size_t size) {
        void* ptr = std::aligned_alloc(alignof(LinkedCASLoop), size);
        if (!ptr) throw std::bad_alloc();
        return ptr;
    }

    // =========================================================================
    // 8. CONSTRUCTORS
    // =========================================================================

    // Standard Constructors
    LinkedCASLoop(size_t size, uint64_t start = 0): Base(size,start) {}
    LinkedCASLoop(T item,size_t size, uint64_t start = 0): Base(item,size,start) {}

    ~LinkedCASLoop() override = default;

private:
    // Tagged Constructor (Co-Allocated Buffer)
    LinkedCASLoop(CoAllocTag, size_t size, uint64_t start)
        : Base(size, start, compute_buffer_addr(this))
    {}

public:
    // ... [Rest of Segment Logic UNCHANGED] ...

    Next getNext() const noexcept final override {
        return next_.load(std::memory_order_acquire);
    }

    static bool is_closed_(uint64_t tail) noexcept {
        return bit::get_msb(tail) != uint64_t{0};
    }

    bool open() noexcept final override {
        uint64_t tail = Base::tail_.load(std::memory_order_relaxed);
        if(bit::get_msb(tail) != 0) {
            uint64_t head = Base::head_.load(std::memory_order_relaxed);
            next_.store(nullptr,std::memory_order_relaxed);
            bool ok = Base::tail_.compare_exchange_strong(tail,head,std::memory_order_acq_rel);
            assert(ok && "LinkedCASLoopQueue: failed open - not exclusive ownership");
        }
        return true;
    }

    bool close() noexcept final override {
        Base::tail_.fetch_or(bit::set_msb(uint64_t{0}),std::memory_order_acq_rel);
        return true;
    }

    inline bool isClosed() const noexcept final override {
        return is_closed_(Base::tail_);
    }

    inline bool isOpened() const noexcept final override {
        return !isClosed();
    }

    inline bool enqueue(T item, [[maybe_unused]] bool info = true) noexcept final override {
        return Base::enqueue(item);
    }

    inline bool dequeue(T& item, [[maybe_unused]] bool info = true) noexcept final override {
        return Base::dequeue(item);
    }

    static_assert(detail::atomic_compatible_v<Next>,"LinkedCASLoop Next field: not lock free");
    static_assert(std::is_default_constructible_v<Next>,"LinkedCASLoop Next field: not default constructible");
    ALIGNED_CACHE std::atomic<Next> next_{};
    CACHE_PAD_TYPES(std::atomic<Next>);
};

}   //namespace segment

}   //namespace queue
