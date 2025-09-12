#pragma once
#include <IQueue.hpp>
#include <ILinkedSegment.hpp>
#include <SequencedCell.hpp>
#include <HeapStorage.hpp>
#include <StaticThreadTicket.hpp>
#include <cassert>
#include <specs.hpp>
#include <bit.hpp>
#include <iostream>



// Forward declaration
template<typename T, typename Proxy, bool Pow2, bool auto_close>
class LinkedPRQ;

/**
 * @brief Lock-free queue implementation using fetch-add loop
 *
 * @tparam T Type of elements stored in the queue
 */
template<typename T, bool Pow2 = false, typename Derived = void>
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
    explicit PRQueue(size_t size, uint64_t start = 0):
        size_{Pow2? bit::next_pow2(size) : size},
        mask_{size_ - 1},
        array_(size_)
    {
        assert(size_ != 0 && "Null capacity");
        assert(!(Pow2 && size_ == 1) &&  "Size 1 and Pow2 optimization enabled");
        assert(size == array_.capacity());
        for(uint64_t i = start; i < start + size_; i++) {
            array_[i % size_].val = T{nullptr};
            array_[i % size_].seq.store(i, std::memory_order_relaxed);
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
                if(is_closed_(tailTicket)){
                    return false;
                }
            }

            size_t tailIndex;
            if constexpr (Pow2) {
                tailIndex = tailTicket & mask_;
            } else {
                tailIndex = tailTicket % size_;
            }

            Cell& cell = array_[tailIndex];
            uint64_t seq = cell.seq.load();
            T val = cell.val.load();

            if( (val == nullptr) &&
                (bit::get63LSB(seq) <= tailTicket) &&
                (!bit::getMSB64(seq) || head_.load() <= tailTicket)
            ) {
                if(cell.val.compare_exchange_strong(val,tagged)) {
                    if (cell.seq.compare_exchange_strong(seq,tailTicket + size_) &&
                        cell.val.compare_exchange_strong(tagged,item)
                    ) return true;
                } else cell.val.compare_exchange_strong(tagged,T{nullptr});
            }

            if(tailTicket >= (head_.load() + size_)) {
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
            uint64_t headIndex;
            if constexpr(Pow2) {
                headIndex = headTicket & mask_;
            } else {
                headIndex = headTicket % size_;
            }
            Cell& cell = array_[headIndex];

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

                if(seq > (headTicket + size_))
                    break;

                if((val != nullptr) && !isReserved(val)) {
                    if(seq == (headTicket + size_)) {
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
                        if(cell.seq.compare_exchange_strong(packed_seq,unsafe | (headTicket + size_)))
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
        tail_.fetch_or(bit::MSB64);
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
        tail_.fetch_and(bit::LSB63_MASK,std::memory_order_release);
        return true;
    }

    /**
     * @brief Checks if the queue is closed.
     *
     * @return true If the queue is closed.
     * @return false If the queue is open.
     */
    bool isClosed() const override {
        uint64_t tail = tail_.load(std::memory_order_acquire);
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

    /**
     * @brief checks if a pointer is per-thread reserved
     *
     * @note a per-thread reserved pointer has the LSB setted and non
     * null 63-MSB
     */
    bool isReserved(T ptr) const {
        return (reinterpret_cast<uintptr_t>(ptr) & 1) != 0;
    }
protected:

    bool is_closed_(uint64_t val) const {
        return bit::getMSB64(val) != 0;
    }

    void fixState() {
        while(1) {
            uint64_t t = tail_.load(std::memory_order_relaxed);
            uint64_t h = head_.load(std::memory_order_relaxed);

            if(t != tail_.load(std::memory_order_acquire)) // inconsistent tail
                continue;

            if(h > t) {
                if(!tail_.compare_exchange_strong(t,h,std::memory_order_acq_rel))
                    continue;
            }

            return;
        }
    }
    alignas(CACHE_LINE) std::atomic<uint64_t> head_; ///< Head ticket index for dequeue.
    char pad_head_[CACHE_LINE - sizeof(std::atomic<uint64_t>)];
    alignas(CACHE_LINE)std::atomic<uint64_t> tail_; ///< Tail ticket index for enqueue.
    char pad_tail_[CACHE_LINE - sizeof(std::atomic<uint64_t>)];
    const size_t size_;
    const size_t mask_;
    util::memory::HeapStorage<Cell> array_; ///< Underlying circular buffer storage.
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
template<typename T, typename Proxy, bool Pow2, bool auto_close = true>
class LinkedPRQ:
    public PRQueue<
        T,
        Pow2,
        std::conditional_t<
            auto_close,
            LinkedPRQ<
                T,
                Proxy,
                Pow2
            >,
            void
        >
    >,
    public base::ILinkedSegment<
        T,
        LinkedPRQ<
            T,
            Proxy,
            Pow2
        >
    >
{
    friend Proxy;   ///< Proxy class can access private methods.

    using Base = PRQueue<T,Pow2,std::conditional_t<auto_close,LinkedPRQ<T,Proxy,Pow2>,void>>;

public:
    /**
     * @brief Constructs a linked CAS loop queue segment.
     *
     * @param size Capacity of this segment.
     * @param start Initial sequence number (defaults to 0).
     */
    LinkedPRQ(size_t size, uint64_t start = 0):
        Base(size,start) {}

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
        uint64_t tail = bit::get63LSB(Base::tail_.load(std::memory_order_relaxed));
        return (tail > 0)? (tail-1) : 0;
    }

    bool open() final override {
        uint64_t tail = Base::tail_.load(std::memory_order_relaxed);
        if(Base::is_closed_(tail)) {
            uint64_t head = Base::head_.load(std::memory_order_relaxed);
            next_.store(nullptr,std::memory_order_relaxed);
            Base::tail_.compare_exchange_strong(tail,head,std::memory_order_acq_rel);
        }
        return true;
    }

    std::atomic<LinkedPRQ*> next_{nullptr}; ///< Pointer to the next segment in the chain.
};
