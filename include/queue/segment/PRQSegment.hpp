#pragma once
#include <atomic>
#include <cassert>
#include <specs.hpp>            //  padding and compatibility def
#include <IQueue.hpp>           //  base queue interface
#include <ILinkedSegment.hpp>   //  base linked segment interface
#include <SequencedCell.hpp>    //  cell definition
#include <bit.hpp>              //  bit manipulation utilities
#include <OptionsPack.hpp>      //  options



namespace queue {

/// Options for the queue
struct PRQOption {
    struct Pow2Size{};
    struct DisableCellPadding{};
    struct DisableAutoClose{};
};

// Forward declaration
// template<typename T, typename Proxy, typename Opt, typename NextT>
// class LinkedPRQ;

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

    static constexpr bool POW2 =        Opt::template has<PRQOption::Pow2Size>;
    static constexpr bool PAD_CELL =    !Opt::template has<PRQOption::DisableCellPadding>;

    using Cell = cell::SequencedCell<T,PAD_CELL>;

    inline size_t mod(uint64_t i) const noexcept {
        if constexpr (POW2) {
            return i & mask_;
        } else {
            return i % size_;
        }
    }

public:
    /**
     * @brief Constructs a queue with the given capacity.
     *
     * Initializes all cells in the buffer with default-constructed values
     * and assigns sequence numbers for correct enqueue/dequeue ordering.
     *
     * @param size Capacity of the queue.
     * @param start Initial sequence number (defaults to 0).
     */
    explicit PRQueue(size_t size, uint64_t start = 0):
        size_(POW2 && !bit::is_pow2(size)? bit::next_pow2(size) : size),
        mask_(POW2 && size_ != 1? (size_ - 1) : 0),
        array_{new Cell[size_]}
    {
        assert(size_ != 0 && "PRQueue: null capacity");
        assert(!POW2 || mask_ != 0 && "PRQueue: null bitmask");
        assert(bit::get_msb(start + size_) == 0ull && "PRQueue: sequence overflow");

        for(uint64_t i = start; i < start + size_; i++) {
            array_[mod(i)].seq.store(i, std::memory_order_relaxed);
        }
        head_.store(start, std::memory_order_relaxed);
        tail_.store(start, std::memory_order_relaxed);
    }

    /**
     * @brief constructs a queue with the given capacity
     * with an item already installed
     *
     * @tparam item:    item to be installed
     * @param size:     capacity of the queue
     * @param start:    start offset
     */
     explicit PRQueue(T item, size_t size, uint64_t start = 0):
        size_(POW2 && !bit::is_pow2(size)? bit::next_pow2(size) : size),
        mask_(POW2 && size_ != 1? (size_ - 1) : 0),
        array_{new Cell[size_]} {
        assert(item != nullptr && "Cannot insert nullptr");
        assert(size_ != 0 && "PRQueue: null capacity");
        assert(!POW2 || mask_ != 0 && "PRQueue: null bitmask");
        assert(bit::get_msb(start + size_) == 0ull && "PRQueue: sequence overflow");

        for(uint64_t i = start; i < start + size_; i++) {
             array_[mod(i)].val.store(nullptr, std::memory_order_relaxed);
             array_[mod(i)].seq.store(i, std::memory_order_relaxed);
        }

        array_[mod(start)].val.store(item,std::memory_order_relaxed);
        array_[mod(start)].seq.store(start + size_,std::memory_order_relaxed);
        head_.store(start, std::memory_order_relaxed);
        tail_.store(start + 1, std::memory_order_relaxed);
     }

    /**
     * @brief Enqueues an item into the queue.
     *
     * @param item Value to insert into the queue.
     * @return true If the enqueue succeeds.
     * @return false If the queue is full or closed.
     */
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

            //queue is most likely full or livelocked
            if(tailTicket >= (head_.load(std::memory_order_acquire) + size_)) {
                if constexpr (AUTO_CLOSE) {
                    if(static_cast<Effective*>(this)->close())
                        return false;
                }
            }
        }
    }

    /**
     * @brief Dequeues an item from the queue.
     *
     * Uses CAS on the head index to reserve a slot, then reads and clears
     * the value. If the queue is empty, the dequeue will fail.
     *
     * @param[out] container Reference where the dequeued item is stored.
     * @return true If the dequeue succeeds.
     * @return false If the queue is empty.
     */
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

                //inconsistent view of the cell
                if(packed_seq != cell.seq.load(std::memory_order_acquire))
                    continue;

                //arrived early
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
                    //reload the tail pointer every MAX_RELOAD iterations
                    if((retry & MAX_RELOAD) == 0) {
                        tailTicket = tail_.load(std::memory_order_acquire);
                        tailIndex = bit::clear_msb(tailTicket);
                        tailClosed = tailTicket - tailIndex;
                    }
                    /**
                     * Conditions
                     * - cell is unsage (dequeues are not dequeueing | could contain reserved pointer)
                     * - tail is behid head (high contention)
                     * - queue is closed
                     * - too many retries on the same cell
                     */
                    if(unsafe || tailIndex < (headTicket + 1) || tailClosed != 0 || retry > MAX_RETRY) {
                        //check if the cell contains a stale bottom pointer
                        if(isReserved(val) && !(cell.val.compare_exchange_strong(val,nullptr,std::memory_order_acq_rel)))
                            continue;

                        //advance the cells epoch
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

    /**
     * @brief Returns the capacity of the queue.
     *
     * @return Maximum number of elements that can be stored.
     */
    size_t capacity() const noexcept override {
        return size_;
    }

    /**
     * @brief Returns the number of items in the queue.
     *
     * @note This value is not exact, since it performs the difference between
     * the tail and head counters that get advanced optimistically
     *
     * @return Current number of elements in the queue.
     *
     */
    size_t size() const noexcept override {
        uint64_t t = bit::clear_msb(tail_.load(std::memory_order_relaxed));
        uint64_t h = head_.load(std::memory_order_acquire);
        return t > h? (t - h) : 0;
    }

    /// @brief Defaulted destructor.
    ~PRQueue() override {
        delete[] array_;
    };

private:

    // ==================================
    // Caching values optimization
    // ==================================

    static constexpr unsigned int MAX_RELOAD    = (1ul << 8) - 1; //has to be 2^n - 1
    static constexpr unsigned int MAX_RETRY     = 4 * 1024;

    /**
     * @brief get a per-thread reserved dirty pointer of type T
     *
     * @note uses thread-local storage to cache the result
     *
     */
    T threadReserved() const noexcept {
        static thread_local T tid = [](){
            static std::atomic<uint64_t> counter{1ull};
            return reinterpret_cast<T>((counter.fetch_add(1ull) << 1) | 1);
        }();
        return tid;
    }

    /**
     * @brief checks if a pointer is per-thread reserved
     *
     * @note a per-thread reserved pointer has the LSB setted and non
     * null 63-MSB
     */
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

protected:  //Accessible to LinkedPRQ

    ALIGNED_CACHE std::atomic<uint64_t> head_; ///< Head ticket index for dequeue.
    CACHE_PAD_TYPES(std::atomic_uint64_t);
    ALIGNED_CACHE std::atomic<uint64_t> tail_; ///< Tail ticket index for enqueue.
    CACHE_PAD_TYPES(std::atomic_uint64_t);
    const size_t size_;
    const size_t mask_;
    Cell* array_; ///< Underlying circular buffer storage.
};



