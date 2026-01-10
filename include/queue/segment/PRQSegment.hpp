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
struct PRQOption {
    struct Pow2Size{};
    struct DisableCellPadding{};
    struct DisableAutoClose{};
};

/**
 * @brief Lock-free queue implementation using fetch-add loop
 *
 * @tparam T Type of elements stored in the queue
 */
template<typename T, typename Opt = meta::EmptyOptions, typename Derived = void>
class PRQueue: public base::IQueue<T> {
    static_assert(std::is_pointer_v<T>,"PRQueue: non-pointer item type");

    using Effective = std::conditional_t<std::is_void_v<Derived>,PRQueue,Derived>;
    static constexpr bool AUTO_CLOSE = !Opt::template has<PRQOption::DisableAutoClose> &&
        base::is_linked_segment_v<Effective>;

    static constexpr bool PAD_CELL =    !Opt::template has<PRQOption::DisableCellPadding>;

protected:
    static constexpr bool POW2 =        Opt::template has<PRQOption::Pow2Size>;
    using Cell = cell::SequencedCell<T,PAD_CELL>;

    // =========================================================================
    // 1. OWNERSHIP FLAG
    // =========================================================================
    const bool owns_buffer_;
    const size_t size_;
    const size_t mask_;
    Cell* array_;

    inline size_t mod(uint64_t i) const noexcept {
        if constexpr (POW2) {
            return i & mask_;
        } else {
            return i % size_;
        }
    }

public:
    // =========================================================================
    // 2. STANDARD CONSTRUCTOR (Allocates separate buffer)
    // =========================================================================
    explicit PRQueue(size_t size, uint64_t start = 0):
        owns_buffer_{true}, // We own the memory
        size_(POW2 && !bit::is_pow2(size)? bit::next_pow2(size) : size),
        mask_(POW2 && size_ != 1? (size_ - 1) : 0),
        array_{new Cell[size_]}
    {
        init_slots(start);
    }

    // Standard Constructor with initial item
     explicit PRQueue(T item, size_t size, uint64_t start = 0):
        owns_buffer_{true},
        size_(POW2 && !bit::is_pow2(size)? bit::next_pow2(size) : size),
        mask_(POW2 && size_ != 1? (size_ - 1) : 0),
        array_{new Cell[size_]}
    {
        init_slots(start);
        // Inject initial item
        array_[mod(start)].val.store(item,std::memory_order_relaxed);
        array_[mod(start)].seq.store(start + size_,std::memory_order_relaxed);
        head_.store(start, std::memory_order_relaxed);
        tail_.store(start + 1, std::memory_order_relaxed);
    }

    // =========================================================================
    // 3. DESTRUCTOR (Conditional Free)
    // =========================================================================
    ~PRQueue() override {
        if (owns_buffer_) {
            delete[] array_;
        }
        // If owns_buffer_ is false, we do nothing; the derived class/factory handles the block.
    };

protected:
    // =========================================================================
    // 4. CO-ALLOCATION CONSTRUCTOR (Uses injected buffer)
    // =========================================================================
    PRQueue(size_t size, uint64_t start, Cell* raw_buffer) :
        owns_buffer_{false}, // We do NOT own the memory
        size_(POW2 && !bit::is_pow2(size)? bit::next_pow2(size) : size),
        mask_(POW2 && size_ != 1? (size_ - 1) : 0),
        array_{raw_buffer}
    {
        for(size_t i = 0; i < size_; ++i) {
            new (&array_[i]) Cell();
        }
        init_slots(start);
    }

    // Helper to avoid duplication
    void init_slots(uint64_t start) {
        assert(size_ != 0 && "PRQueue: null capacity");
        assert(!POW2 || mask_ != 0 && "PRQueue: null bitmask");
        assert(bit::get_msb(start + size_) == 0ull && "PRQueue: sequence overflow");

        for(uint64_t i = start; i < start + size_; i++) {
            array_[mod(i)].seq.store(i, std::memory_order_relaxed);
            // Ensure value is clean (important for placement new case)
            array_[mod(i)].val.store(nullptr, std::memory_order_relaxed);
        }
        head_.store(start, std::memory_order_relaxed);
        tail_.store(start, std::memory_order_relaxed);
    }

public:
    // ... [Rest of Public Interface: enqueue, dequeue, size, etc. UNCHANGED] ...

