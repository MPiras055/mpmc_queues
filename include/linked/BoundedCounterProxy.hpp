#include <IProxy.hpp>               //proxy interface
#include <DynamicThreadTicket.hpp>  //cached thread tickets for hazard pointers
#include <HazardVector.hpp>         //basic hazard pointer implementation
#include <specs.hpp>                //padding definition
#include <bit.hpp>                  //bit manipulation

template <typename T, template<typename,typename> typename Seg, size_t Seg_count = 4>
class BoundedCounterProxy: public base::IProxy<T,Seg> {
    using Segment = Seg<T, BoundedCounterProxy>;
    static constexpr bool is_unbounded = true;

public: 
    explicit BoundedCounterProxy(size_t cap, size_t maxThreads) : 
        ticketing_{maxThreads},hazard_{maxThreads},seg_capacity_{cap / Seg_count},full_capacity_{cap} {
        assert(cap != 0 && "Segment Capacity must be non-null");
        assert(cap / Seg_count > 0 && "Underlying segment capacity overflow");
        Segment* sentinel = new Segment(seg_capacity_,0);
        head_.store(sentinel,std::memory_order_relaxed);
        tail_.store(sentinel,std::memory_order_relaxed);
    }

    ~BoundedCounterProxy() {
        T ignore;
        while(this->dequeue(ignore));
        delete head_.load();
    }

    bool enqueue(T item) override {
        uint64_t ticket = get_ticket_();

        Segment* tail = hazard_.protect(tail_.load(std::memory_order_relaxed), ticket);

        while (true) {
            //check for tail consistency
            Segment* tail2 = tail_.load(std::memory_order_acquire);
            if (tail != tail2) {
                tail = hazard_.protect(tail2, ticket);
                continue;
            }

            //check if next ptr was setted
            Segment* next = tail->getNext();
            if (next != nullptr) {
                //try update the tail pointer globally
                bool ret = tail_.compare_exchange_strong(tail,next);
                tail = hazard_.protect(ret? next : tail, ticket);
                continue;
            }

            // the enqueue operation breaks the capacity constraint
            if(!capacity_respected_()) {
                hazard_.clear(ticket);
                return false;
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
                //try to update the global tail pointer
                (void)tail_.compare_exchange_strong(tail, newTail);
                break;
            } 
                
            delete newTail; //failed: another tail was already linked
            //acquire protection on the current new tail
            tail = hazard_.protect(null, ticket);
        }
        itemsPushed_.fetch_add(1,std::memory_order_release);
        hazard_.clear(ticket);
        return true;
    }



    bool dequeue(T& out) override {
        uint64_t ticket = get_ticket_();
        Segment *head = hazard_.protect(head_.load(std::memory_order_relaxed),ticket);
        while(1) {
            //check for head consistency
            Segment* head2 = head_.load(std::memory_order_acquire);
            if(head != head2) {
                head = hazard_.protect(head2,ticket);
                continue;
            }

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
                        //retire the current segment
                        hazard_.retire(head,ticket);
                        head = hazard_.protect(next,ticket);
                    } else {
                        head = hazard_.protect(head,ticket);
                    }
                    continue;
                }
            }

            itemsPopped_.fetch_add(1,std::memory_order_release);
            hazard_.clear(ticket);
            return true;
        }
    }


    /**
     * @brief get the underlying segment capacity
     * @returns `size_t` capacity of all segments
     */
    size_t capacity() const override { return full_capacity_; }

    /**
     * @brief get an approximation of the total number of elements the queue holds
     * 
     * @warning requires the thread to have acquired an operation slot
     */
    size_t size() override {
        return  itemsPushed_.load(std::memory_order_relaxed) - 
                itemsPopped_.load(std::memory_order_acquire);
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
        uint64_t ignore;
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
        static thread_local Segment *lastSeen = nullptr;

        if (lastSeen == tail && tail->isClosed()) {
            return false;  // Don't attempt enqueue if the segment is already closed
        }
        
        if (!tail->enqueue(item)) {
            lastSeen = tail;
            return false;  // Enqueue failed, mark the segment as stale/full
        }

        lastSeen = nullptr;
        return true;
    }


    /**
     * @brief checks if a successful enqueue would respect the capacity provided
     */
    inline bool capacity_respected_() const {
        return  (itemsPushed_.load(std::memory_order_relaxed) - 
                itemsPopped_.load(std::memory_order_acquire)) <
                full_capacity_;

    }

    /**
     * @brief internal get_ticket function
     * 
     * @note asserts that the calling thread possesses a ticket
     */
    inline uint64_t get_ticket_() {
        uint64_t retval;
        assert(ticketing_.acquire(retval) && "Warning: no ticket could be acquired");
        return retval;
    }

    alignas(CACHE_LINE) std::atomic<Segment*> head_{nullptr};
    char pad_head_[CACHE_LINE - sizeof(std::atomic<Segment *>)];
    alignas(CACHE_LINE) std::atomic<Segment*> tail_{nullptr};
    char pad_tail_[CACHE_LINE - sizeof(std::atomic<Segment *>)];
    alignas(CACHE_LINE) std::atomic<uint64_t> itemsPushed_{0};
    char pad_pushed_[CACHE_LINE - sizeof(std::atomic<uint64_t>)];
    alignas(CACHE_LINE) std::atomic<uint64_t> itemsPopped_{0};
    char pad_popped_[CACHE_LINE - sizeof(std::atomic<uint64_t>)];
    util::threading::DynamicThreadTicket ticketing_;
    util::hazard::HazardVector<Segment*> hazard_;
    const size_t seg_capacity_;
    const size_t full_capacity_;
    
};