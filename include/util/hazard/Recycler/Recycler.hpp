#pragma once
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <utility>
#include <type_traits>
#include <cassert>

#include "Buckets.hpp"              //Cache and LimboBuffer Implementation
#include "PtrLookup.hpp"            //Immutable Lookup Table Implementation
#include "VersionedIndex.hpp"       //DynamicPackedU64 Impkementation
#include "OptionsPack.hpp"          //Template Options
#include "HazardCell.hpp"           //Padded SingleWriterLocation with general Metadata field
#include "DynamicThreadTicket.hpp"  //TLS Ticket Manager
#include "EpochCell.hpp"            //EpochCell SWL
#include "specs.hpp"                //Cache alignment and compatibility checks

namespace util::hazard::recycler {

/**
 * @brief Configuration options for the Recycler.
 */
struct RecyclerOpt {
    struct Pow2_Cache{};
    struct Disable_Cache_Padding{};
    struct Disable_Cache{};
};

/**
 * @brief A high-performance Epoch-Based Recycler (EBR).
 */
template<typename T, size_t Capacity, typename Opt = meta::EmptyOptions, typename Meta = void>
class Recycler {
public:
    using VersionedIndex = details::VersionedIndex<Capacity>;
    using Index          = typename VersionedIndex::Index;

private:
    // Configuration
    static constexpr bool POW2          = Opt::template has<RecyclerOpt::Pow2_Cache>;
    static constexpr bool NO_CACHE      = Opt::template has<RecyclerOpt::Disable_Cache>;

    // Internal Types
    using EpochCell      = details::EpochCell;
    using ThreadCell     = HazardCell<EpochCell, Meta>;
    using Epoch          = typename EpochCell::Epoch;
    using PtrLookupT     = details::ImmutablePtrLookup<T>;
    using Ticketing      = threading::DynamicThreadTicket;

    // FIX: Use LimboBuffer to ensure slots are physically cleared (exchange EMPTY_VAL) upon dequeue.
    // This prevents the "double reclaim" bug where old data is read after bucket rotation.
    using Bucket         = details::DebugBucket<Capacity>;
    // using Bucket = details::DebugBucket<Capacity,meta::EmptyOptions>;

    static_assert(std::is_same_v<Index, details::Value>, "Recycler: Index type mismatch with Bucket Value");

    // Cache Types
    using CacheOpt       = typename meta::EmptyOptions
                           ::template add_if<POW2, details::CacheOpt::Pow2Size>;
    using RealCache      = details::DebugBucket<Capacity>;

    struct DisabledCache {};
    using CacheMember    = std::conditional_t<NO_CACHE, DisabledCache, RealCache>;

public:
    enum class BucketState : uint64_t {
        Current = 0,
        Grace   = 3,
        Free    = 2,
        Next    = 1,
    };

    template<typename... Args>
    explicit Recycler(size_t maxThreads, Args&&... args) :
        epoch_{0},
        threadRecord_{new ThreadCell[maxThreads]},
        ticketing_{maxThreads},
        lookup_{Capacity, std::forward<Args>(args)...},
        cache_(),
        buckets_{new Bucket[4]}
    {
        // Initialize: Fill the 'initial' Free bucket (index 2 for epoch 0)
        if constexpr (NO_CACHE) {
            Bucket& initialFree = buckets_[2];
            for(size_t i = 0; i < Capacity; ++i) {
                initialFree.enqueue(Index{i});
            }
        } else {
            RealCache& initialFree = static_cast<RealCache&>(cache_);
            for(size_t i = 0; i < Capacity; ++i) {
                initialFree.enqueue(Index{i});
            }
        }
    }

    ~Recycler() {
        delete[] threadRecord_;
        delete[] buckets_;
    }

    // =========================================================================
    // Thread Management
    // =========================================================================

    template <typename M = Meta>
    typename std::enable_if_t<!std::is_void_v<M>, M&>
    getMetadata() {
        return threadRecord_[get_ticket()].metadata();
    }

    template<typename Func>
    void metadataIter(Func&& f) const {
        if constexpr (!std::is_void_v<Meta>) {
            for(size_t i = 0; i < ticketing_.max_threads(); ++i) {
                f(threadRecord_[i].metadata());
            }
        } else {
            assert(false && "Recycler: metadataIter called on void Metadata");
            std::abort();
        }
    }

    template<typename Func>
    void metadataInit(Func&& f) {
        if constexpr (!std::is_void_v<Meta>) {
            for(size_t i = 0; i < ticketing_.max_threads(); ++i) {
                f(threadRecord_[i].metadata());
            }
        } else {
            assert(false && "Recycler: metadataInit called on void Metadata");
            std::abort();
        }
    }

    [[nodiscard]] bool register_thread() noexcept {
        uint64_t t;
        return ticketing_.acquire(t);
    }

    void unregister_thread() {
        uint64_t t;
        if(ticketing_.acquire(t)) {
            threadRecord_[t].data().clear();
        }
        ticketing_.release();
    }

    // =========================================================================
    // Pointer Access
    // =========================================================================

    T* decode(Index idx) const noexcept {
        return lookup_[idx];
    }

    T* decode(VersionedIndex idx) const noexcept {
        return lookup_[idx.index()];
    }

    // =========================================================================
    // Epoch Protection
    // =========================================================================

    void protect_epoch() {
        uint64_t ticket = get_ticket();
        uint64_t current = epoch_.load(std::memory_order_acquire);
        threadRecord_[ticket].data().protect(current);
    }