    bool enqueue(T item) noexcept override {
        assert(item != nullptr && "Cannot insert nullptr");
        while(1) {
            uint64_t tailTicket = tail_.fetch_add(1,std::memory_order_relaxed);
            if constexpr(AUTO_CLOSE) {
                if(static_cast<Effective*>(this)->is_closed_(tailTicket)){
                    return false;
                }
            }
            T tagged = threadReserved();
            size_t tailIndex = mod(tailTicket);
            Cell& cell = array_[tailIndex];
            uint64_t seq = cell.seq.load(std::memory_order_relaxed);
            T val = cell.val.load(std::memory_order_acquire);
            if( (val == nullptr) &&
                (bit::clear_msb(seq) <= tailTicket) &&
                (!bit::get_msb(seq) || head_.load(std::memory_order_acquire) <= tailTicket)
            ) {
                if(cell.val.compare_exchange_strong(val,tagged)) {
                    if (cell.seq.compare_exchange_strong(seq,tailTicket + size_) &&
                        cell.val.compare_exchange_strong(tagged,item)
                    ) return true;
                } else
                    cell.val.compare_exchange_strong(tagged,T{nullptr});
            }
            if(tailTicket >= (head_.load(std::memory_order_acquire) + size_)) {
                if constexpr (AUTO_CLOSE) {
                    if(static_cast<Effective*>(this)->close())
                        return false;
                }
            }
        }
    }

    bool dequeue(T& container) noexcept override {
        while(1) {
            uint64_t headTicket = head_.fetch_add(1,std::memory_order_relaxed);
            uint64_t headIndex  = mod(headTicket);
            Cell& cell = array_[headIndex];
            unsigned int retry = 0;
            uint64_t tailTicket,tailIndex,tailClosed;
            while(1) {
                uint64_t packed_seq = cell.seq.load(std::memory_order_acquire);
                uint64_t unsafe = bit::get_msb(packed_seq);
                uint64_t seq = bit::clear_msb(packed_seq);
                T val = cell.val.load(std::memory_order_relaxed);
                if(packed_seq != cell.seq.load(std::memory_order_acquire))
                    continue;
                if(seq > (headTicket + size_))
                    break;
                if((val != nullptr) && !isReserved(val)) {
                    if(seq == (headTicket + size_)) {
                        cell.val.store(nullptr,std::memory_order_release);
                        container = val;
                        return true;
                    } else {
                        if(unsafe) {
                            if(cell.seq.load(std::memory_order_acquire) == packed_seq)
                                break;
                        } else {
                            if(cell.seq.compare_exchange_strong(packed_seq,bit::set_msb(seq)))
                                break;
                        }
                    }
                } else {
                    if((retry & MAX_RELOAD) == 0) {
                        tailTicket = tail_.load(std::memory_order_acquire);
                        tailIndex = bit::clear_msb(tailTicket);
                        tailClosed = tailTicket - tailIndex;
                    }
                    if(unsafe || tailIndex < (headTicket + 1) || tailClosed != 0 || retry > MAX_RETRY) {
                        if(isReserved(val) && !(cell.val.compare_exchange_strong(val,nullptr,std::memory_order_acq_rel)))
                            continue;
                        if(cell.seq.compare_exchange_strong(packed_seq,unsafe | (headTicket + size_)))
                            break;
                    }
                }
                ++retry;
            }
            if(bit::clear_msb(tail_.load(std::memory_order_acquire)) < (headTicket + 1)) {
                fixState();
                return false;
            }
        }
    }

    size_t capacity() const noexcept override { return size_; }

    size_t size() const noexcept override {
        uint64_t t = bit::clear_msb(tail_.load(std::memory_order_relaxed));
        uint64_t h = head_.load(std::memory_order_acquire);
        return t > h? (t - h) : 0;
    }

private:
    static constexpr unsigned int MAX_RELOAD    = (1ul << 8) - 1;
    static constexpr unsigned int MAX_RETRY     = 4 * 1024;

    T threadReserved() const noexcept {
        static thread_local T tid = [](){
            static std::atomic<uint64_t> counter{1ull};
            return reinterpret_cast<T>((counter.fetch_add(1ull) << 1) | 1);
        }();
        return tid;
    }

    bool isReserved(T ptr) const noexcept {
        return (reinterpret_cast<uintptr_t>(ptr) & 1) != 0;
    }

    void fixState() noexcept {
        uint64_t t = tail_.load(std::memory_order_relaxed);
        uint64_t h = head_.load(std::memory_order_relaxed);
        while(
            (h > t) &&
            !tail_.compare_exchange_strong(t,h,std::memory_order_acq_rel,std::memory_order_acquire)
        );
        return;
    }

protected:
    ALIGNED_CACHE std::atomic<uint64_t> head_;
    CACHE_PAD_TYPES(std::atomic_uint64_t);
    ALIGNED_CACHE std::atomic<uint64_t> tail_;
    CACHE_PAD_TYPES(std::atomic_uint64_t);
};

