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
 * * These tags can be passed via the `Opt` template parameter (using `meta::OptionsPack`)
 * to customize the internal behavior and performance characteristics of the Recycler.
 */
struct RecyclerOpt {
    /// @brief Use power-of-2 sized buckets for performance optimization (requires Capacity to be pow2).
    struct Pow2_Buckets{};
    /// @brief Disable padding in cache structures to save memory.
    struct Disable_Cache_Padding{};
    /// @brief Completely disable the hot-path thread-local/global cache.
    struct Disable_Cache{};
    /// @brief Enable auto-fixing state in the underlying LimboBuffer (internal bucket detail).
    struct AutoFixLimbo{};
};

/**
 * @brief A high-performance Epoch-Based Recycler (EBR).
 *
 * This class manages the safe reclamation of memory indices in concurrent data structures.
 * It employs a Global Epoch mechanism with a 4-bucket rotation strategy to ensure
 * objects are only reused when no threads can possibly hold a reference to them.
 *
 * The rotation cycle relative to Global Epoch E is:
 * - **Current (E)**:   Active threads retire deleted items here.
 * - **Next (E+1)**:    Empty, waiting to become Current.
 * - **Free (E-2)**:    Safe to reclaim items from here.
 * - **Grace (E-1)**:   Items waiting for straggling threads (stuck on E-1) to finish.
 *
 * It integrates `DynamicThreadTicket` for automatic thread registration and handling.
 *
 * @tparam T        The type of objects being managed (value type, not pointer).
 * @tparam Capacity The maximum number of objects managed.
 * @tparam Opt      Configuration options (see `RecyclerOpt`).
 * @tparam Meta     Optional per-thread metadata type (default void).
 */
template<typename T, size_t Capacity, typename Opt = meta::EmptyOptions, typename Meta = void>
class Recycler {
public:
    using VersionedIndex = details::VersionedIndex<Capacity>;
    using Index          = typename VersionedIndex::Index;

private:
    // Configuration
    static constexpr bool POW2          = Opt::template has<RecyclerOpt::Pow2_Buckets>;
    static constexpr bool NO_CACHE      = Opt::template has<RecyclerOpt::Disable_Cache>;
    static constexpr bool AUTOFIX_LIMBO = Opt::template has<RecyclerOpt::AutoFixLimbo>;

    // Internal Types
    using EpochCell      = details::EpochCell;
    using ThreadCell     = HazardCell<EpochCell, Meta>;
    using Epoch          = typename EpochCell::Epoch;
    using PtrLookupT     = details::ImmutablePtrLookup<T>;
    using Ticketing      = threading::DynamicThreadTicket;

    using Bucket         = details::LimboBuffer<Capacity,
        std::conditional_t<
            AUTOFIX_LIMBO,
            details::LimboBufferOpt::Auto_FixState,
            meta::EmptyOptions
        >>;

    static_assert(std::is_same_v<Index, details::Value>, "Recycler: Index type mismatch with Bucket Value");

    // Cache Types
    using CacheOpt       = typename meta::EmptyOptions
                           ::template add_if<POW2, details::CacheOpt::Pow2Size>;
    using RealCache      = details::Cache<Capacity, CacheOpt>;

    struct DisabledCache { explicit DisabledCache(size_t) {} };
    using CacheMember    = std::conditional_t<NO_CACHE, DisabledCache, RealCache>;

public:
    /**
     * @brief Enum representing the logical state of a bucket relative to the current epoch.
     * The bucket index is calculated as: `(Epoch + State) % 4`.
     */
    enum class BucketState : uint64_t {
        Current = 0, ///< The bucket currently receiving retired items (Epoch E).
        Grace   = 3, ///< The bucket waiting for readers to drain (Epoch E-1).
        Free    = 2, ///< The bucket containing items safe to reclaim (Epoch E-2).
        Next    = 1, ///< The empty bucket waiting to become Current (Epoch E+1).
    };

    /**
     * @brief Constructs the Recycler.
     *
     * Initializes the lookup table, cache, and buckets.
     * Pre-fills the initial 'Free' bucket (Index 2 at Epoch 0) so allocation can start immediately.
     *
     * @param maxThreads The maximum number of concurrent threads supported.
     * @param args       Arguments forwarded to the constructor of the objects in the lookup table.
     */
    template<typename... Args>
    explicit Recycler(size_t maxThreads, Args&&... args) :
        epoch_{0},
        threadRecord_{new ThreadCell[maxThreads]},
        ticketing_{maxThreads},
        lookup_{Capacity, std::forward<Args>(args)...},
        cache_{Capacity},
        buckets_{new Bucket[4]}
    {
        // Initialize: Fill the 'initial' Free bucket (index 2 for epoch 0)
        Bucket& initialFree = buckets_[2];
        for(size_t i = 0; i < Capacity; ++i) {
            initialFree.enqueue(Index{i});
        }
    }

    /**
     * @brief Destructor. Cleans up allocated resources.
     */
    ~Recycler() {
        delete[] threadRecord_;
        delete[] buckets_;
    }

    // =========================================================================
    // Thread Management
    // =========================================================================

