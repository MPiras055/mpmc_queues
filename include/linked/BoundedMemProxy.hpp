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
    size_t ChunkFactor = 4,     //recycler's pool size
    bool Pow2 = true,           //segment pow2
    bool RecyclerPow2 = true,   //enable pow2 optimization for recycler queues
    bool useCache = true        //enable recycler's cache
> class BoundedMemProxy: public base::IProxy<T, SegmentType> {

    using Segment = SegmentType<T,BoundedMemProxy,Pow2,true>;
    //for this design we NECESSARILY need the recycler to use the cache
    using Recycler = util::hazard::Recycler<Segment,std::atomic<int64_t>,true,RecyclerPow2>;
    using VersionedIndex = uint64_t;
    using Index = Recycler::Tml;
    using Version = uint32_t;
    using Ticket = uint64_t;
    using TaggedPtr = uint64_t;
    static constexpr uint32_t INC_VERSION = 2;

    static_assert(
                ((sizeof(Version) + sizeof(Index)) <= sizeof(VersionedIndex)) &&
                (sizeof(VersionedIndex) <= sizeof(Segment*)),
                    "Version and index cannot fit in a single memory word"
    );

public:

    explicit BoundedMemProxy(size_t capacity, size_t maxThreads):
            ticketing_(maxThreads),
            seg_capacity_((Pow2? bit::next_pow2(capacity) : capacity) / ChunkFactor),
            recycler_(ChunkFactor,maxThreads,seg_capacity_) {

        assert(seg_capacity_ != 0 && "Segment Capacity is null (if Pow2 abilitated check division underflow)");
        assert(maxThreads != 0 && "The managed number of threads must be non null");

        if(!useCache) {
            std::abort();
        }

        Index SentinelIndex;
        if(!recycler_.get_cache(SentinelIndex)) {
            assert(false && "Cannot get segment from cache");
        }
        Version v = versionPool_.fetch_add(INC_VERSION);
        TaggedPtr Sentinel = getTagged(v, SentinelIndex);
        head_.store(Sentinel);
        tail_.store(Sentinel);

        for(size_t i = 0; i < maxThreads; i++) {
            recycler_.getMetadata(i).store(0,std::memory_order_relaxed);
        }

        //check if next pointer is setted correctly
        assert(
            recycler_.decode(SentinelIndex)->getNext() == nullptr &&
            "Constructor: next pointer not correct"
        );

    }

    bool enqueue(T item) override {
        Ticket t = get_ticket_();
        bool failedReclaim{false};

        while(1) {
            TaggedPtr taggedTail = recycler_.protect_epoch_and_load(t,tail_);

            if(updateNextTail(taggedTail)){
                continue;
            }

            if(safeEnqueue(taggedTail,item)) {
                recycler_.clear_epoch(t);
                recycler_.getMetadata(t).fetch_add(1,std::memory_order_relaxed);
                return true;
            }

            //failed we have to check and reclaim
            Index newQueueIndex = 0;
            if(recycler_.get_cache(newQueueIndex)) {
                //reset the reclaim flag
                failedReclaim = false;
                //enqueue your item
                Segment *newQueue = recycler_.decode(newQueueIndex);
                assert(newQueue->isOpened() && "Private queue from cache wasn't opened");
                T ignore;
                assert(newQueue->dequeue(ignore) == false && "Not expected dequeue from private segment");
                (void)newQueue->enqueue(item);
                Version v = versionPool_.fetch_add(INC_VERSION);
                newQueue = ptrCast(getTagged(v,newQueueIndex));
                Segment *nullNode = nullptr;
                Segment *currentTail = recycler_.decode(getIndex(taggedTail));
                //try to link
                if(currentTail->next_.compare_exchange_strong(nullNode,newQueue)) {
                    //link successful
                    (void) tail_.compare_exchange_strong(taggedTail,taggedCast(newQueue));
                    recycler_.clear_epoch(t);
                    recycler_.getMetadata(t).fetch_add(1,std::memory_order_relaxed);
                    return true;
                }  else {
                    //dequeue your item and place
                    T ignore;
                    size_t success = 0;
                    while(recycler_.decode(newQueueIndex)->dequeue(ignore)) {
                        success++;
                    }
                    assert(success == 1 && "More enqueues from private segment than expected");
                    recycler_.put_cache(newQueueIndex);
                }
            } else {
                //try to reclaim
                recycler_.clear_epoch(t);
                if(recycler_.reclaim(newQueueIndex,t)) {
                    //clear this queue and place it in cache
                    Segment* reclaimedQueue = recycler_.decode(newQueueIndex);
                    assert(reclaimedQueue->isClosed() && "Reclaimed queue wasn't closed");
                    //open the previously cloead queue
                    (void) reclaimedQueue->open();
                    recycler_.put_cache(newQueueIndex);
                } else if(failedReclaim) {
                    return false;
                } else {
                    failedReclaim = true;
                }
            }
        }
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
                        recycler_.retire(getIndex(taggedHead),t,true);
                    }
                    continue;
                }
            }

            recycler_.clear_epoch(t);
            recycler_.getMetadata(t).fetch_sub(1,std::memory_order_relaxed);
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

    size_t size() override {
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


private:

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
     * @return true if the tail was updated false otherwise
     */
    inline bool updateNextTail(TaggedPtr tail) {
        Segment* ptr    = recycler_.decode(getIndex(tail));
        Segment* next   = ptr->getNext();
        if(next == nullptr) {
            return false;
        }
        //try to cas the tail to the next tail
        (void)tail_.compare_exchange_strong(tail,taggedCast(next));
        return true;

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

    alignas(CACHE_LINE) std::atomic<TaggedPtr> tail_{0};   //matches the nullptr value
    char pad_tail_[CACHE_LINE - sizeof(std::atomic<TaggedPtr>)];
    alignas(CACHE_LINE) std::atomic<TaggedPtr> head_{0};   //matches the nullptr value
    char pad_head_[CACHE_LINE - sizeof(std::atomic<TaggedPtr>)];
    alignas(CACHE_LINE) std::atomic<Version> versionPool_{1};   //all versions must be odd (version 0 is reserved for nullptr)
    char pad_version_[CACHE_LINE - sizeof(std::atomic<Version>)];
    util::threading::DynamicThreadTicket ticketing_;
    size_t const seg_capacity_;
    Recycler recycler_;
};
