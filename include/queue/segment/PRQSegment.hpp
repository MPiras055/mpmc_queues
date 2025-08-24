#include <IQueue.hpp>
#include <ILinkedSegment.hpp>
#include <SequencedCell.hpp>
#include <HeapStorage.hpp>
#include <functional>
#include <thread>

// Forward declaration
template<typename T, typename Proxy, bool auto_close>
class LinkedPRQ;

/**
 * @brief Lock-free queue implementation using fetch-add loop
 * 
 * @tparam T Type of elements stored in the queue
 */
template<typename T, typename Derived = void>
class PRQueue: public meta::IQueue<T> {
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
        for(uint64_t i = 0; i < array_.capacity(); i++) {
            array_[i].val = nullptr;
            array_[i].seq.store(i + start, std::memory_order_relaxed);
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
        thread_local T tagged = [] {
            std::size_t hash = std::hash<std::thread::id>{}(std::this_thread::get_id());
            uintptr_t encoded = static_cast<uintptr_t>(hash) << 1 | 1;
            return reinterpret_cast<T>(encoded);
        }();

        while(1) {
            if constexpr(meta::is_linked_segment_v<Effective>) {
                if(isClosed())
                    return false;
            }

            uint64_t tailTicket = tail_.fetch_add(1,std::memory_order_acq_rel);
            Cell& cell = array_[tailTicket % array_.capacity()];
            uint64_t seq = cell.seq.load(std::memory_order_acquire);
            T val = cell.val.load(std::memory_order_acquire);
            
            if( val == nullptr &&
                get63LSB(seq) <= tailTicket &&
                (!getMSB(seq) || head_.load(std::memory_order_relaxed) <= tailTicket)
            ) {
                if(cell.val.compare_exchange_strong(val,tagged,std::memory_order_acq_rel)) {
                    if (cell.seq.compare_exchange_strong(seq,tailTicket + array_.capacity(),std::memory_order_acq_rel) &&
                        cell.val.compare_exchange_strong(tagged,item,std::memory_order_acq_rel)
                    ) return true;
                } else cell.val.compare_exchange_strong(tagged,nullptr,std::memory_order_relaxed);
            }

            if(tailTicket >= (head_.load(std::memory_order_acquire) + capacity())) {
                if constexpr (meta::is_linked_segment_v<Effective>) {
                    (void)close();
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
                uint64_t packed_seq = cell.seq.load(std::memory_order_acquire);
                uint64_t unsafe = getMSB(packed_seq);
                uint64_t seq = get63LSB(packed_seq);
                T val = cell.val.load(std::memory_order_acquire);

                if(packed_seq != cell.seq.load(std::memory_order_acquire))
                    continue;
                if(seq > (headTicket + array_.capacity()))
                    break;
                if((val != nullptr) && !isReserved(val)) {
                    if(seq == headTicket + array_.capacity()) {
                        cell.val.store(nullptr,std::memory_order_relaxed);
                        container = val;
                        return true;
                    } else {
                        if(unsafe) {
                            if(cell.seq.load(std::memory_order_acquire) == packed_seq)
                                break;
                        } else {
                            if(cell.seq.compare_exchange_strong(packed_seq,setMSB(seq),std::memory_order_acq_rel))
                                break;
                        }
                    }
                } else {
                    if((retry & MAX_RELOAD) == 0) {
                        tailTicket = tail_.load(std::memory_order_relaxed);
                        tailIndex = get63LSB(tailTicket);
                        tailClosed = tailTicket - tailIndex;
                    }
                    if(unsafe || tailTicket < headTicket + 1 || tailClosed || retry > MAX_RETRY) {
                        if(isReserved(val) && !(cell.val.compare_exchange_strong(val,nullptr,std::memory_order_acq_rel)))
                            continue;
                        if(cell.seq.compare_exchange_strong(packed_seq,unsafe | headTicket + array_.capacity(),std::memory_order_acq_rel))
                            break;
                    }
                }
                ++retry;
            }

            if(get63LSB(tail_.load(std::memory_order_acquire)) <= headTicket + 1) {
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
    size_t size() const override {
        return tail_.load(std::memory_order_relaxed) - head_.load(std::memory_order_acquire);
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
        tail_.fetch_or(1ull<<63, std::memory_order_release);
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
        tail_.fetch_and(~(1ull<<63), std::memory_order_relaxed);
        return true;
    }

    /**
     * @brief Checks if the queue is closed.
     * 
     * @return true If the queue is closed.
     * @return false If the queue is open.
     */
    bool isClosed() const override {
        return (tail_.load(std::memory_order_relaxed) & (1ull<<63)) != 0;
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

    std::atomic<uint64_t> head_; ///< Head ticket index for dequeue.
    std::atomic<uint64_t> tail_; ///< Tail ticket index for enqueue.
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
     * @deprecated now hardcoded in `enqueue` method
     */
    void* threadReserved() const {
        thread_local void* reserved = [] {
            std::size_t hash = std::hash<std::thread::id>{}(std::this_thread::get_id());
            uintptr_t encoded = static_cast<uintptr_t>(hash) << 1 | 1;
            return reinterpret_cast<T>(encoded);
        }();
        return reserved;
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
            uint64_t t = tail_.load(std::memory_order_relaxed);
            uint64_t h = head_.load(std::memory_order_relaxed);
            
            if(t != tail_.load(std::memory_order_acquire))
                continue;
            if(h > t && !tail_.compare_exchange_strong(t,h,std::memory_order_acq_rel))
                continue;
            return;  
        }
    }

    static uint64_t get63LSB(uint64_t v) {   return v & ~(1ull << 63);}
    static uint64_t getMSB(uint64_t v) {     return v & (1ull << 63);}
    static uint64_t setMSB(uint64_t v) {     return v | (1ull << 63);}
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
    public meta::ILinkedSegment<T,LinkedPRQ<T,Proxy>> {
    friend Proxy;   ///< Proxy class can access private methods.

    using Base = PRQueue<T,std::conditional_t<auto_close,LinkedPRQ<T,Proxy>,void>>;

public:
    /// @brief Defaulted destructor.
    ~LinkedPRQ() override = default;

private:
    /**
     * @brief Constructs a linked CAS loop queue segment.
     * 
     * @param size Capacity of this segment.
     * @param start Initial sequence number (defaults to 0).
     */
    LinkedPRQ(size_t size, uint64_t start = 0): 
        PRQueue<T,LinkedPRQ<T,Proxy>>(size,start) {}

    /**
     * @brief Returns the next linked segment in the chain.
     * 
     * @return Pointer to the next segment, or nullptr if none.
     */
    LinkedPRQ* getNext() const override {
        return next_.load(std::memory_order_acquire);
    }

    bool open() final override {
        if((Base::tail_.fetch_and(~(1ull << 63),std::memory_order_acquire) & 1ull << 63 != 0)) {
            next_.store(nullptr,std::memory_order_relaxed);
        }
        return true;
    }

    size_t size() const final override {
        return Base::size() & ~(1ull<<63);
    }

    std::atomic<LinkedPRQ*> next_{nullptr}; ///< Pointer to the next segment in the chain.
};