    /**
     * @brief Accesses the metadata associated with the calling thread.
     *
     * This method is only available if the `Meta` template parameter is not `void`.
     * It automatically acquires a ticket for the current thread if one is not held.
     *
     * @tparam M Internal SFINAE helper (default Meta).
     * @return Reference to the thread's metadata.
     */
    template <typename M = Meta>
    typename std::enable_if_t<!std::is_void_v<M>, M&>
    getMetadata() {
        return threadRecord_[get_ticket()].metadata();
    }

    /**
     * @brief Iterates over the metadata of all registered threads.
     *
     * Useful for aggregating statistics or inspecting global state.
     * If Meta is void, this method asserts false (runtime check).
     *
     * @tparam Func Type of the callback function.
     * @param f A callable accepting `const Meta&`.
     */
    template<typename Func>
    void metadataIter(Func&& f) const {
        if constexpr (!std::is_void_v<Meta>) {
            for(size_t i = 0; i < ticketing_.max_threads(); ++i) {
                f(threadRecord_[i].metadata());
            }
        } else {
            assert(false && "Recycler: metadataIter called on void Metadata");
        }
    }

    /**
     * @brief Iterates over metadata to initialize or modify it.
     *
     * Similar to `metadataIter` but passes non-const references.
     *
     * @tparam Func Type of the callback function.
     * @param f A callable accepting `Meta&`.
     */
    template<typename Func>
    void metadataInit(Func&& f) {
        if constexpr (!std::is_void_v<Meta>) {
            for(size_t i = 0; i < ticketing_.max_threads(); ++i) {
                f(threadRecord_[i].metadata());
            }
        } else {
            assert(false && "Recycler: metadataInit called on void Metadata");
        }
    }

    // =========================================================================
    // Pointer Access
    // =========================================================================

    /**
     * @brief Decodes an Index into a raw pointer to the managed object.
     *
     * @param idx The index to decode.
     * @return Pointer to the object at that index.
     */
    T* decode(Index idx) const noexcept {
        return lookup_[idx];
    }

    /**
     * @brief Decodes a VersionedIndex into a raw pointer.
     *
     * @param idx The versioned index to decode.
     * @return Pointer to the object at the index portion of the VersionedIndex.
     */
    T* decode(VersionedIndex idx) const noexcept {
        return lookup_[idx.index()];
    }

    // =========================================================================
    // Epoch Protection
    // =========================================================================

    /**
     * @brief Marks the calling thread as active and protecting the current global epoch.
     *
     * This prevents the global epoch from advancing past `Current + 1`, effectively
     * preventing the reclamation of items retired in `Current` (which become `Grace` in the next epoch).
     */
    void protect_epoch() {
        uint64_t ticket = get_ticket();
        uint64_t current = epoch_.load(std::memory_order_acquire);
        threadRecord_[ticket].data().protect(current);
    }

    /**
     * @brief Marks the calling thread as inactive.
     *
     * The thread no longer prevents epoch advancement.
     */
    void clear_epoch() {
        uint64_t ticket = get_ticket();
        threadRecord_[ticket].data().clear();
    }

    /**
     * @brief Protects the current epoch and performs an atomic load.
     *
     * Retries the protection and load until the value loaded is consistent
     * with the epoch protected. This prevents reading a value that was reclaimed
     * and reused while the load was happening (ABA protection via Epoch).
     *
     * @tparam AtomT Type of the atomic variable.
     * @param atom   Reference to the atomic variable to load.
     * @return The loaded value.
     */
    template<typename AtomT>
    AtomT protect_epoch_and_load(std::atomic<AtomT>& atom) {
        uint64_t ticket = get_ticket();
        EpochCell& cell = threadRecord_[ticket].data();
        AtomT val;
        do {
            // Internal protect to avoid redundant ticket lookup
            uint64_t current = epoch_.load(std::memory_order_acquire);
            cell.protect(current);

            val = atom.load(std::memory_order_acquire);
        } while(val == atom.load(std::memory_order_acquire)); // Verification loop

        return val;
    }

    // =========================================================================
    // Cache Operations
    // =========================================================================

    /**
     * @brief Attempts to retrieve an index from the thread-local/hot cache.
     *
     * @param[out] out_idx The retrieved index.
     * @return true if an index was retrieved, false if the cache is empty or disabled.
     */
    bool get_cache(Index& out_idx) {
        if constexpr (NO_CACHE) {
            assert(false && "Recycler: get_cache called while cache disabled");
        }
        else {
            return static_cast<RealCache&>(cache_).dequeue(out_idx);
        }
    }

    /**
     * @brief Returns an index to the thread-local/hot cache.
     *
     * @param idx The index to store.
     */
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

