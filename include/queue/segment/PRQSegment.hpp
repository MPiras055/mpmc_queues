#pragma once
#include <IQueue.hpp>
#include <ILinkedSegment.hpp>
#include <SequencedCell.hpp>
#include <HeapStorage.hpp>
#include <StaticThreadTicket.hpp>
#include <cassert>
#include <specs.hpp>
#include <bit.hpp>



// Forward declaration
template<typename T, typename Proxy, bool auto_close>
class LinkedPRQ;

/**
 * @brief Lock-free queue implementation using fetch-add loop
 * 
 * @tparam T Type of elements stored in the queue
 */
template<typename T, typename Derived = void>
class PRQueue: public base::IQueue<T> {

    using Effective = std::conditional_t<std::is_void_v<Derived>,PRQueue,Derived>;
    using Cell = SequencedCell<T>;

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
    explicit PRQueue(size_t size, uint64_t start = 0): array_(size) {
        assert(size != 0 && "Null capacity");
        for(uint64_t i = start; i < start + size; i++) {
            array_[i % size].val = T{nullptr};
            array_[i % size].seq.store(i, std::memory_order_relaxed);
        }

        head_.store(start, std::memory_order_relaxed);
        tail_.store(start, std::memory_order_relaxed);
    }

        /**
     * @brief Enqueues an item into the queue.
     * 
     * @param item Value to insert into the queue.
     * @return true If the enqueue succeeds.
     * @return false If the queue is full or closed.
     */
    bool enqueue(T item) final override {
        T tagged = threadReserved();

        while(1) {
            uint64_t tailTicket = tail_.fetch_add(1);
            if constexpr(base::is_linked_segment_v<Effective>) {
                if(is_closed_(tailTicket))
                    return false;
            }

            Cell& cell = array_[tailTicket % array_.capacity()];
            uint64_t seq = cell.seq.load();
            T val = cell.val.load();
            
            if( (val == nullptr) &&
                (bit::get63LSB(seq) <= tailTicket) &&
                (!bit::getMSB64(seq) || head_.load() <= tailTicket)
            ) {
                if(cell.val.compare_exchange_strong(val,tagged)) {
                    if (cell.seq.compare_exchange_strong(seq,tailTicket + array_.capacity()) &&
                        cell.val.compare_exchange_strong(tagged,item)
                    ) return true;
                } else cell.val.compare_exchange_strong(tagged,T{nullptr});
            }

            if(tailTicket >= (head_.load() + array_.capacity())) {
                if constexpr (base::is_linked_segment_v<Effective>) {
                    (void) close();
                }
                return false;
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
    bool dequeue(T& container) override {
        while(1) {
            uint64_t headTicket = head_.fetch_add(1,std::memory_order_acq_rel);
            Cell& cell = array_[headTicket % array_.capacity()];

            unsigned int retry = 0;
            uint64_t tailTicket,tailIndex,tailClosed;

            while(1) {
                uint64_t packed_seq = cell.seq.load();
                uint64_t unsafe = bit::getMSB64(packed_seq);
                uint64_t seq = bit::get63LSB(packed_seq);
                T val = cell.val.load();

                //inconsistent view of the cell
                if(packed_seq != cell.seq.load())
                    continue;

                if(seq > (headTicket + array_.capacity()))
                    break;
                    
                if((val != nullptr) && !isReserved(val)) {
                    if(seq == (headTicket + array_.capacity())) {
                        cell.val.store(nullptr);
                        container = val;
                        return true;
                    } else {
                        if(unsafe) {
                            if(cell.seq.load() == packed_seq)
                                break;
                        } else {
                            if(cell.seq.compare_exchange_strong(packed_seq,bit::setMSB64(seq)))
                                break;
                        }
                    }
                } else {
                    if((retry & MAX_RELOAD) == 0) {
                        tailTicket = tail_.load();
                        tailIndex = bit::get63LSB(tailTicket);
                        tailClosed = tailTicket - tailIndex;
                    }
                    if(unsafe || tailIndex < (headTicket + 1) || tailClosed || retry > MAX_RETRY) {
                        if(isReserved(val) && !(cell.val.compare_exchange_strong(val,nullptr)))
                            continue;
                        if(cell.seq.compare_exchange_strong(packed_seq,unsafe | (headTicket + array_.capacity())))
                            break;
                    }
                }
                ++retry;
            }

            if(bit::get63LSB(tail_.load(std::memory_order_acquire)) <= (headTicket + 1)) {
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
    size_t capacity() const override {
        return array_.capacity();
    }

    /**
     * @brief Returns the number of items in the queue.
     * 
     * This value may not be exact in multithreaded use, but is safe to use 
     * for metrics or capacity checks.
     * 
     * @return Current number of elements in the queue.
     * 
     * @warning this value may be approximated
     */
    size_t size() override {
        uint64_t t = bit::get63LSB(tail_.load(std::memory_order_relaxed));
        uint64_t h = head_.load(std::memory_order_acquire);
        return t > h? t - h : 0;
    }

    /// @brief Defaulted destructor.
    ~PRQueue() override = default;

protected:
    // Only accessible to friends (e.g. Proxy classes) or derived types (LinkedSegments).

    // ==================================
    // Lifecycle Control (Linked Queues)
    // ==================================

    /**
     * @brief Marks the queue as closed.
     * 
     * Once closed, further enqueue attempts will fail.
     * 
     * @return Always true.
     */
    bool close() override {
        tail_.fetch_or(bit::MSB64, std::memory_order_release);
        return true;
    }

    /**
     * @brief Reopens a previously closed queue.
     * 
     * Allows enqueue operations again.
     * 
     * @return Always true.
     */
    bool open() override {
        tail_.fetch_and(bit::LSB63_MASK, std::memory_order_relaxed);
        return true;
    }

    /**
     * @brief Checks if the queue is closed.
     * 
     * @return true If the queue is closed.
     * @return false If the queue is open.
     */
    bool isClosed() const override {
        uint64_t tail = tail_.load(std::memory_order_relaxed);
        return is_closed_(tail);
    }

    /**
     * @brief Checks if the queue is open.
     * 
     * @return true If the queue is open.
     * @return false If the queue is closed.
     */
    bool isOpened() const override {
        return !isClosed();
    }

    alignas(CACHE_LINE) std::atomic<uint64_t> head_; ///< Head ticket index for dequeue.
    char pad_head_[CACHE_LINE - sizeof(std::atomic<uint64_t>)];
    alignas(CACHE_LINE)std::atomic<uint64_t> tail_; ///< Tail ticket index for enqueue.
    char pad_tail_[CACHE_LINE - sizeof(std::atomic<uint64_t>)];
    util::memory::HeapStorage<Cell> array_; ///< Underlying circular buffer storage.

private:

    // ==================================
    // Caching values optimization
    // ==================================

    static constexpr unsigned int MAX_RELOAD    = (1ul << 8) - 1; //has to be 2^n - 1
    static constexpr unsigned int MAX_RETRY     = 4*1024; 

    /**
     * @brief get a per-thread reserved dirty pointer of type T
     * 
     * @note uses thread-local storage to cache the result
     * 
     */
    T threadReserved() const {
        thread_local static T tid = [] {
            static std::atomic<int> counter{1};
            return reinterpret_cast<T>((counter.fetch_add(1) << 1) | 1);
        }();
        return tid;
    }

    bool is_closed_(uint64_t val) const {
        return bit::getMSB64(val);
    }


    /**
     * @brief checks if a pointer is per-thread reserved
     * 
     * @note a per-thread reserved pointer has the LSB setted and non
     * null 63-MSB
     */
    bool isReserved(T ptr) const {
        return (reinterpret_cast<uintptr_t>(ptr) & 1) != 0;
    }

    void fixState() {
        while(1) {
            uint64_t t = tail_.load();
            uint64_t h = head_.load();
            
            if(t != tail_.load()) // inconsistent tail
                continue;

            if(h > t) { 
                if(!tail_.compare_exchange_strong(t,h))
                    continue;
            }

            return;  
        }
    }
};

/**
 * @brief Linked segment extension of PRQueue.
 * 
 * This class enables chaining multiple fixed-size PRQueue into 
 * a larger, virtually unbounded lock-free queue.
 * 
 * @tparam T Type of elements stored in the queue.
 * @tparam Proxy Friend class allowed to access private members (e.g., higher-level queue).
 */
template<typename T, typename Proxy, bool auto_close = true>
class LinkedPRQ: 
    public PRQueue<T,std::conditional_t<auto_close,LinkedPRQ<T,Proxy>,void>>,
    public base::ILinkedSegment<T,LinkedPRQ<T,Proxy>> {
    friend Proxy;   ///< Proxy class can access private methods.

    using Base = PRQueue<T,std::conditional_t<auto_close,LinkedPRQ<T,Proxy>,void>>;
    static constexpr bool AUTO_CLOSE = auto_close;

public:
    /**
     * @brief Constructs a linked CAS loop queue segment.
     * 
     * @param size Capacity of this segment.
     * @param start Initial sequence number (defaults to 0).
     */
    LinkedPRQ(size_t size, uint64_t start = 0): 
        PRQueue<T,LinkedPRQ<T,Proxy>>(size,start) {}

    /// @brief Defaulted destructor.
    ~LinkedPRQ() override = default;

private:

    /**
     * @brief Returns the next linked segment in the chain.
     * 
     * @return Pointer to the next segment, or nullptr if none.
     */
    LinkedPRQ* getNext() const override {
        return next_.load(std::memory_order_acquire);
    }


    uint64_t getNextStartIndex() const override {
        uint64_t tail = Base::tail_.load(std::memory_order_relaxed);
        return bit::get63LSB(tail) - 1;
    }

    bool open() final override {
        if(bit::getMSB64(Base::tail_.fetch_and(bit::LSB63_MASK)) != 0) {
            next_.store(nullptr,std::memory_order_relaxed);
        }
        return true;
    }

    size_t size() final override {
        return bit::get63LSB(Base::size());
    }

    std::atomic<LinkedPRQ*> next_{nullptr}; ///< Pointer to the next segment in the chain.
};