namespace segment {

template<typename T, typename Proxy, typename Opt = meta::EmptyOptions, typename NextT = void>
class LinkedPRQ:
    public PRQueue<
        T,Opt,LinkedPRQ<T,Proxy,Opt,NextT>
    >,
    public base::ILinkedSegment<
        T, std::conditional_t<
            std::is_void_v<NextT>,
            LinkedPRQ<T,Proxy,Opt,NextT>*,
            NextT
        >
    >
{
    using Base = PRQueue<T,Opt,LinkedPRQ<T,Proxy,Opt,NextT>>;
    using Next = std::conditional_t<std::is_void_v<NextT>,LinkedPRQ<T,Proxy,Opt,NextT>*,NextT>;
    using Cell = typename Base::Cell; // Import Cell type

    friend Base;
    friend Proxy;

    static_assert(!Opt::template has<PRQOption::DisableAutoClose>,"LinkedPRQ: AutoClose disabled");

    // =========================================================================
    // 5. INTERNAL MECHANICS FOR CO-ALLOCATION
    // =========================================================================
    struct CoAllocTag {}; // Private tag to protect unsafe constructor

    static Cell* compute_buffer_addr(void* self) {
        // Buffer sits exactly after the LinkedPRQ object
        return reinterpret_cast<Cell*>(reinterpret_cast<char*>(self) + sizeof(LinkedPRQ));
    }

public:
    static constexpr bool info_required = true;
    static constexpr bool optimized_alloc = true;

    // =========================================================================
    // 6. FACTORY METHOD (The "Smart" Allocator)
    // =========================================================================
    static LinkedPRQ* create(size_t s, uint64_t start = 0) {
        // Calculate total bytes: Object Header + Array Buffer
        // We reuse PRQueue logic for sizing (next_pow2 if needed) is tricky inside Base.
        // For simplicity assuming s is the desired size, alignment is key.
        size_t real_size = s;
        if constexpr (Base::POW2) {
             if(!bit::is_pow2(s)) real_size = bit::next_pow2(s);
        }

        size_t total_bytes = sizeof(LinkedPRQ) + (sizeof(Cell) * real_size);

        // Cache Line Alignment padding
        if(total_bytes % CACHE_LINE != 0)
            total_bytes += CACHE_LINE - (total_bytes % CACHE_LINE);

        // Allocate unified block
        void* mem = std::aligned_alloc(alignof(LinkedPRQ), total_bytes);
        if(!mem) throw std::bad_alloc();

        // Construct using the Tagged Constructor
        return ::new (mem) LinkedPRQ(CoAllocTag{}, real_size, start);
    }

    // =========================================================================
    // 7. MEMORY OPERATOR OVERRIDES
    // =========================================================================
    // Allows standard 'delete ptr;' to work for both Co-Allocated and Standard
    static void operator delete(void* ptr) {
        std::free(ptr);
    }

    static void* operator new(size_t size) {
        void* ptr = std::aligned_alloc(alignof(LinkedPRQ), size);
        if (!ptr) throw std::bad_alloc();
        return ptr;
    }

    // =========================================================================
    // 8. CONSTRUCTORS
    // =========================================================================

    // Standard Constructor (Buffer separate)
    LinkedPRQ(size_t size, uint64_t start = 0): Base(size,start) {}

    // Standard Constructor with item
    LinkedPRQ(T item, size_t size, uint64_t start = 0): Base(item,size,start) {}

    ~LinkedPRQ() override = default;

private:
    // Private Tagged Constructor (Co-Allocated Buffer)
    // Passes the calculated address of the trailing buffer to Base
    LinkedPRQ(CoAllocTag, size_t size, uint64_t start)
        : Base(size, start, compute_buffer_addr(this))
    {}

public:
    // ... [Rest of Segment Logic: getNext, close, open... UNCHANGED] ...

    Next getNext() const noexcept override  {
        return next_.load(std::memory_order_acquire);
    }

    static bool is_closed_(uint64_t val) noexcept {
        return bit::get_msb(val) != uint64_t{0};
    }

    bool close() noexcept final override {
        Base::tail_.fetch_or(bit::set_msb(uint64_t{0}),std::memory_order_acq_rel);
        return true;
    }

    bool open() noexcept final override {
        uint64_t tail = Base::tail_.load(std::memory_order_relaxed);
        if(bit::get_msb(tail) != 0) {
            uint64_t head = Base::head_.load(std::memory_order_relaxed);
            next_.store(nullptr,std::memory_order_relaxed);
            bool ok = Base::tail_.compare_exchange_strong(tail,head,std::memory_order_acq_rel);
            assert(ok && "LinkedPRQ: failed open - not exclusive ownership");
        }
        return true;
    }

    bool isClosed() const noexcept final override {
        return is_closed_(Base::tail_);
    }

    bool isOpened() const noexcept final override {
        return !isClosed();
    }

    bool enqueue(T item, [[maybe_unused]] bool info = false) noexcept final override {
        return info && isClosed()? false : Base::enqueue(item);
    }

    bool dequeue(T& item, [[maybe_unused]] bool info = true) noexcept final override {
        return Base::dequeue(item);
    }

    static_assert(detail::atomic_compatible_v<Next>,"LinkedPRQ Next field: not lock free");
    static_assert(std::is_default_constructible_v<Next>,"LinkedPRQ Next field: not default constructible");
    ALIGNED_CACHE std::atomic<Next> next_{};
    CACHE_PAD_TYPES(std::atomic<Next>);
};

}   //namespace segment
}   //namespace queue
