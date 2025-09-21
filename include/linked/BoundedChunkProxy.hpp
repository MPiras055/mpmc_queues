#include <IProxy.hpp>               //proxy interface
#include <DynamicThreadTicket.hpp>  //cached thread tickets for hazard pointers
#include <HazardVector.hpp>         //basic hazard pointer implementation
#include <specs.hpp>                //padding definition
#include <bit.hpp>                  //bit manipulation

template <
    typename T,
    template<typename,typename,bool,bool> typename Seg,
    size_t Seg_count = 4,
    bool Pow2 = false
>
class BoundedChunkProxy: public base::IProxy<T,Seg> {
    using Segment = Seg<T, BoundedChunkProxy,Pow2,true>;
    using ThreadRecord = std::atomic<int64_t>;  //for accurate length computation
    using Ticket = util::threading::DynamicThreadTicket::Ticket;    //ticket for thread access
    static_assert(Seg_count != 0, "Chunk factor cannot be 0");

public:
    static constexpr size_t Segments = Seg_count;
    explicit BoundedChunkProxy(size_t cap, size_t maxThreads) :
        ticketing_{maxThreads},hazard_{maxThreads},seg_capacity_{cap / Seg_count},full_capacity_{cap} {
        assert(cap != 0 && "Segment Capacity must be non-null");
        assert(cap / Seg_count > 0 && "Underlying segment capacity overflow");
        assert(maxThreads > 0 && "Cannot manage 0 threads");
        Segment* sentinel = new Segment(seg_capacity_,0);
        head_.store(sentinel,std::memory_order_relaxed);
        tail_.store(sentinel,std::memory_order_relaxed);
    }

    ~BoundedChunkProxy() {
        T ignore;
        while(dequeue(ignore));
        delete head_.load();
    }

    bool enqueue(T item) override {
        Ticket ticket = get_ticket_();

        while (true) {
            //check for tail consistency
            Segment* tail= hazard_.protect(tail_,ticket);

            //check if next ptr was setted
            Segment* next = tail->getNext();

            if (next != nullptr) {
                //try update the tail pointer globally
                bool ret = tail_.compare_exchange_strong(tail,next);
                tail = hazard_.protect(ret? next : tail, ticket);
                continue;
            }

            //try to enqueue on current segment
            if(safeEnqueue_(tail,item)) {
                break;
            }

            // since enqueue failed someone could have pushed
            if(!capacity_respected_()) {
                hazard_.clear(ticket);
                return false;
            }

            //enqueue failed: segment is full or stale
            //allocate a new segment and push current item
            Segment* newTail = new Segment(seg_capacity_, tail->getNextStartIndex());
            (void)newTail->enqueue(item);

            Segment* null = nullptr;
            //try to link the private segment as the new tail
            if (tail->next_.compare_exchange_strong(null, newTail)) {
                tail_idx_.fetch_add(1,std::memory_order_release);
                //try to update the global tail pointer
                (void)tail_.compare_exchange_strong(tail, newTail);
                break;
            }
            tail = hazard_.protect(null, ticket);
            delete newTail; //failed: another tail was already linked

        }
        hazard_.clear(ticket);
        recordEnqueue(ticket);
        return true;
    }



    bool dequeue(T& out) override {
        Ticket ticket = get_ticket_();
        while(1) {
            //check for head consistency
            Segment* head = hazard_.protect(head_,ticket);

            //try to dequeue on current segment
            if(!head->dequeue(out)) {
                //if segment empty check for next
                Segment *next = head->getNext();
                if(next == nullptr) {
                    //if no next then nothing to dequeue
                    hazard_.clear(ticket);
                    return false;
                }

                //next was setted: try one more time to dequeue on the current segment
                if(!head->dequeue(out)) {
                    //if dequeue failed then no-one will enqueue on this segment
                    //try to update the current head
                    if(head_.compare_exchange_strong(head,next)) {
                        head_idx_.fetch_add(1,std::memory_order_release);
                        //retire the current segment (delayed - since the caller holds protection)
                        hazard_.retire(head,ticket);
                        //update protection on the current segment
                        head = hazard_.protect(next,ticket);
                    } else {
                        head = hazard_.protect(head,ticket);
                    }
                    continue;
                }
            }
            hazard_.clear(ticket);
            recordDequeue(ticket);
            return true;
        }
    }


