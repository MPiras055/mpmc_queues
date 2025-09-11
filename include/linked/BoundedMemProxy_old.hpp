#include <IProxy.hpp>               // proxy interface
#include <DynamicThreadTicket.hpp>  // cached thread tickets for hazard pointers
#include <Recycler.hpp>      // hazard segment recycle
#include <HeapStorage.hpp>              // for segment storage
#include <specs.hpp>                // padding definition
#include <bit.hpp>                  // bit manipulation

template <typename T, template<typename,typename> typename Seg, size_t Seg_count = 4>
class BoundedMemProxy: public base::IProxy<T,Seg> {
    using Segment = Seg<T, BoundedMemProxy>;
    static constexpr bool is_unbounded = true;
    static inline thread_local uint64_t success = 0;

public:
    explicit BoundedMemProxy(size_t cap, size_t maxThreads) :
        ticketing_{maxThreads}, recycler_{Seg_count,cap / Seg_count,maxThreads}, seg_capacity_{cap / Seg_count} {
        Segment *sentinel = nullptr;
        assert(recycler_.getCache(sentinel) && "BoundedMemProxy - constructor - no segment got form recycler cache");
        tail_.store(sentinel,std::memory_order_relaxed);
        head_.store(sentinel,std::memory_order_relaxed);
    }

    ~BoundedMemProxy() {
        T ignore;
        while(dequeue(ignore)); //implicitly returns segments to the recycler
    }

    bool enqueue(T item) override {
        uint64_t ticket = get_ticket_();    //get per thread ticket
        recycler_.protect(ticket);  //before getting any shared pointers we need to protect the epoch
        Segment* tail = tail_.load(std::memory_order_acquire);
        Segment* localNext = nullptr;
        uint64_t local_version = ~0ull;

        static thread_local int64_t retries = -1;

        //start retry loop
        while(1) {
            if(retries % 1000000 == 0) {
                std::cout << "FATAL: enqueue livelock" << std::endl;
                std::abort();
            }
            retries++;
            recycler_.protect(ticket);  //protection before acquiring a shared pointers
            Segment *tail2 = tail_.load(std::memory_order_acquire);
            if(tail != tail2) {
                tail = tail2;
                continue;   //until tail stabilizes
            }

            //check if the next field was setted
            //no need to acquire protection since next is a field of the tail (if we protected tail we are also protecting next)
            Segment *next = tail->getNext();
            if(next != nullptr) {
                //try to CAS it as new global tail
                (void)(tail_.compare_exchange_strong(tail, next,std::memory_order_release));
                continue;
            }

            //we are protecting tail
            if(safeEnqueue_(tail,item)) {
                recycler_.clear(ticket);
                if(localNext != nullptr)
                    putBack(localNext);
                retries = -1;
                return true;
            }

            //local version is setted if get() fails
            if(local_version == version.load(std::memory_order_acquire)) {
                recycler_.clear(ticket);
                return false;
            }

            //the current segment was closed
            if(localNext == nullptr) {  //we need to acquire a local copy
                recycler_.clear(ticket);    //dropping epoch protection
                local_version = version.load(std::memory_order_acquire);
                if(recycler_.get(localNext,ticket)) {
                    local_version = ~0ull; //got a new segment so reset the version
                    bool ret = localNext->open();
                    ret = ret && localNext->enqueue(item);  //we insert the item here
                    assert(ret && "Problems opening the segment");
                }

                recycler_.protect(ticket);
                tail = tail_.load(std::memory_order_acquire);
                continue;   //need to make sure we get a reliable snapshot of the tail
            }

            next = nullptr;
            //we are still protecting the tail so we can attempt a CAS
            if(tail->next_.compare_exchange_strong(next,localNext)) { //we successfuly linked our segment
                //successfuly linked [update version to signal new tail]
                version.fetch_add(1,std::memory_order_acq_rel);
                //still protecting tail [update the current global tail]
                (void) tail_.compare_exchange_strong(tail,localNext);
                recycler_.clear(ticket);
                retries = -1;
                return true;
            }


        }
    }

    /**
     * @brief puts back a segment in the recycler cache
     * @par seg: segment pointer
     * @warning after this method the segment is not usable anymore
     */
    void putBack(Segment* ptr) {
        T ignore{nullptr};
        int success = 0;
        int nulls = 0;
        while(ptr->dequeue(ignore)) {
            success++;
        }
        assert(success != 0 && "Empty Segment should have supported a single dequeue");
        assert(success == 1 && "Multiple dequeues on the same segment");
        recycler_.putCache(ptr);
    }



    bool dequeue(T& out) override {
        uint64_t ticket = get_ticket_();

        recycler_.protect(ticket);  //update protection
        Segment *head = head_.load(std::memory_order_acquire);
        while(1) {
            //check for head consistency
            recycler_.protect(ticket);  //update protection
            Segment* head2 = head_.load(std::memory_order_acquire);
            if(head != head2) {
                head = head2;
                continue; //until head stabilizes
            }

            //try to dequeue on current segment
            if(!head->dequeue(out)) {
                //if segment empty check for next


                Segment *next = head->getNext();
                if(next == nullptr) {
                    //if no next then nothing to dequeue
                    recycler_.clear(ticket);
                    return false;
                }

                //next was setted: try one more time to dequeue on the current segment
                if(!head->dequeue(out)) {
                    //if dequeue failed then no-one will enqueue on this segment
                    //try to update the current head
                    Segment* exp = head;
                    if(head_.compare_exchange_strong(exp,next)) {
                        recycler_.recycle(head); //put the segment in the recycler
                        head = next;    //successfuly updated to next
                    } else {
                        head = exp; //failed somebody updated the head before us
                    }

                    continue;
                }
            }

            recycler_.clear(ticket);
            return true;
        }
    }


    /**
     * @brief get the underlying segment capacity
     * @returns `size_t` capacity of all segments
     */
    size_t capacity() const override { return seg_capacity_ * Seg_count; }

    /**
     * @brief get an approximation of the total number of elements the queue holds
     *
     * @warning requires the thread to have acquired an operation slot
     */
    size_t size() override {
        uint64_t tail,head;
        uint64_t ticket = get_ticket_();
        recycler_.protect(ticket);  //operating on shared memory
        Segment *tail_seg = tail_.load(std::memory_order_acquire);
        tail = bit::get63LSB(tail_seg->tail_.load(std::memory_order_acquire));
        Segment *head_seg = head_.load(std::memory_order_acquire);
        head = head_seg->head_.load(std::memory_order_acquire);
        recycler_.clear(ticket);
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
        static thread_local uint64_t lastSeen = ~0ull;
        uint64_t localVersion = version.load(std::memory_order_acquire);
        if ((lastSeen == localVersion) && tail->isClosed()) {
            return false;  // Don't attempt enqueue if the segment is already closed

        } if (!tail->enqueue(item)) {
            lastSeen = localVersion;
            return false;  // Enqueue failed, mark the segment as stale/full
        }

        return true;
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
    alignas(CACHE_LINE) std::atomic<uint64_t> version{0}; //ABA prevention
    char pad_new_[CACHE_LINE - sizeof(std::atomic<uint64_t>)];
    util::threading::DynamicThreadTicket ticketing_;
    util::hazard::Recycler<Segment> recycler_;
    size_t const seg_capacity_;
};