    void clear_epoch() {
        uint64_t ticket = get_ticket();
        threadRecord_[ticket].data().clear();
    }

    template<typename AtomT>
    AtomT protect_epoch_and_load(std::atomic<AtomT>& atom) {
        uint64_t ticket = get_ticket();
        EpochCell& cell = threadRecord_[ticket].data();
        AtomT val;
        do {
            uint64_t current = epoch_.load(std::memory_order_acquire);
            cell.protect(current);
            val = atom.load(std::memory_order_acquire);
        } while(val == atom.load(std::memory_order_acquire));

        return val;
    }

    // =========================================================================
    // Cache Operations
    // =========================================================================

    bool get_cache(Index& out_idx) {
        if constexpr (NO_CACHE) {
            assert(false && "Recycler: get_cache called while cache disabled");
            return false;
        }
        else {
            return static_cast<RealCache&>(cache_).dequeue(out_idx);
        }
    }

    void put_cache(Index idx) {
        if constexpr (NO_CACHE) {
            assert(false && "Recycler: put_cache called while cache disabled");
        }
        else {
            static_cast<RealCache&>(cache_).enqueue(idx);
        }
    }

    // =========================================================================
    // Core Logic: Retire & Reclaim
    // =========================================================================

    void retire(Index idx) {
        uint64_t ticket = get_ticket();
        bool was_active;
        Epoch current_epoch;
        EpochCell& c = threadRecord_[ticket].data();

        // 1. Check if we are already protecting an epoch
        c.snapshot(was_active, current_epoch);

        if (!was_active) {
            // 2. Stabilize Protection
            current_epoch = epoch_.load(std::memory_order_acquire);
            c.protect(current_epoch);
            current_epoch = epoch_.load(std::memory_order_acquire);
        }

        // 3. Queue to the bucket of the epoch we are holding (Protected)
        // FIX: Use current_epoch, NOT epoch_.load(). Using epoch_.load()
        // introduces a race where we retire into a future epoch bucket
        // while protecting an old one.
        Bucket& target = get_bucket(current_epoch, BucketState::Current);
        target.enqueue(idx);

        // 4. Cleanup if we started protection
        if (!was_active) {
            c.clear();
        }
    }

    bool reclaim(Index& out_idx) {
        uint64_t ticket = get_ticket();
        bool was_active;
        Epoch e;
        EpochCell& c = threadRecord_[ticket].data();
        bool gotIdx = false;

        c.snapshot(was_active, e);

        // LIMIT: Full epoch rotation Valid epochs are Current - Grace - Free
        constexpr size_t MAX_ATTEMPTS = 3;

        for(size_t i = 0; i < MAX_ATTEMPTS; i++) {
            // 1. Stabilize Epoch and Protect
            // Protecting 'e' ensures we don't read from a bucket that unexpectedly
            // shifts from Free (Safe) to Current (Unsafe) while we are reading.
            if(!was_active) {
                e = epoch_.load(std::memory_order_acquire);
                c.protect(e);
                e = epoch_.load(std::memory_order_relaxed); //account for epoch shifting
            }

            // 2. Try Dequeue from Free Bucket
            Bucket& free_b = get_bucket(e, BucketState::Free);
            if (gotIdx = free_b.dequeue(out_idx); gotIdx) {
                break;
            }

            // 3. Try to Advance Epoch
            if(can_advance_epoch(e)) {
                //if epoch can be advanced then fix the state of the bucket
                // get_bucket(e,BucketState::Next).reset();
                Epoch dummy_e = e;
                bool ok = epoch_.compare_exchange_strong(
                    dummy_e,dummy_e + 1,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire
                );

                if(ok || dummy_e > e) {
                    continue;
                }

            }

            //check if the epoch advanced
            if (epoch_.load(std::memory_order_acquire) != e) {
                continue;
            } else {
                // Cannot advance (blocked by Grace threads) and Free is empty.
                // No point spinning further.
                break;
            }
        }

        if(!was_active)
            c.clear();
        return gotIdx;
    }

private:
    uint64_t get_ticket() {
        uint64_t t;
        bool ok = ticketing_.acquire(t);
        assert(ok && "Recycler: Thread limit reached");
        if(!ok) std::abort();
        return t;
    }

    Bucket& get_bucket(uint64_t epoch, BucketState state) {
        uint64_t offset = static_cast<uint64_t>(state);
        return buckets_[(epoch + offset) & 3];
    }

    bool can_advance_epoch(uint64_t expected_epoch) const {
        const size_t max_t = ticketing_.max_threads();
        bool active;
        uint64_t t_epoch;

        for (size_t i = 0; i < max_t; ++i) {
            threadRecord_[i].data().snapshot(active, t_epoch);
            // If any thread is stuck on the 'unsafe' epoch (Grace), we cannot advance.
            // Note: t_epoch == expected_epoch (Active on Current) is OK.
            if (active && t_epoch != expected_epoch ||
                epoch_.load(std::memory_order_relaxed) != expected_epoch) {
                return false;
            }
        }
        return true;
    }

    ALIGNED_CACHE std::atomic<uint64_t> epoch_;
    CACHE_PAD_TYPES(std::atomic_uint64_t);

    ThreadCell* threadRecord_;
    Ticketing   ticketing_;
    PtrLookupT  lookup_;

    ALIGNED_CACHE CacheMember cache_;
    ALIGNED_CACHE Bucket* buckets_;
};

} // namespace util::hazard::recycler