    /**
     * @brief get the underlying segment capacity
     * @returns `size_t` capacity of all segments
     */
    size_t capacity() const override { return full_capacity_; }

    /**
     * @brief size method
     *
     * @note doesn't require a thread to have acquired an operational slot
     */
    size_t size() const override {
        int64_t total = 0;
        hazard_.metadataIter([&total](const std::atomic<int64_t>& m) {
            total += m.load(std::memory_order_relaxed);
        });
        assert(total >= 0 && "Negative size detected");
        return static_cast<size_t>(total);
    }

    /**
     * @brief books a ticket for the calling thread
     *
     * Operation on proxy requires all threads to be tracked for memory management.
     * A threads that intends to operate on the data structure requires to acquire
     * a slot.
     *
     * @return true if the slot has been acquired false otherwise
     * @warning operating on the data structure without acquiring a slot results in
     * undefined behaviour
     */
    bool acquire() override {
        Ticket ignore;
        return ticketing_.acquire(ignore);

    }

    /**
     * @brief clears the calling thread ticket
     *
     * @return void
     * @note this method is idempotent (calling it multiple times results in no
     * side effects)
     */
    void release() override {
        return ticketing_.release();
    }

private:

    /**
    * @brief records an enqueue in the caller thread metadata section
    */
    inline void recordEnqueue(Ticket t) {
        hazard_.getMetadata(t).fetch_add(1,std::memory_order_relaxed);
    }

    /**
    * @brief records a dequeue in the caller thread metadata section
    */
    inline void recordDequeue(Ticket t) {
        hazard_.getMetadata(t).fetch_sub(1,std::memory_order_relaxed);
    }

    /**
     * @brief wrapper for enqueue on segment (livelock prevention)
     *
     * Segments have a `close` flag that blocks further insertions.
     * On some segments, if the flag is setted, trying further insertions
     * can make dequeues have to do extra work (to reallineate indexes) and
     * in some cases lead to livelock phoenomena.
     *
     * This method uses a TLS cached tail pointer, to avoid calling inner
     * segment enqueues if the segment was already recorded as close
     *
     *  @warning requires the pointer to be hazard protected
     */
    inline bool safeEnqueue_(Segment *tail, T item) {
    // Thread-local pointer to track the last seen tail that was closed or full
        static thread_local uint64_t lastSeen = 0;

        if ((lastSeen == tail_idx_.load(std::memory_order_relaxed)) && tail->isClosed()) {
            return false;  // Don't attempt enqueue if the segment is already closed
        }

        if (!tail->enqueue(item)) {
            lastSeen = tail_idx_.load(std::memory_order_acquire);
            return false;  // Enqueue failed, mark the segment as stale/full
        }

        lastSeen = 0;
        return true;
    }


    /**
     * @brief checks if a successful enqueue would respect the capacity provided
     *
     * @note checks if the max number of segments was allocated
     */
    inline bool capacity_respected_() const {
        uint64_t tail,head;
        tail = tail_idx_.load(std::memory_order_relaxed);
        head = head_idx_.load(std::memory_order_acquire);

        return ((tail - head) + 1) < Seg_count;

    }

    /**
     * @brief internal get_ticket function
     *
     * @note asserts that the calling thread possesses a ticket
     */
    inline uint64_t get_ticket_() {
        uint64_t retval;
        bool ok = ticketing_.acquire(retval);
        assert(ok && "Warning: no ticket could be acquired");
        return retval;
    }

    align std::atomic<Segment*> head_{nullptr};
    CACHE_PAD_TYPES(std::atomic<Segment*>);
    align std::atomic<Segment*> tail_{nullptr};
    CACHE_PAD_TYPES(std::atomic<Segment*>);
    align std::atomic<uint64_t> tail_idx_{1};
    CACHE_PAD_TYPES(std::atomic<uint64_t>);
    align std::atomic<uint64_t> head_idx_{1};
    CACHE_PAD_TYPES(std::atomic<uint64_t>);
    util::threading::DynamicThreadTicket ticketing_;
    util::hazard::HazardVector<Segment*,ThreadRecord> hazard_;
    const size_t seg_capacity_;
    const size_t full_capacity_;

};
