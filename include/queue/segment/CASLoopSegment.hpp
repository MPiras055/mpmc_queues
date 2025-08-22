#include <ILinkedSegment.hpp>
#include <SequencedCell.hpp>
#include <HeapStorage.hpp>
#include <atomic>

/**
 * @brief Lock-free queue implementation using a compare-and-swap loop.
 * 
 * This queue is based on a circular buffer of cells, where each cell 
 * maintains a sequence number to coordinate between producers (enqueue) 
 * and consumers (dequeue). It uses atomics and sequence numbers to ensure 
 * wait-free progress for single producer/consumer pairs, and lock-free 
 * progress for multiple threads.
 * 
 * @tparam T Type of elements stored in the queue.
 */
template<typename T> 
class CASLoopQueue: public meta::IQueue<T> {
    using Cell = SequencedCell<T>; ///< Internal buffer cell (value + sequence counter).

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
    CASLoopQueue(size_t size, uint64_t start = 0): array_(size) {
        for(uint64_t i = 0; i < array_.capacity(); i++) {
            array_[i].val = T{};
            array_[i].seq.store(i + start, std::memory_order_relaxed);
        }

        head_.store(start, std::memory_order_relaxed);
        tail_.store(start, std::memory_order_relaxed);
    }

    /**
     * @brief Enqueues an item into the queue.
     * 
     * Uses CAS on the tail index to reserve a slot, then writes the item 
     * into the reserved cell. If the queue is full, the enqueue will fail.
     * 
     * @param item Value to insert into the queue.
     * @return true If the enqueue succeeds.
     * @return false If the queue is full or closed.
     */
    bool enqueue(T item) final override {
        uint64_t tailTicket, seq;
        Cell* node;

        do {
            tailTicket = tail_.load(std::memory_order_relaxed);

            if constexpr (IS_LINKED) {
                if(isClosed()) {
                    return false;
                }
            }

            node = &(array_[tailTicket % array_.capacity()]);
            seq  = node->seq.load(std::memory_order_acquire);

            if(tailTicket == seq) {
                if(tail_.compare_exchange_weak(
                    tailTicket, tailTicket + 1,
                    std::memory_order_relaxed)) {
                    break;
                }
            } else if (tailTicket > seq) {
                if constexpr (IS_LINKED) {
                    (void)close();
                } 
                return false;
            }
        } while (true);

        node->val = item;
        node->seq.store(seq + 1, std::memory_order_release);
        return true;
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
        uint64_t headTicket, seq;
        Cell* node;
        do {
            headTicket = head_.load(std::memory_order_relaxed);
            node = &(array_[headTicket % array_.capacity()]);
            seq  = node->seq.load(std::memory_order_acquire);

            int64_t diff = seq - (headTicket + 1);
            
            if(diff == 0) {
                if(head_.compare_exchange_weak(
                    headTicket, headTicket + 1,
                    std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false;
            }
        } while(true);

        container = node->val;
        node->seq.store(headTicket + array_.capacity(), std::memory_order_release);
        return true;
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
     * @brief Returns the approximate number of items in the queue.
     * 
     * This value may not be exact in multithreaded use, but is safe to use 
     * for metrics or capacity checks.
     * 
     * @return Current number of elements in the queue.
     */
    size_t size() const override {
        return tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_acquire);
    }

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

    /// @brief Defaulted destructor.
    ~CASLoopQueue() override = default;

private:
    util::memory::HeapStorage<Cell> array_; ///< Underlying circular buffer storage.

protected:
    std::atomic<uint64_t> head_; ///< Head ticket index for dequeue.
    std::atomic<uint64_t> tail_; ///< Tail ticket index for enqueue.
};


/**
 * @brief Linked segment extension of CASLoopQueue.
 * 
 * This class enables chaining multiple fixed-size CASLoopQueues into 
 * a larger, virtually unbounded lock-free queue.
 * 
 * @tparam T Type of elements stored in the queue.
 * @tparam Proxy Friend class allowed to access private members (e.g., higher-level queue).
 */
template<typename T, typename Proxy>
class LinkedCASLoop: 
    public meta::ILinkedSegment<T>,
    public CASLoopQueue<T> {
    friend Proxy;   ///< Proxy class can access private methods.

public:
    /**
     * @brief Constructs a linked CAS loop queue segment.
     * 
     * @param size Capacity of this segment.
     * @param start Initial sequence number (defaults to 0).
     */
    LinkedCASLoop(size_t size, uint64_t start = 0): 
        CASLoopQueue<T>(size,start) {}

    /// @brief Defaulted destructor.
    ~LinkedCASLoop() override = default;

private:
    /**
     * @brief Returns the next linked segment in the chain.
     * 
     * @return Pointer to the next segment, or nullptr if none.
     */
    LinkedCASLoop* getNext() const override {
        return next_.load(std::memory_order_acquire);
    }

    std::atomic<LinkedCASLoop*> next_{nullptr}; ///< Pointer to the next segment in the chain.
};
