#pragma once
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <utility>
#include <type_traits>
#include <cassert>

#include <LFring.hpp>               //For bucket and cache impl
#include "PtrLookup.hpp"            //Immutable Lookup Table Implementation
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
    struct Disable_Cache{};
};

/**
 * @brief A high-performance Epoch-Based Recycler (EBR).
 */
template<typename T, size_t Capacity, typename Opt = meta::EmptyOptions, typename Meta = void>
class Recycler {
    // Configuration
    static constexpr bool NO_CACHE      = Opt::template has<RecyclerOpt::Disable_Cache>;

    // Internal Types
    using EpochCell      = details::EpochCell;
    using Epoch          = EpochCell::Epoch;
    using ThreadCell     = HazardCell<EpochCell, Meta>;
    using PtrLookupT     = details::ImmutablePtrLookup<T>;
    using Ticketing      = threading::DynamicThreadTicket;

    using Bucket        = queue::LFring<size_t>;

    using RealCache      = queue::LFring<size_t>;

    struct DisabledCache {
        DisabledCache() = default;
    };
    using CacheMember    = Bucket;

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
        buckets_{queue::LFringSlab<size_t>(NO_CACHE? 4 : 5,Capacity)},
        cache_{*buckets_.get(NO_CACHE? 0 : 4)}
    {
        // Initialize: Fill the 'initial' Free bucket (index 2 for epoch 0)

        Bucket& initialFree = get_bucket(epoch_.load(std::memory_order_relaxed),BucketState::Free);
        for(size_t i = 0; i < Capacity; ++i) {
            initialFree.enqueue(i);
        }

    }

    ~Recycler() {
        delete[] threadRecord_;
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
        if(ticketing_.has_ticket()) {
            uint64_t t;
            if(ticketing_.acquire(t)) {
                threadRecord_[t].data().clear();
            }
            ticketing_.release();
        }
    }

    // =========================================================================
    // Pointer Access
    // =========================================================================

    T* decode(size_t idx) const noexcept {
        return lookup_[idx];
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
        } while(val != atom.load(std::memory_order_acquire));

        return val;
    }

    // =========================================================================
    // Cache Operations
    // =========================================================================

    bool get_from_cache(size_t& out_idx) {
        if constexpr (NO_CACHE) {
            return false;
        } else {
            return static_cast<RealCache&>(cache_).dequeue(out_idx);
        }
    }

    void put_in_cache(size_t idx) {
        if constexpr (NO_CACHE) {
            assert(false && "Recycler: put_cache called while cache disabled");
        } else {
            static_cast<RealCache&>(cache_).enqueue(idx);
        }
    }

    // =========================================================================
    // Core Logic: Retire & Reclaim
    // =========================================================================

    void retire(size_t idx) {
        uint64_t ticket = get_ticket();
        bool was_active;
        Epoch current_epoch;
        EpochCell& c = threadRecord_[ticket].data();

        // 1. Check if we are already protecting an epoch
        c.snapshot(was_active, current_epoch);

        if (!was_active) {
            // Protect the current epoch
            current_epoch = epoch_.load(std::memory_order_acquire);
            c.protect(current_epoch);
            // at this point epoch can have shifted
            // so the Index is placed in the Grace bucket
            // Epoch can not advance more than once since protection
        }

        Bucket& target = get_bucket(current_epoch, BucketState::Grace);
        target.enqueue(idx);

        // cleanup if we protected
        if (!was_active) {
            c.clear();
        }
    }

    bool reclaim(size_t& out_idx) {
        uint64_t ticket = get_ticket();
        bool was_active;
        Epoch e;
        EpochCell& c = threadRecord_[ticket].data();
        bool gotIdx = false;

        c.snapshot(was_active, e);

        // LIMIT: Full epoch rotation Valid epochs are Current - Grace - Free
        constexpr size_t MAX_ATTEMPTS = 3;

        for(size_t i = 0; i < MAX_ATTEMPTS; i++) {

            //if thread wasn't active protect the current epoch
            if(!was_active) {
                e = epoch_.load(std::memory_order_acquire);
                c.protect(e);
                // by now epoch can have shifted to the Grace bucket
                //
            }

            // Try Dequeue from Free Bucket
            Bucket& free_b = get_bucket(e, BucketState::Free);
            if (gotIdx = free_b.dequeue(out_idx); gotIdx) {
                break;
            }

            // 3. Try to Advance Epoch
            if(can_advance_epoch(e)) {
                //if epoch can be advanced then fix the state of the bucket
                // get_bucket(e,BucketState::Next).reset();
                Epoch dummy_e = e;
                (void)epoch_.compare_exchange_strong(
                    dummy_e,dummy_e + 1,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire
                );
            }

            if(epoch_.load(std::memory_order_acquire) == e) {
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
        return *buckets_.get((epoch + offset) & 3);
    }

    bool can_advance_epoch(uint64_t expected_epoch) const {
        const size_t max_t = ticketing_.max_threads();
        bool active;
        uint64_t t_epoch;

        for (size_t i = 0; i < max_t; ++i) {
            threadRecord_[i].data().snapshot(active, t_epoch);
            // If any thread is stuck on the 'unsafe' epoch (Grace), we cannot advance.
            // Note: t_epoch == expected_epoch (Active on Current) is OK.
            if ((active && t_epoch != expected_epoch) ||
                epoch_.load(std::memory_order_relaxed) != expected_epoch) {
                return false;
            }
        }
        return true;
    }

    ThreadCell* threadRecord_;
    Ticketing   ticketing_;
    PtrLookupT  lookup_;

    ALIGNED_CACHE std::atomic<uint64_t> epoch_;
    CACHE_PAD_TYPES(std::atomic_uint64_t);
    queue::LFringSlab<size_t> buckets_;
    CacheMember& cache_;
};

} // namespace util::hazard::recycler
