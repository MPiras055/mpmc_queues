#pragma once
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <utility>
#include <type_traits>
#include <cassert>

#include "Buckets.hpp"
#include "PtrLookup.hpp"
#include "VersionedIndex.hpp"
#include "OptionsPack.hpp"
#include "HazardCell.hpp"
#include "DynamicThreadTicket.hpp"
#include "EpochCell.hpp"
#include "Recycler/VersionedIndex.hpp"
#include "specs.hpp"

namespace util::hazard::recycler {

struct RecyclerOpt {
    struct Pow2_Buckets{};
    struct Disable_Cache_Padding{};
    struct Disable_Cache{};
    struct AutoFix_Bucket{};
};

/**
 * @brief A high-performance Epoch-Based Recycler (EBR) with integrated thread management.
 * * Manages the lifecycle of memory locations using a global epoch and a 4-bucket rotation.
 * Integrates thread registration (DynamicThreadTicket) to simplify the API.
 */
template<typename T, size_t Capacity, typename Opt = meta::EmptyOptions, typename Meta = void>
class Recycler {
public:
    using VersionedIndex = details::VersionedIndex<Capacity>;
private:

    // =========================================================================
    // Configuration & Constants
    // =========================================================================
    static constexpr bool POW2 =         Opt::template has<RecyclerOpt::Pow2_Buckets>;
    static constexpr bool NO_CACHE =     Opt::template has<RecyclerOpt::Disable_Cache>;
    static constexpr bool AUTO_FIX =     Opt::template has<RecyclerOpt::AutoFix_Bucket>;
    static constexpr bool NO_CACHE_PAD = Opt::template has<RecyclerOpt::Disable_Cache_Padding>;

    // =========================================================================
    // Internal Types
    // =========================================================================
    using EpochCell      = details::EpochCell;
    using ThreadCell     = HazardCell<EpochCell, Meta>;
    using Epoch          = typename EpochCell::Epoch;
    using PtrLookupT     = details::ImmutablePtrLookup<T>;
    using Ticketing      = threading::DynamicThreadTicket;
    using Index          = VersionedIndex::Index;

    // Buckets (Phased MPMC)
    using BucketOpt      = typename meta::EmptyOptions
                           ::template add_if<AUTO_FIX, details::LimboBufferOpt::Auto_FixState>;
    using Bucket         = details::LimboBuffer<Capacity, BucketOpt>;

    static_assert(std::is_same_v<Index,details::Value>,"Recycler: Index type doesn't match Bucket Value");

    // Cache (General MPMC)
    using CacheOpt       = typename meta::EmptyOptions
                           ::template add_if<POW2, details::CacheOpt::Pow2Size>;
    using RealCache      = details::Cache<Capacity, CacheOpt>;

    // Conditional Cache
    struct DisabledCache { explicit DisabledCache(size_t) {} };
    using CacheMember = std::conditional_t<NO_CACHE, DisabledCache, RealCache>;

public:
    // =========================================================================
    // Public Types & Enums
    // =========================================================================

    enum class BucketState : uint64_t {
        Current = 0,    // Accumulation (E)
        Grace   = 1,    // Waiting (E-1)
        Free    = 2,    // Reclamation (E-2)
        Next    = 3,    // Future (E+1)
        States  = 4     // Sentinel Value
    };

    // =========================================================================
    // Constructor & Destructor
    // =========================================================================

