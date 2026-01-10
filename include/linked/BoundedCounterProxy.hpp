#include <IProxy.hpp>               //proxy interface
#include <DynamicThreadTicket.hpp>  //cached thread tickets for hazard pointers
#include <HazardVector.hpp>         //basic hazard pointer implementation
#include <atomic>
#include <specs.hpp>                //padding definition
#include <bit.hpp>                  //bit manipulation
#include <OptionsPack.hpp>

struct BoundedCounterProxyOpt {
    template<size_t N> struct ChunkFactor{};
};

template <
    typename T,
    template<typename,typename,typename,typename> typename Seg,
    typename ProxyOpt   = meta::EmptyOptions,
    typename SegmentOpt = meta::EmptyOptions
>
class BoundedCounterProxy:
    public base::IProxy<T,Seg<T,void,SegmentOpt,ProxyOpt>> {
    using Segment = Seg<T, BoundedCounterProxy,SegmentOpt,void>;
    using Ticket = util::threading::DynamicThreadTicket::Ticket;

    static constexpr bool INFO_REQUIRED = Segment::info_required;

    struct ThreadMetadata {
        Segment* lastSeen{nullptr};
    };

    inline bool dequeueAfterNextLinked(Segment* lhead,T& out) {
        // This is a hack for LinkedSCQ.
        // See SCQ::prepareDequeueAfterNextLinked for details.
        if constexpr(requires(Segment s) {
        s.prepareDequeueAfterNextLinked();
        }) {
            lhead->prepareDequeueAfterNextLinked();
        }

        return lhead->dequeue(out);
    }

public:

    //If Chunk factor is not defined, the queue has one chunk
    static constexpr size_t ChunkFactor = ProxyOpt::template get<BoundedCounterProxyOpt::ChunkFactor,1>;
    static_assert(ChunkFactor >= 1, "ChunkFactor must be >= 1 === queue capacity must be a direct multiple of ChunkFactor");

    explicit BoundedCounterProxy(size_t cap, size_t maxThreads) :
        seg_capacity_{cap/ChunkFactor},full_capacity_{cap},
        ticketing_{maxThreads},hazard_{maxThreads} {
        assert(cap != 0 && "Queue Capacity must be non-null");
        assert(cap % ChunkFactor == 0 && "Capacity must be a multiple of chunkFactor");
        Segment* sentinel = new Segment(seg_capacity_,0);
        head_.store(sentinel,std::memory_order_relaxed);
        tail_.store(sentinel,std::memory_order_relaxed);
    }

    ~BoundedCounterProxy() final override {
        T ignore;
        while(dequeue(ignore));
        delete head_.load(std::memory_order_seq_cst);
    }

    bool enqueue(T item) noexcept final override {
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

            //check if new enqueue respects the capacity
            if(!capacity_respected_()) {
                hazard_.clear(ticket);
                return false;
            }

            //try to enqueue on current segment
            if(safeEnqueue_(tail,ticket,item)) {
                break;
            }

            // since enqueue failed someone could have pushed
            if(!capacity_respected_()) {
                hazard_.clear(ticket);
                return false;
            }

            //enqueue failed: segment is full or stale
            //allocate a new segment and push current item
            Segment *newTail;
            if constexpr (Segment::optimized_alloc) {
                newTail = Segment::create(seg_capacity_);
            } else {
                newTail = new Segment(seg_capacity_);
            }
            (void)newTail->enqueue(item);

            Segment* null = nullptr;
            //try to link the private segment as the new tail
            if (tail->next_.compare_exchange_strong(null, newTail)) {
                //try to update the global tail pointer
                (void)tail_.compare_exchange_strong(tail, newTail);
                break;
            }
            tail = hazard_.protect(null, ticket);
            delete newTail; //failed: another tail was already linked

        }
        hazard_.clear(ticket);
        recordEnqueue();
        return true;
    }



    bool dequeue(T& out) noexcept final override {
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
                if(!dequeueAfterNextLinked(head,out)) {
                    //if dequeue failed then no-one will enqueue on this segment
                    //try to update the current head
                    if(head_.compare_exchange_strong(head,next)) {
                        //record the old segment
                        hazard_.retire(head,ticket);
                        //update the current segment
                        head = hazard_.protect(next,ticket);
                    } else {
                        head = hazard_.protect(head,ticket);
                    }
                    continue;
                }
            }

            hazard_.clear(ticket);
            recordDequeue();
            return true;
        }
    }

    /**
     * @brief get the underlying segment capacity
     * @returns `size_t` capacity of all segments
     *
     * @warning: the underlying queue capacity may exceed this value up to seg_capacity_ * maxThreads
     */
    size_t capacity() const noexcept final override { return seg_capacity_ * ChunkFactor; }

    /**
     * @brief get an approximation of the total number of elements the queue holds
     *
     * @warning requires the thread to have acquired an operation slot
     */
    size_t size() const noexcept final override {
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
    bool acquire() noexcept final override {
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
    void release() noexcept final override {
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
     * The Segment::info_required flag allows us to optimize this method for any
     * given segment
     *
     * This method uses a cached pointer (see HazardCell for caching implementation),
     * and for each segment enqueue call provides the segment with info on whether itself
     * may be already closed. If enqueue fails then the
     *
     *  @warning requires the pointer to be hazard protected
     */
    inline bool safeEnqueue_(Segment *tail,Ticket t,T item) {
        if constexpr (INFO_REQUIRED) {
            Segment*& lastSeen = hazard_.getMetadata(t).lastSeen;
            bool info = tail == lastSeen;

            bool enq_ok = tail->enqueue(item,info);
            lastSeen = enq_ok? nullptr : tail;
            return enq_ok;
        } else {
            return tail->enqueue(item);
        }
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
    inline Ticket get_ticket_() {
        Ticket retval;
        bool ok = ticketing_.acquire(retval);
        assert(ok && "Warning: no ticket could be acquired");
        return retval;
    }

    inline void recordEnqueue() {
        itemsPushed_.fetch_add(1,std::memory_order_release);
    }

    inline void recordDequeue() {
        itemsPopped_.fetch_add(1,std::memory_order_release);
    }

    ALIGNED_CACHE std::atomic<Segment*> head_{nullptr};
    CACHE_PAD_TYPES(std::atomic<Segment*>);
    ALIGNED_CACHE std::atomic<Segment*> tail_{nullptr};
    CACHE_PAD_TYPES(std::atomic<Segment*>);
    ALIGNED_CACHE std::atomic<uint64_t> itemsPushed_{0};
    CACHE_PAD_TYPES(std::atomic<uint64_t>);
    ALIGNED_CACHE std::atomic<uint64_t> itemsPopped_{0};
    CACHE_PAD_TYPES(std::atomic<uint64_t>);
    const size_t seg_capacity_;
    const size_t full_capacity_;
    util::threading::DynamicThreadTicket ticketing_;
    util::hazard::HazardVector<Segment*,ThreadMetadata> hazard_;

};
