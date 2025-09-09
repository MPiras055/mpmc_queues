#pragma once
#include <IQueue.hpp>
#include <ILinkedSegment.hpp>
#include <cassert>
#include <SequencedCell.hpp>
#include <HeapStorage.hpp>
#include <bit.hpp>


// Forward declaration
template<typename T, typename Proxy, bool Pow2, bool auto_close>
class LinkedCASLoop;

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
 * @tparam Derived Type of the derived segment (CRTP) default void
 */
template<typename T, bool Pow2 = false, typename Derived = void> 
class CASLoopQueue: public base::IQueue<T> {
    using Effective = std::conditional_t<std::is_void_v<Derived>, CASLoopQueue, Derived>;
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
    CASLoopQueue(size_t size, uint64_t start = 0): 
        array_(Pow2? bit::next_power_of_two(size) : size),
        mask_(array_.capacity() - (Pow2? 1 : 0)) 
    {
        assert(size != 0 && "Null capacity");
        assert(array_.capacity() == size  && "HeapStorage doesn't match required size");
        for(uint64_t i = start; i < start + size; i++) {
            array_[i % size].val = T{};
            array_[i % size].seq.store(i, std::memory_order_relaxed);

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
    size_t index;

    do {
        tailTicket = bit::get63LSB(tail_.load(std::memory_order_relaxed));

        if constexpr (base::is_linked_segment_v<Effective>) {
            if (isClosed()) {
                return false;
            }
        }

        if constexpr (Pow2) {
            index = tailTicket & mask_;
        } else {
            index = tailTicket % mask_;
        }

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
            if constexpr (base::is_linked_segment_v<Effective>) {
                (void) close();
            }
            return false;
        }

        //CAS failed: retry
    } while (true);
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
        size_t index;
        do {
            headTicket = head_.load(std::memory_order_relaxed);
            if constexpr (Pow2) {
                index = headTicket & mask_;
            } else {
                index = headTicket % mask_;
            } 
            Cell& node = (array_[index]);
            seq  = node.seq.load(std::memory_order_acquire);

            int64_t diff = static_cast<int64_t>(seq) - static_cast<int64_t>(headTicket + 1);
            
            if(diff == 0) {
                if(head_.compare_exchange_weak(
                    headTicket, headTicket + 1,
                    std::memory_order_relaxed)) {
                    container = node.val.load(std::memory_order_acquire);
                    node.seq.store(headTicket + array_.capacity(), std::memory_order_release);
                    return true;
                }
            } else if(diff < 0){
                if(size() == 0) {
                    return false;
                }
            }
            
        } while(true);
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
    size_t size() override {
        return bit::get63LSB(tail_.load(std::memory_order_acquire)) - head_.load(std::memory_order_acquire);
    }

    /// @brief Defaulted destructor.
    ~CASLoopQueue() override = default;

protected:
    // Only accessible to friends (e.g. Proxy classes) or derived types (LinkedSegments).

    // ==========================
    // Lifecycle Control (Linked Queues)
    // ==========================

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
        tail_.store(head_.load(std::memory_order_acquire),std::memory_order_release);
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
        return bit::getMSB64(tail) != 0;
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
    const size_t mask_;
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
template<typename T, typename Proxy, bool Pow2 = false, bool auto_close = true>
class LinkedCASLoop: 
    //Base queue
    public CASLoopQueue<
        T,
        Pow2,
        std::conditional_t<
            auto_close,
            LinkedCASLoop<
                T,
                Proxy
            >,
        void>
    >,

    //Base linked interface
    public base::ILinkedSegment<
        T,
        LinkedCASLoop<
            T,
            Proxy,
            Pow2
        >
    > 
{
    friend Proxy;   ///< Proxy class can access private methods.

    using Base = CASLoopQueue<T,Pow2,std::conditional_t<auto_close,LinkedCASLoop<T,Proxy>,void>>;

public:
    /**
     * @brief Constructs a linked CAS loop queue segment.
     * 
     * @param size Capacity of this segment.
     * @param start Initial sequence number (defaults to 0).
     */
    LinkedCASLoop(size_t size, uint64_t start = 0): 
        Base(size,start) {
            assert(base::is_linked_segment_v<std::decay_t<decltype(*this)>> && "LinkedSegment is not linked");
        }

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

    bool open() final override {
        (void)Base::open();     //align the indexes
        return true;
    }

    uint64_t getNextStartIndex() const override {
        uint64_t tail = Base::tail_.load(std::memory_order_relaxed);
        return bit::get63LSB(tail);
    }

    std::atomic<LinkedCASLoop*> next_{nullptr}; ///< Pointer to the next segment in the chain.
};