    template<typename... Args>
    explicit Recycler(size_t maxThreads, Args&&... args) :
        epoch_{0},
        threadRecord_{new ThreadCell[maxThreads]},
        ticketing_{maxThreads},
        lookup_{Capacity, std::forward<Args>(args)...},
        cache_{Capacity},
        buckets_{new Bucket[BucketState::States]}
    {
        // Initialize: Items start in Free bucket (Epoch 0 -> Free is idx 2)
        Bucket& initialFree = get_bucket_by_state(0, BucketState::Free);

        for(size_t i = 0; i < Capacity; ++i) {
            if constexpr (!NO_CACHE) {
                static_cast<RealCache&>(cache_).enqueue(reinterpret_cast<Index*>(i+1));
            } else {
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

    bool register_thread() {
        uint64_t ticket;
        return ticketing_.acquire(ticket);
    }

    void unregister_thread() {
        clear();
        ticketing_.release();
    }

    Meta& get_metadata() {
        uint64_t ticket = get_ticket_assert();
        return threadRecord_[ticket].metadata();
    }

    template<typename Func>
    void iterate_metadata(Func&& f) const {
        for(size_t i = 0; i < ticketing_.max_threads(); ++i) {
            f(threadRecord_[i].metadata());
        }
    }

    // =========================================================================
    // Epoch Protection
    // =========================================================================

    void protect() {
        uint64_t ticket = get_ticket_assert();
        uint64_t current = epoch_.load(std::memory_order_acquire);
        threadRecord_[ticket].data().protect(current);
    }

    void clear() {
        uint64_t ticket = get_ticket_assert();
        threadRecord_[ticket].data().clear();
    }

    template<typename AtomT>
    AtomT protect_and_load(std::atomic<AtomT>& atom) {
        AtomT val;
        do {
            protect();
            val = atom.load(std::memory_order_acquire);
        } while (val != atom.load(std::memory_order_relaxed));
        return val;
    }

    // =========================================================================
    // Pointer / Object Access
    // =========================================================================

    T* get(VersionedIndex idx) const noexcept {
        return lookup_[idx.index()];
    }

    // =========================================================================
    // Cache Operations
    // =========================================================================

    bool get_cache(VersionedIndex& out_idx) {
        if constexpr (NO_CACHE) return false;
        else {
            RealCache& c = static_cast<RealCache&>(cache_);
            Index i;
            if(c.dequeue(i)) {
                out_idx.setIndex(i);
                return true;
            } else {
                return false;
            }
        }
    }

    void put_cache(VersionedIndex idx) {
        if constexpr (NO_CACHE) {
            assert(false && "Recycler: Cache disabled, use retire()");
        } else {
            static_cast<RealCache&>(cache_).enqueue(idx.index());
        }
    }

    // =========================================================================
    // Reclamation Operations
    // =========================================================================

    void retire(VersionedIndex v) {
        uint64_t current_epoch = epoch_.load(std::memory_order_acquire);
        uint64_t ticket = get_ticket_assert();
        assert(threadRecord_[ticket].data().isActive() && "Recycler: retire call needs epoch protection");

        Bucket& current = get_bucket_by_state(current_epoch, BucketState::Current);

        current.enqueue(v.index());
    }

    bool reclaim(VersionedIndex& out_idx) {
        uint64_t ticket = get_ticket_assert();
        assert(!threadRecord_[ticket].data().isActive() && "Recycler: retire call need clear epoch");

        EpochCell& cell = threadRecord_[ticket].data();

        constexpr int MAX_EPOCH_ADVANCE_ATTEMPTS = 2;
        Index idx;

        for (int i = 0; i <= MAX_EPOCH_ADVANCE_ATTEMPTS; ++i) {
            // Load snapshot of epoch at start of loop iteration
            Epoch e =

            // 1. Try scanning Free bucket for THIS snapshot
            Bucket& free_b = get_bucket_by_state(e, BucketState::Free);

            if (free_b.dequeue(idx)) {
                out_idx = out_idx.setIndex(idx);
                clear();
                return true;
            }

            // 2. If Free bucket empty, try to advance epoch
            if (i < MAX_EPOCH_ADVANCE_ATTEMPTS) {
                // try_advance_epoch checks if 'e' is still current AND if advancement is safe.
                // It internally performs a CAS on 'e'.
                if (!try_advance_epoch(e)) {
                    uint64_t fresh_e = epoch_.load(std::memory_order_relaxed);
                    if (fresh_e != e) {
                        // Someone else advanced! Restart with fresh_e.
                        continue;
                    } else {
                        // Blocked by straggler.
                        return false;
                    }
                }
                // Success! We advanced E -> E+1. Loop continues to check NEW free bucket.
            }
        }
        return false;
    }

private:
    // =========================================================================
    // Helpers
    // =========================================================================

    uint64_t get_ticket_assert() {
        uint64_t t;
        bool ok = ticketing_.acquire(t);
        assert(ok && "Thread not registered with Recycler");
        return t;
    }

    Bucket& get_bucket_by_state(uint64_t epoch, BucketState state) {
        static constexpr uint64_t OFFSETS[] = { 0, 3, 2, 1 }; // Current, Grace, Free, Next
        uint64_t offset = OFFSETS[static_cast<int>(state)];
        return buckets_[(epoch + offset) & 3];
    }

    /**
     * @brief Tries to advance global epoch from expected_val to expected_val + 1.
     * @return true if WE successfully advanced the epoch.
     */
    bool try_advance_epoch(uint64_t expected_val) {
        uint64_t ticket = get_ticket_assert();
        // 1. Scan threads to verify safety
        for (size_t i = 0; i < ticketing_.max_threads(); ++i) {
            bool active;
            uint64_t t_epoch;
            threadRecord_[i].data().snapshot(active, t_epoch);

            // Check against stale snapshot
            if (epoch_.load(std::memory_order_relaxed) != expected_val) {
                return false;
            }

            // Safety Check: We cannot move Global E -> E+1 if any thread is stuck on E-1.
            // Because moving to E+1 turns E-1 (Grace) into Free.
            // If a thread is accessing E-1, we must not free it.
            // Note: unsigned subtraction handles wrap around correctly.
            if ((i != ticket) && (active && t_epoch == (expected_val - 1))) {
                return false;
            }
        }

        // 2. CAS Epoch
        if (epoch_.compare_exchange_strong(expected_val, expected_val + 1, std::memory_order_acq_rel)) {
            // Reset the bucket that just became 'Next' (was Free/E-2).
            // (expected_val + 2) corresponds to the OLD Free bucket.
            buckets_[(expected_val + 2) & 3].reset();
            return true;
        }

        return false;
    }

    // =========================================================================
    // Data Members
    // =========================================================================

    ALIGNED_CACHE std::atomic<uint64_t> epoch_;
    CACHE_PAD_TYPES(std::atomic_uint64_t);

#ifdef CACHE_PAD_TYPES
    CACHE_PAD_TYPES(std::atomic_uint64_t, std::atomic_uint32_t);
#endif

    ThreadCell* threadRecord_;
    Ticketing   ticketing_;
    PtrLookupT  lookup_;
    ALIGNED_CACHE CacheMember cache_;
    ALIGNED_CACHE Bucket* buckets_;
};

} // namespace util::hazard::recycler