    /**
     * @brief Retires an index, scheduling it for reclamation.
     *
     * The index is added to the bucket corresponding to the epoch the *thread*
     * is currently protecting. This ensures that even if the global epoch advances,
     * the item is placed in a bucket consistent with the thread's view.
     *
     * @warning The calling thread **MUST** be protecting an epoch (via `protect_epoch`)
     * before calling this method.
     *
     * @param idx The index to retire.
     */
    void retire(Index idx) {
        uint64_t ticket = get_ticket();
        bool active;
        uint64_t protected_epoch;

        // Snapshot the thread's current state
        threadRecord_[ticket].data().snapshot(active, protected_epoch);

        // Assert contract: Thread MUST be protecting an epoch to retire safely
        assert(active && "Recycler: retire called without epoch protection");

        // Enqueue into the bucket associated with the epoch the thread is actually seeing
        // (This handles cases where the thread is lagging behind the global epoch)
        Bucket& target = get_bucket(protected_epoch, BucketState::Current);
        target.enqueue(idx);
    }

    /**
     * @brief Attempts to reclaim a free index for reuse.
     *
     * Strategies tried in order:
     * 1. Dequeue from the current `Free` bucket (Epoch E-2).
     * 2. If empty, attempt to advance the global epoch.
     * 3. If advanced (by self or others), retry dequeue from the new `Free` bucket.
     *
     * @param[out] out_idx The reclaimed index.
     * @return true if an index was successfully reclaimed, false if the system is exhausted.
     */
    bool reclaim(Index& out_idx) {
        uint64_t ticket = get_ticket();
        int attempts = 0;
        constexpr int MAX_ATTEMPTS = 2;

        while (attempts < MAX_ATTEMPTS) {
            uint64_t e = epoch_.load(std::memory_order_acquire);
            Bucket& free_b = get_bucket(e, BucketState::Free);

            // 1. Try to get from Free bucket
            if (free_b.dequeue(out_idx)) {
                return true;
            }

            if constexpr (!AUTOFIX_LIMBO) {
                // If the bucket uses a manual reset strategy (not auto-fixing),
                // we assume it's now drained and reset it for future reuse.
                free_b.reset();
            }

            // 2. Free bucket empty? Try to advance epoch.
            if (try_advance_epoch(e, ticket)) {
                // Success: Buckets rotated. Reset attempts to check the NEW free bucket.
                attempts = 0;
            } else {
                // Failure: Check if someone else advanced it
                if (epoch_.load(std::memory_order_relaxed) != e) {
                    attempts = 0; // Epoch changed, retry immediately
                } else {
                    attempts++;
                }
            }
        }
        return false;
    }

private:
    // =========================================================================
    // Helpers
    // =========================================================================

    /**
     * @brief Gets the thread ticket, acquiring one via DynamicThreadTicket if necessary.
     * @return The thread's unique ticket ID.
     */
    uint64_t get_ticket() {
        uint64_t t;
        bool ok = ticketing_.acquire(t);
        assert(ok && "Recycler: Thread limit reached");
        return t;
    }

    /**
     * @brief Resolves the bucket for a given epoch and state.
     * @param epoch The reference epoch.
     * @param state The desired logical state.
     * @return Reference to the corresponding bucket.
     */
    Bucket& get_bucket(uint64_t epoch, BucketState state) {
        uint64_t offset = static_cast<uint64_t>(state);
        return buckets_[(epoch + offset) & 3];
    }

    /**
     * @brief Tries to advance the global epoch from `expected_epoch` to `expected_epoch + 1`.
     *
     * Checks if any thread is "stuck" on `expected_epoch - 1` (the Grace epoch).
     * If any active thread holds the Grace epoch, advancement is unsafe.
     *
     * @param expected_epoch The current global epoch value we expect.
     * @param my_ticket      The caller's ticket (used for optimizations internally).
     * @return true if the epoch was successfully advanced by this call.
     */
    bool try_advance_epoch(uint64_t expected_epoch, uint64_t my_ticket) {
        uint64_t unsafe_epoch = expected_epoch - 1; // Unsigned wrap works

        // 1. Scan threads
        const size_t max_t = ticketing_.max_threads();
        bool active;
        uint64_t t_epoch;

        for (size_t i = 0; i < max_t; ++i) {
            threadRecord_[i].data().snapshot(active, t_epoch);

            if (active) {
                // If any thread is stuck on the 'unsafe' epoch (Grace), we cannot advance.
                if (t_epoch == unsafe_epoch) {
                    return false;
                }
            }

            // Optimization: check if global epoch changed during scan
            if (epoch_.load(std::memory_order_relaxed) != expected_epoch) {
                return false;
            }
        }

        // 2. CAS global epoch
        if (epoch_.compare_exchange_strong(expected_epoch, expected_epoch + 1,
                                           std::memory_order_acq_rel)) {
            // 3. Reset the bucket that transitioned Free -> Next
            // (Only implicitly, or explicitly if we managed state here)
            return true;
        }

        return false;
    }

    // =========================================================================
    // Data Members
    // =========================================================================

    ALIGNED_CACHE std::atomic<uint64_t> epoch_;
    CACHE_PAD_TYPES(std::atomic_uint64_t);

    ThreadCell* threadRecord_;
    Ticketing   ticketing_;
    PtrLookupT  lookup_;

    ALIGNED_CACHE CacheMember cache_;
    ALIGNED_CACHE Bucket* buckets_;
};

} // namespace util::hazard::recycler