namespace segment {
/**
 * @brief Linked segment extension of PRQueue.
 *
 * This class enables chaining multiple fixed-size PRQueue into
 * a larger, virtually unbounded lock-free queue.
 *
 * @tparam T Type of elements stored in the queue.
 * @tparam Proxy Friend class allowed to access private members (e.g., higher-level queue).
 */
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
    friend Base;
    friend Proxy;   ///< Proxy class can access private methods.

    //proxies require segments to be on auto_close mode;
    static_assert(!Opt::template has<PRQOption::DisableAutoClose>,"LinkedPRQ: AutoClose disabled");

public:
    /**
     * @brief Constructs a linked PRQ segment.
     *
     * @param size Capacity of this segment.
     * @param start Initial sequence number (defaults to 0).
     */
    LinkedPRQ(size_t size, uint64_t start = 0): Base(size,start) {}
    LinkedPRQ(T item, size_t size, uint64_t start = 0): Base(item,size,start) {}

    /// @brief Defaulted destructor.
    ~LinkedPRQ() override = default;

private:

    /**
     * @brief Returns the next linked segment in the chain.
     *
     * @return Pointer to the next segment, or nullptr if none.
     */
    Next getNext() const noexcept override  {
        return next_.load(std::memory_order_acquire);
    }

    /**
     * @brief internal method to check if the queue is closed
     */
    static bool is_closed_(uint64_t val) noexcept {
        return bit::get_msb(val) != uint64_t{0};
    }

    /**
     * @brief closes the queue to further insertions (until open() is called)
     */
    bool close() noexcept final override {
        Base::tail_.fetch_or(bit::set_msb(uint64_t{0}),std::memory_order_acq_rel);
        return true;
    }

    /**
     * @brief reopens a previously closed segment
     *
     * Checks if the closure bit is setted, if so it alignes the head and tail index
     * via CAS and clears the next pointer.
     *
     * @warning it is supposed that this method gets called on a fully drained queue,
     * undefined behaviour can occur if it's not the case.
     */
    bool open() noexcept final override {
        uint64_t tail = Base::tail_.load(std::memory_order_relaxed);
        if(bit::get_msb(tail) != 0) {
            uint64_t head = Base::head_.load(std::memory_order_relaxed);
            next_.store(nullptr,std::memory_order_relaxed); //this is guarded by the CAS
            bool ok = Base::tail_.compare_exchange_strong(tail,head,std::memory_order_acq_rel);
            assert(ok && "LinkedPRQ: failed open - not exclusive ownership");
        }
        return true;
    }

    /**
     * @brief checks if the segment is closed to further insertions
     */
    bool isClosed() const noexcept final override {
        return is_closed_(Base::tail_);
    }

    /**
     * @brief checks if the segment is open to further insertions
     */
    bool isOpened() const noexcept final override {
        return !isClosed();
    }

    /// @brief enqueue with additional info
    /// @param T item: item to be enqueued
    /// @param bool info: if true check if the segment is closed before attempting the operation
    ///
    /// @note: this is necessary to prevent thrashing indexes that can lead to livelock of the segment
    bool enqueue(T item, [[maybe_unused]] bool info = false) noexcept final override {
        return info && isClosed()? false : Base::enqueue(item);
    }

    /// @brief dequeue with additional info
    bool dequeue(T& item, [[maybe_unused]] bool info = true) noexcept final override {
        return Base::dequeue(item);
    }


    static_assert(detail::atomic_compatible_v<Next>,"LinkedPRQ Next field: not lock free");
    static_assert(std::is_default_constructible_v<Next>,"LinkedPRQ Next field: not default constructible");
    ALIGNED_CACHE std::atomic<Next> next_{}; ///< Pointer to the next segment in the chain.
    CACHE_PAD_TYPES(std::atomic<Next>);
};

}   //namespace segment

}   //namespace queue
