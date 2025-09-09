#include <IProxy.hpp>               //proxy interface
#include <DynamicThreadTicket.hpp>  //cached thread tickets for hazard pointers
#include <HazardVector.hpp>         //basic hazard pointer implementation
#include <specs.hpp>                //padding definition
#include <bit.hpp>                  //bit manipulation

template <typename T, template<typename,typename> typename Seg>
class UnboundedProxy: public base::IProxy<T,Seg> {
    using Segment = Seg<T, UnboundedProxy>;

public: 
    explicit UnboundedProxy(size_t cap, size_t maxThreads) : 
        ticketing_{maxThreads},hazard_{maxThreads},seg_capacity_{cap} {
        assert(cap != 0 && "Segment Capacity must be non-null");
        Segment* sentinel = new Segment(cap,0);
        head_.store(sentinel,std::memory_order_relaxed);
        tail_.store(sentinel,std::memory_order_relaxed);
    }

    ~UnboundedProxy() {
        T ignore;
        while(dequeue(ignore));
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

            //try to enqueue on current segment
            if (tail->enqueue(item)) {
                break;
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
            } else {
                delete newTail; //failed: another tail was already linked
            }
            //acquire protection on the current new tail
            tail = hazard_.protect(null, ticket);
        }
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

            hazard_.clear(ticket);
            return true;
        }
    }


    /**
     * @brief get the underlying segment capacity
     * @returns `size_t` capacity of all segments
     */
    size_t capacity() const override { return seg_capacity_; }

    /**
     * @brief get an approximation of the total number of elements the queue holds
     * 
     * @warning requires the thread to have acquired an operation slot
     */
    size_t size() override {
        uint64_t tail,head;
        uint64_t ticket = get_ticket_();
        Segment *tail_seg = hazard_.protect(tail_,ticket);
        tail = bit::get63LSB(tail_seg->tail_.load(std::memory_order_relaxed));
        Segment *head_seg = hazard_.protect(head_,ticket);
        head = head_seg->head_.load(std::memory_order_acquire);
        hazard_.clear(get_ticket_());
        return head > tail ? 0 : tail - head;
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
    util::threading::DynamicThreadTicket ticketing_;
    util::hazard::HazardVector<Segment*> hazard_;
    const size_t seg_capacity_;
    
};