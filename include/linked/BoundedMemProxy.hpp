#pragma once
#include <OptionsPack.hpp>
#include <IProxy.hpp>               //proxy interface
#include <DynamicThreadTicket.hpp>  //cached thread tickets
#include <VersionedIndex.hpp>
#include <Recycler.hpp>             //memory recycler
#include <atomic>
#include <cstdint>
#include <specs.hpp>                //padding definition
#include <bit.hpp>                  //bitwise manipulation

struct BoundedMemProxyOpt {
    template<size_t N> struct ChunkFactor{};
    struct DisableCache{};
};


template <
    typename T,
    template<typename,typename,typename,typename> typename Seg,
    typename ProxyOpt = meta::EmptyOptions,
    typename SegmentOpt = meta::EmptyOptions
> class BoundedMemProxy: public base::IProxy<T,Seg<T,uint64_t,SegmentOpt,ProxyOpt>> {

    static constexpr size_t ChunkFactor = ProxyOpt::template get<BoundedMemProxyOpt::ChunkFactor,4>;
    static constexpr bool NoCache       = !ProxyOpt::template has<BoundedMemProxyOpt::DisableCache>;

    using   RawVersionedIndex = util::hazard::recycler::details::RawVersionedIndex;
    using   VersionedIndex    = util::hazard::recycler::details::VersionedIndex<ChunkFactor>;
    using   Index             = VersionedIndex::Index;
    using   Version           = VersionedIndex::Version;

    using Segment = Seg<T,BoundedMemProxy,VersionedIndex,SegmentOpt>;
    using RecyclerOptions = std::conditional_t<
        NoCache,
        meta::OptionsPack<util::hazard::recycler::RecyclerOpt::Disable_Cache>,
        meta::EmptyOptions
    >;

    struct ThreadMetadata { //whole struct gets automatically aligned and padded
        std::atomic_int64_t OpCounter{0};
        RawVersionedIndex lastSeen{RawVersionedIndex{}};
    };

    //default versioned index is 0, so if we use all odd versions to encapsulate nodes we'll be alright
    static constexpr VersionedIndex NULL_NODE = VersionedIndex{};
    static constexpr bool INFO_REQUIRED = Segment::info_required;
    using Recycler = util::hazard::recycler::Recycler<Segment,ChunkFactor,RecyclerOptions,ThreadMetadata>;

    using Ticket = util::threading::DynamicThreadTicket::Ticket;

    static Version nextVersion(Version i) {
        constexpr Version MAX_VER = (1 << VersionedIndex::VERSION_BITS) - 1;
        static_assert(MAX_VER > 2, "MaxVersion too low");
        return (i % MAX_VER) + 1;
    }

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
    static constexpr size_t Segments = ChunkFactor;
    explicit BoundedMemProxy(size_t capacity, size_t maxThreads):
            seg_capacity_(capacity / ChunkFactor),
            recycler_(ChunkFactor,maxThreads,seg_capacity_) {

        assert((capacity % ChunkFactor) == 0 && "Queue capacity has to be a multiple of chunk factor (Chunkfactor Default = 4)");
        assert(seg_capacity_ != 0 && "Segment Capacity underflow detected");
        assert(maxThreads != 0 && "The managed number of threads must be non null");

        size_t SentinelIndex;
        bool ok = recycler_.reclaim(SentinelIndex,0); //we can use any ticket up to maxThreads since all others are inactive

        assert(ok && "No Sentinel segment could have been got");
        recycler_.decode(SentinelIndex)->next.store(NULL_NODE,std::memory_order_release);
        VersionedIndex sentinel = VersionedIndex(nextVersion(0),SentinelIndex);

        head_.store(sentinel,std::memory_order_relaxed);
        tail_.store(sentinel,std::memory_order_relaxed);

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

    bool enqueue(T item) noexcept final override {
        bool failedReclamation = false;
        VersionedIndex lastSeen{};
        recycler_.protect_epoch();
        VersionedIndex tail = tail_.load(std::memory_order_acquire);

        while(1) {
            //Check that tail hasn't changed
            VersionedIndex tail2 = tail_.load(std::memory_order_acquire);
            if(tail != tail2) {
                recycler_.protect_epoch();
                tail = tail_.load(std::memory_order_acquire);
                failedReclamation = false;
                continue;
            }

            VersionedIndex next    = recycler_.decode(tail.index())->getNext();
            if(next != NULL_NODE) {
                recycler_.protect_epoch();  //protect current epoch
                (void)tail_.compare_exchange_strong(tail,next);
                failedReclamation = false;
                continue;
            }


            if(failedReclamation && (lastSeen == tail)) {
                recycler_.clear_epoch();
                return false;
            } else failedReclamation = false;

            // =====================
            //  Enqueue on segment
            // =====================

            if(safeEnqueue(tail,item)) {
                break;
            }

            // =====================
            // New segment link
            // =====================

            Index newIndex;

            if(!recycler_.get_from_cache(newIndex)) {
                if(!recycler_.reclaim(newIndex)) {
                    failedReclamation = true;
                    lastSeen = tail;
                    continue;
                }
            }

            //reinitialize the segment
            Segment* s = recycler_.decode(newIndex);
            bool okOpen = s->open();
            assert(okOpen && "Could not reopen segment");
            s->enqueue(item);

            //try to link
            VersionedIndex null = NULL_NODE;
            VersionedIndex newTail = VersionedIndex(nextVersion(tail.version()),newIndex);
            Segment* t = recycler_.decode(tail);

            //link successful
            if(t->next_.compare_exchange_strong(null,newTail)) {
                //try to update the global tail
                (void) tail_.compare_exchange_strong(tail,newTail);
                break;

            } else {
                T dummy;
                (void)s->dequeue(dummy);
                //protect the new tail
                if constexpr (NoCache) recycler_.retire(newTail);
                else recycler_.put_in_cache(newTail);
                recycler_.clear_epoch();
                tail = ChunkFactor; //dummy tail that will be setted next round
            }
        }

        //successful
        recycler_.clear_epoch();
        recordEnqueue();
        return true;
    }

    bool dequeue(T& item) noexcept final override {
        while(1) {
            VersionedIndex taggedHead = recycler_.protect_epoch_and_load(head_);
            Segment* head = recycler_.decode(taggedHead.index());
            if(!head->dequeue(item)) {
                //check next
                VersionedIndex next = head->getNext();
                if(next == NULL_NODE) {
                    //empty queue
                    recycler_.clear_epoch();
                    return false;
                }
                if(!dequeueAfterNextLinked(head,item)) {
                    //try to cas the new next
                    if(head_.compare_exchange_strong(taggedHead,next)) {
                        recycler_.retire(taggedHead.index());
                    }
                }
            }

            recycler_.clear_epoch();
            recordDequeue();
            return true;
        }
    }

    inline bool acquire() noexcept final override {
        return recycler_.register_thread();
    }

    inline void release() noexcept final override {
        recycler_.unregister_thread();
    }

    /**
     * @brief size method
     *
     * @note doesn't require a thread to have acquired an operational slot
     */
    size_t size() const noexcept final override {
        int64_t total = 0;
        recycler_.metadataIter([&total](const ThreadMetadata& m) {
            total += m.OpCounter.load(std::memory_order_relaxed);
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
    inline void recordEnqueue() {
        recycler_.getMetadata().OpCounter.fetch_add(1,std::memory_order_relaxed);
    }

    /**
     * @brief records a dequeue in the caller thread metadata section
     */
    inline void recordDequeue(Ticket t) {
        recycler_.getMetadata().OpCounter.fetch_sub(1,std::memory_order_relaxed);
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
    inline bool safeEnqueue(VersionedIndex tail, T item) {
        Segment* s = recycler_.decode(tail.index());
        if constexpr (INFO_REQUIRED) {
            VersionedIndex& lastSeen = recycler_.getMetadata().lastSeen;
            bool info = tail == lastSeen;
            bool enq_ok = s->enqueue(item,info);
            lastSeen = enq_ok? NULL_NODE : tail;
            return enq_ok;
        } else {
            return s->enqueue(item);
        }
    }

    size_t const seg_capacity_;
    ALIGNED_CACHE std::atomic<VersionedIndex> tail_{NULL_NODE};   //matches the nullptr value
    CACHE_PAD_TYPES(std::atomic<VersionedIndex>);
    ALIGNED_CACHE std::atomic<VersionedIndex> head_{NULL_NODE};   //matches the nullptr value
    CACHE_PAD_TYPES(std::atomic<VersionedIndex>);
    ALIGNED_CACHE Recycler recycler_;
    CACHE_PAD_TYPES(Recycler);
};
