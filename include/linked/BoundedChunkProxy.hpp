#include <IProxy.hpp>               //proxy interface
#include <DynamicThreadTicket.hpp>  //cached thread tickets for hazard pointers
#include <HazardVector.hpp>         //basic hazard pointer implementation
#include <atomic>
#include <specs.hpp>                //padding definition
#include <bit.hpp>                  //bit manipulation
#include <OptionsPack.hpp>          //Options


struct BoundedChunkProxyOpt {
    template<size_t N> struct ChunkFactor{};
};

template <
    typename T,
    template<typename,typename,typename,typename> typename Seg,
    typename ProxyOpt   = meta::EmptyOptions,
    typename SegmentOpt = meta::EmptyOptions
>
class BoundedChunkProxy:
    public base::IProxy<T,Seg<T,void,SegmentOpt,ProxyOpt>> {
    using Segment = Seg<T, BoundedChunkProxy,SegmentOpt,void>;
    static constexpr bool INFO_REQUIRED = Segment::info_required;
    using Ticket = util::threading::DynamicThreadTicket::Ticket;    //ticket for thread access

    struct ThreadMetadata { //the whole struct gets automatically aligned and padded to cache Line;
        std::atomic_int64_t OpCounter{0};
        uint64_t lastSeen{0};
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

    static constexpr size_t ChunkFactor = ProxyOpt::template get<BoundedChunkProxyOpt::ChunkFactor,4>;
    static_assert(ChunkFactor > 1, "ChunkFactor must be greater than 1 === queue capacity must be a direct multiple of ChunkFactor");

    explicit BoundedChunkProxy(size_t cap, size_t maxThreads) :
        ticketing_{maxThreads},hazard_{maxThreads},seg_capacity_{cap / ChunkFactor} {
        assert(cap != 0 && "Segment Capacity must be non-null");
        assert(cap % ChunkFactor == 0 && "Queue capacity must be a direct multiple of ChunkFactor");
        assert(cap / ChunkFactor > 0 && "Queue capacity must be a direct multple of ChunkFactor");
        Segment* sentinel = new Segment(seg_capacity_);
        head_.store(sentinel,std::memory_order_relaxed);
        tail_.store(sentinel,std::memory_order_relaxed);
    }

    ~BoundedChunkProxy() {
        T ignore;
        while(dequeue(ignore));
        delete head_.load(std::memory_order_seq_cst);
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
            Segment* newTail = new Segment(item,seg_capacity_);

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
                if(!dequeueAfterNextLinked(head,out)) {
                    //if dequeue failed then no-one will enqueue on this segment
                    //try to update the current head
                    if(head_.compare_exchange_strong(head,next)) {
                        (void)head_idx_.fetch_add(1,std::memory_order_release);
                        //retire the current segment (delayed - since the caller holds protection)
                        (void)hazard_.retire(head,ticket);
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
     *
     * @warning: actual capacity may grow (due to unexpected timing windows) up to
     * (seg_capacity_ + 1) * ChunkFactor;
     */
    size_t capacity() const override { return seg_capacity_ * ChunkFactor; }

    /**
     * @brief size method
     *
     * @note doesn't require a thread to have acquired an operational slot
     */
    size_t size() const noexcept final override {
        int64_t total = 0;
        hazard_.metadataIter([&total](const ThreadMetadata& m) {
            total += m.OpCounter.load(std::memory_order_relaxed);
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
    * @brief records an enqueue in the caller thread metadata section
    */
    inline void recordEnqueue(Ticket t) noexcept {
        hazard_.getMetadata(t).OpCounter.fetch_add(1,std::memory_order_relaxed);
    }

    /**
    * @brief records a dequeue in the caller thread metadata section
    */
    inline void recordDequeue(Ticket t) noexcept {
        hazard_.getMetadata(t).OpCounter.fetch_sub(1,std::memory_order_relaxed);
    }

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
    inline bool safeEnqueue_(Segment *tail, Ticket t, T item) {
        if constexpr (INFO_REQUIRED) {
            uint64_t& lastSeen = hazard_.getMetadata(t).lastSeen;
            uint64_t actualTail = tail_idx_.load(std::memory_order_relaxed);
            bool info = actualTail == lastSeen;
            bool enq_ok = tail->enqueue(item,info);
            lastSeen = enq_ok? 0 : actualTail;
            return enq_ok;
        } else {
            return tail->enqueue(item);
        }
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

        return ((tail - head) + 1) < ChunkFactor;

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

    ALIGNED_CACHE std::atomic<Segment*> head_{nullptr};
    CACHE_PAD_TYPES(std::atomic<Segment*>);
    ALIGNED_CACHE std::atomic<Segment*> tail_{nullptr};
    CACHE_PAD_TYPES(std::atomic<Segment*>);
    ALIGNED_CACHE std::atomic<uint64_t> tail_idx_{1};
    CACHE_PAD_TYPES(std::atomic<uint64_t>);
    ALIGNED_CACHE std::atomic<uint64_t> head_idx_{1};
    CACHE_PAD_TYPES(std::atomic<uint64_t>);
    util::threading::DynamicThreadTicket ticketing_;
    util::hazard::HazardVector<Segment*,ThreadMetadata> hazard_;
    const size_t seg_capacity_;

};
