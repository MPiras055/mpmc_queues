#include <IProxy.hpp>               //proxy interface
#include <DynamicThreadTicket.hpp>  //cached thread tickets
#include <Recycler.hpp>             //memory recycler
#include <atomic>
#include <cstdint>
#include <specs.hpp>                //padding definition
#include <bit.hpp>                  //bitwise manipulation

template <
    typename T, //type of the queue item
    template<typename T1,typename Proxy, bool p = true, bool auto_close = true> typename SegmentType, //deferred segment type
    size_t ChunkFactor = 4,         //recycler's pool size
    bool Pow2 = false,              //segment pow2
    bool RecyclerPow2 = false,      //enable pow2 optimization for recycler queues
    bool useCache = true            //enable recycler's cache
> class BoundedMemProxy: public base::IProxy<T, SegmentType> {

    using Segment = SegmentType<T,BoundedMemProxy,Pow2,true>;
    //for this design we NECESSARILY need the recycler to use the cache
    using Recycler = util::hazard::Recycler<Segment,std::atomic<int64_t>,useCache,RecyclerPow2>;
    using Index = Recycler::Tml;
    using Version = uint32_t;
    using Ticket = util::threading::DynamicThreadTicket::Ticket;
    using TaggedPtr = uint64_t;

    static_assert(
                ((sizeof(Version) + sizeof(Index)) <= sizeof(TaggedPtr)) &&
                (sizeof(TaggedPtr) <= sizeof(Segment*)),
                    "Version and index cannot fit in a single memory word"
    );

public:
    static constexpr size_t Segments = ChunkFactor;
    explicit BoundedMemProxy(size_t capacity, size_t maxThreads):
            ticketing_(maxThreads),
            seg_capacity_((Pow2? bit::next_pow2(capacity) : capacity) / ChunkFactor),
            recycler_(ChunkFactor,maxThreads,seg_capacity_) {

        assert(
            ((Pow2? bit::next_pow2(capacity) : capacity) % ChunkFactor) == 0 &&
            "Queue capacity has to be a multiple of chunk factor (Chunkfactor Default = 4)"
        );
        assert(seg_capacity_ != 0 &&
            "Segment Capacity underflow detected"
        );
        assert(maxThreads != 0 &&
            "The managed number of threads must be non null"
        );

        Index SentinelIndex;
        bool ok;
        if constexpr (useCache) {
            ok = recycler_.get_cache(SentinelIndex);
        } else {
            ok = recycler_.reclaim(SentinelIndex,0); //we can use any ticket up to maxThreads since all others are inactive
        }

        assert(ok &&
            "No Sentinel segment could have been get"
        );

        TaggedPtr Sentinel = getTagged(getNewVersion(), SentinelIndex);
        head_.store(Sentinel);
        tail_.store(Sentinel);

        //Initialize thread metadata for accurate length tracking
        for(size_t i = 0; i < maxThreads; i++) {
            recycler_.getMetadata(i).store(0,std::memory_order_relaxed);
        }

        //check if next pointer is setted correctly
        assert(
            recycler_.decode(SentinelIndex)->getNext() == nullptr &&
            "Constructor: next pointer not correct"
        );

    }

    ~BoundedMemProxy() {
        T ignore;
        while(dequeue(ignore));
    }

    /**
     * @brief proxy enqueue operation
     *
     * @debug: have to rework the protection scheme in order to minimize
     * protection windows. Make the link logic easier
     */
    // bool enqueue(T item) override {
    //     Ticket t = get_ticket_();
    //     bool failedReclaim{false};
    //     while(1) {
    //         TaggedPtr taggedTail = recycler_.protect_epoch_and_load(t,tail_);

    //         if(updateNextTail(taggedTail)){
    //             continue;
    //         }

    //         if(safeEnqueue(taggedTail,item)) {
    //             recycler_.clear_epoch(t);
    //             recordEnqueue(t);
    //             return true;
    //         }

    //         //failed we have to check and reclaim
    //         Index newQueueIndex = 0;
    //         if(recycler_.get_cache(newQueueIndex)) {
    //             //reset the reclaim flag
    //             failedReclaim = false;
    //             //enqueue your item
    //             Segment *newQueue = recycler_.decode(newQueueIndex);
    //             assert(newQueue->isOpened() && "Private queue from cache wasn't opened");
    //             T ignore;
    //             assert(newQueue->dequeue(ignore) == false && "Not expected dequeue from private segment");
    //             (void)newQueue->enqueue(item);
    //             Version v = getNewVersion();
    //             newQueue = ptrCast(getTagged(v,newQueueIndex));
    //             Segment *nullNode = nullptr;
    //             Segment *currentTail = recycler_.decode(getIndex(taggedTail));
    //             //try to link
    //             if(currentTail->next_.compare_exchange_strong(nullNode,newQueue)) {
    //                 //link successful
    //                 (void) tail_.compare_exchange_strong(taggedTail,taggedCast(newQueue));
    //                 recycler_.clear_epoch(t);
    //                 recordEnqueue(t);
    //                 return true;
    //             }  else {
    //                 //dequeue your item and place
    //                 size_t success = 0;
    //                 while(recycler_.decode(newQueueIndex)->dequeue(ignore)) {
    //                     success++;
    //                 }
    //                 assert(success == 1 && "More enqueues from private segment than expected");
    //                 recycler_.put_cache(newQueueIndex);
    //             }
    //         } else {
    //             //try to reclaim
    //             recycler_.clear_epoch(t);
    //             if(recycler_.reclaim(newQueueIndex,t)) {
    //                 //clear this queue and place it in cache
    //                 Segment* reclaimedQueue = recycler_.decode(newQueueIndex);
    //                 assert(reclaimedQueue->isClosed() && "Reclaimed queue wasn't closed");
    //                 //open the previously cloead queue
    //                 (void) reclaimedQueue->open();
    //                 recycler_.put_cache(newQueueIndex);
    //             } else if(failedReclaim) {
    //                 return false;
    //             } else {
    //                 failedReclaim = true;
    //             }
    //         }
    //     }
    // }

    bool enqueue(T item) override {
        Ticket t = get_ticket_();
        bool failedReclamation{false};
        Version lastSeen{0};

        while(1) {
            TaggedPtr taggedTail = recycler_.protect_epoch_and_load(t, tail_);

            // ==================
            // Preliminary checks
            // ==================

            if(updateNextTail(taggedTail)) {
                failedReclamation = false;
                continue;
            }

            if(failedReclamation) {
                //no segment has been got in previous iteration and tail hasn't changed
                //queue is most likely full
                if(lastSeen == getVersion(taggedTail)) {
                     recycler_.clear_epoch(t);
                     return false;
                }
                failedReclamation = false;
            }

            // =====================
            //  Enqueue on segment
            // =====================

            if(safeEnqueue(taggedTail,item)) {
                break;
            }

            //get the current snapshot of the the tail
            TaggedPtr taggedTail2 = tail_.load(std::memory_order_acquire);
            if(taggedTail != taggedTail2) {
                continue;   //don't even try to link (tail has changed)
            }

            // =====================
            // New segment link
            // =====================

            Index newTailIndex;
            // TRY TO GET A NEW SEGMENT
            if(!recycler_.get_cache(newTailIndex)) {
                recycler_.clear_epoch(t);
                if(!recycler_.reclaim(newTailIndex,t)) {
                    // NO SEGMENT GOT (SET FLAG FOR NEXT ITERATION)
                    failedReclamation = true;
                    lastSeen = getVersion(taggedTail);
                    continue;
                }

                // IF TAIL HAS CHANGED, DON'T BOTHER LINKING
                TaggedPtr tagged2 = recycler_.protect_epoch_and_load(t,tail_);
                if(tagged2 != taggedTail){
                    privateQueuePutBack<false>(newTailIndex,t,false);
                    continue;
                }
                //open the queue
                (void) recycler_.decode(newTailIndex)->open();
            }

            //push the current item
            (void) recycler_.decode(newTailIndex)->enqueue(item);
            //pack the new queue in a Segment*
            Segment* newTail = ptrCast(getTagged(getNewVersion(),newTailIndex));
            Segment* nullNode = nullptr;
            Segment* currentTail = recycler_.decode(getIndex(taggedTail));

            //try to perform the link
            if(currentTail->next_.compare_exchange_strong(nullNode,newTail)) {
                (void) tail_.compare_exchange_strong(taggedTail,taggedCast(newTail));
                break;
            }

            //link was performed by someone else
            privateQueuePutBack<true>(newTailIndex,t,false);
        }

        //successful
        recycler_.clear_epoch(t);
        recordEnqueue(t);
        return true;
    }

    bool dequeue(T& item) override {
        Ticket t = get_ticket_();
        while(1) {
            TaggedPtr taggedHead = recycler_.protect_epoch_and_load(t,head_);
            Segment* head = recycler_.decode(getIndex(taggedHead));
            if(!head->dequeue(item)) {
                //check next
                Segment* next = head->getNext();
                if(next == nullptr) {
                    //empty queue
                    recycler_.clear_epoch(t);
                    return false;
                }

                if(!head->dequeue(item)) {
                    //try to cas the new next
                    if(head_.compare_exchange_strong(taggedHead,taggedCast(next))) {
                        //drop protection
                        recycler_.retire(getIndex(taggedHead),t);
                    }
                    continue;
                }
            }

            recycler_.clear_epoch(t);
            recordDequeue(t);
            return true;
        }
    }

    bool acquire() override {
        Ticket ignore;
        return ticketing_.acquire(ignore);
    }

    void release() override {
        ticketing_.release();
    }

    /**
     * @brief size method
     *
     * @note doesn't require a thread to have acquired an operational slot
     */
    size_t size() const override {
        int64_t total = 0;
        recycler_.metadataIter([&total](const std::atomic<int64_t>& m) {
            total += m.load(std::memory_order_relaxed);
        });
        assert(total >= 0 && "Negative size detected");
        return static_cast<size_t>(total);
    }


    size_t capacity() const override {
        return seg_capacity_ * ChunkFactor;
    }

    static std::string toString() {
        return std::string("BoundedMem") + Segment::toString();
    }


private:

    /**
     * @brief records an enqueue in the caller thread metadata section
     */
    inline void recordEnqueue(Ticket t) {
        recycler_.getMetadata(t).fetch_add(1,std::memory_order_relaxed);
    }

    /**
     * @brief records a dequeue in the caller thread metadata section
     */
    inline void recordDequeue(Ticket t) {
        recycler_.getMetadata(t).fetch_sub(1,std::memory_order_relaxed);
    }

    /**
     * @brief Safe enqueue operation
     *
     * @note calling plain `q->enqueue()` on a previously closed segment
     * can cause delays due to index disalignment. This method records the
     * last known Version (monotonic counter), and checks if the queue is closed
     * if the version matches.
     *
     * @debug: might have to rework this, if the version is set maybe there's no
     * need in checking the queue, ABA prevention counts for ~ 2 million iterations
     */
    inline bool safeEnqueue(TaggedPtr tail, T item) {
        static thread_local Version lastV = 0; //reserved for nullptr
        Segment * ptr = recycler_.decode(getIndex(tail));
        Version v = getVersion(tail);
        if((v == lastV) && ptr->isClosed()) {
            return false;
        } else if (ptr->enqueue(item)) {
            lastV = 0;
            return true;
        } else {
            lastV = v;
            return false;
        }
    }

    /**
     * @brief checks if a new next tail exists
     *
     * @warning supposes the caller is protecting the tail
     *
     * @return true if the tail was updated false otherwise
     */
    inline bool updateNextTail(TaggedPtr tail) {
        Segment* ptr    = recycler_.decode(getIndex(tail));
        Segment* next   = ptr->getNext();
        if(next == nullptr) return false;

        //try to cas the tail to the next tail
        (void)tail_.compare_exchange_strong(tail,taggedCast(next));
        return true;

    }

    /**
     * @brief clears a private segment and gives it back to the Recycler
     * @param Index q: Recycler::Index of the Segment
     * @param Ticket t: ticket of the thread calling the method
     * @param bool dropProtection: if true automatically drops the epoch protection
     *
     * @warning: if the segment was made public then undefined behaviour
     * @warning: thread must be protecting an epoch before calling this method
     */
    template<bool do_empty>
    inline void privateQueuePutBack(Index q,Ticket t,bool dropProtection = true) {
        Segment* queue = recycler_.decode(q);
        if constexpr (do_empty) {
            size_t success = 0;
            T ignore;
            while(queue->dequeue(ignore)) {
                success++;
            }
            assert(success == 1 && "Exactely one enqueue constraint failed");
        }
        if constexpr (useCache) {
            /**
             * if segment is being put in cache open it (Open is constant time)
             */
            (void) queue->open();
            assert(queue->isOpened() && "Putting in cache queue that wasn't opened");
            recycler_.put_cache(q);
        } else {
            recycler_.retire(q,t,dropProtection); //put it back in the free space
        }
    }

    inline Ticket get_ticket_() {
        Ticket t;
        bool ok = ticketing_.acquire(t);
        assert(ok && "No ticket could have bee acquired");
        return t;
    }

    // ========================
    // Casting & Tagged Methods
    // ========================

    inline TaggedPtr taggedCast(Segment* ptr) const {
        return reinterpret_cast<TaggedPtr>(ptr);
    }

    TaggedPtr getTagged(Version v, Index i) const {
        return bit::merge(v,i);
    }

    inline Segment* ptrCast(TaggedPtr ptr) const {
        return reinterpret_cast<Segment*>(ptr);
    }

    inline Index getIndex(TaggedPtr p) const {
        return bit::keep_low(p);
    }

    inline Version getVersion(TaggedPtr p) const {
        return bit::keep_high(p);
    }

    inline Version getNewVersion() {
        Version ret;
        do {
            ret = versionPool_.fetch_add(1, std::memory_order_acq_rel);
        } while(ret == 0);
        return ret;
    }

    ALIGNED_CACHE std::atomic<TaggedPtr> tail_{0};   //matches the nullptr value
    CACHE_PAD_TYPES(std::atomic<TaggedPtr>);
    ALIGNED_CACHE std::atomic<TaggedPtr> head_{0};   //matches the nullptr value
    CACHE_PAD_TYPES(std::atomic<TaggedPtr>);
    ALIGNED_CACHE std::atomic<Version> versionPool_{1};   //version reserved for nullptr
    CACHE_PAD_TYPES(std::atomic<Version>);
    ALIGNED_CACHE util::threading::DynamicThreadTicket ticketing_;
    size_t const seg_capacity_;
    Recycler recycler_;
};
