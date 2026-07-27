#pragma once
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <utility>
#include <type_traits>
#include <cassert>

#include <mem/detail/RingSlab.hpp>          //Positionally-addressed collection of index rings
#include <util/hazard/Recycler/PtrLookup.hpp> //Immutable Lookup Table Implementation
#include <meta/OptionsPack.hpp>          //Template Options
#include <util/hazard/HazardCell.hpp>           //Padded SingleWriterLocation with general Metadata field
#include <util/threading/DynamicThreadTicket.hpp>  //TLS Ticket Manager
#include <util/bit.hpp>
#include <util/specs.hpp>                //Cache alignment and compatibility checks

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
template<typename T, size_t Capacity, typename Opt = meta::EmptyOptions, typename Meta = void,
         typename Lookup = details::ImmutablePtrLookup<T>>
class Recycler {
    using Epoch = uint64_t;

    struct EpochCell {
        static constexpr Epoch INACTIVE = 0;

        std::atomic<Epoch> local_epoch;
        void snapshot(bool& active, Epoch& epoch) const {
            Epoch e = local_epoch.load();
            active = bit::get_msb(e) != 0;
            epoch  = bit::clear_msb(e);
            return;
        }

        void protect(Epoch e) {
            local_epoch.store(bit::set_msb<Epoch>(e));
        }

        void clear() {
            local_epoch.store(INACTIVE);
        }
    };

    using ThreadCell    = hazard::HazardCell<EpochCell,Meta>;
    // Defaulted to ImmutablePtrLookup, which builds each T inline in a flat array.
    // Segments allocated as a single block (header + trailing cells) cannot be built
    // that way, so mem::source::Pool substitutes mem::detail::SlabLookup.
    using PtrLookupT    = Lookup;
    using Ticketing     = threading::DynamicThreadTicket;
    using Ticket        = Ticketing::Ticket;


    // Configuration
    static constexpr bool NO_CACHE      = Opt::template has<RecyclerOpt::Disable_Cache>;

    // The buckets hold slot indices, which is exactly what an index ring stores.
    using Bucket        = mem::detail::RingSlab::Ring;
    using RealCache     = mem::detail::RingSlab::Ring;

    struct DisabledCache {
        DisabledCache() = default;
    };
    using CacheMember    = Bucket;

    static constexpr uint8_t FREE_OFFSET     = 2;
    static constexpr uint8_t STAGE_OFFSET    = 0;
    static constexpr size_t STAGES = 4; //4 Buckets used: Current : Grace : Free : Next

public:
    template<typename... Args>
    explicit Recycler(size_t maxThreads, Args&&... args) :
        // Listed in declaration order: members are initialized in that order regardless,
        // and cache_ aliases into buckets_, so buckets_ must precede it.
        threadRecord_{new ThreadCell[maxThreads]},
        ticketing_{maxThreads},
        lookup_(Capacity, std::forward<Args>(args)...),
        epoch_{0},
        buckets_(STAGES + (NO_CACHE? 0 : 1), Capacity),
        cache_{*buckets_.get(NO_CACHE? 0 : STAGES)}
    {
        // Initialize: Fill the 'initial' Free bucket (index 2 for epoch 0)

        Bucket& initialFree = free_bucket(0);
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
    void metadataInit(Func&& f) const {
        if constexpr (!std::is_void_v<Meta>) {
            for(size_t i = 0; i < ticketing_.max_threads(); ++i) {
                f(threadRecord_[i].metadata());
            }
        } else {
            assert(false && "Recycler: metadataIter called on void Metadata");
            std::abort();
        }
    }


    [[nodiscard]] bool register_thread() noexcept {
        Ticket t;
        return ticketing_.acquire(t);
    }

    void unregister_thread() {
        ticketing_.release();
    }

    // =========================================================================
    // Pointer Access
    // =========================================================================

    T* decode(size_t idx) const noexcept {
        return lookup_[idx];
    }

    /// Calling thread's ticket, for indexing caller-owned per-thread state.
    size_t ticket() noexcept { return get_ticket(); }

    size_t max_threads() const noexcept { return ticketing_.max_threads(); }

    // =========================================================================
    // Epoch Protection
    // =========================================================================

    void protect_epoch() {
        Ticket ticket = get_ticket();
        Epoch epoch = bit::set_msb(epoch_.load());
        threadRecord_[ticket].data().protect(epoch);
    }

    void clear_epoch() {
        uint64_t ticket = get_ticket();
        threadRecord_[ticket].data().clear();
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
        bool active;
        Epoch current_epoch;
        EpochCell& c = threadRecord_[ticket].data();

        // 1. Check if we are already protecting an epoch
        c.snapshot(active, current_epoch);

        if (!active) {
            // Protect the current epoch
            protect_epoch();
            current_epoch = epoch_.load();
        }

        (void)stage_bucket(current_epoch).enqueue(idx);  //enqueue always succeeds

        // cleanup if we protected
        if (!active) clear_epoch();

    }

    bool reclaim(size_t& index) {
        uint64_t ticket = get_ticket();
        Epoch e;
        bool active;
        EpochCell& cell = threadRecord_[ticket].data();
        cell.snapshot(active,e);
        return active? reclaim_while_protecting(e,index):
        reclaim_unprotected(cell,index);
    }

    static Epoch next_epoch(Epoch e) {
        return (e + 1) & 3;
    }


    bool reclaim_while_protecting(Epoch protected_e, size_t& index) {

        //check the free bucket for the protected epoch
        if(free_bucket(protected_e).dequeue(index))
            return true;

        //epoch might have shifted (at most by one: since of the protecting)

        Epoch current = epoch_.load();

        if(current != protected_e) //no point in checking for advancement
            return free_bucket(current).dequeue(index);
        //try advance the epoch
        else if(can_advance_epoch(current)) {
            current = next_epoch(current);
            epoch_.store(current);
            //if we advanced the epoch we have to account for the advancement
            return free_bucket(current).dequeue(index);
        } else {    //the advancement failed (either epoch mismatch or one thread was stale)
            current = epoch_.load();
            return current != protected_e?
                free_bucket(current).dequeue(index):   //epoch was advanced in between our check
                false;  //epoch cannot be advanced (we already checked the free bucket for this epoch)
        }
    }

    bool reclaim_unprotected(EpochCell& c, size_t& index) {
        static constexpr unsigned COUNT = 3;    //full cycle
        Epoch e;
        [[maybe_unused]] bool active;
        unsigned i = COUNT;

        while(i != 0) {
            protect_epoch();
            c.snapshot(active, e);

            if(free_bucket(e).dequeue(index)){
                clear_epoch();
                return true;
            }

            //check if epoch shifted (we could have dequeued from next bucket)
            Epoch s = epoch_.load();
            if(s != e) continue;   //update epoch protection and retry

            i--;

            //try to advance
            if(can_advance_epoch(e)) {
                e = next_epoch(e);
                epoch_.store(e);
            }
        }
        clear_epoch();
        return false;
    }

private:
    uint64_t get_ticket() {
        uint64_t t;
        bool ok = ticketing_.acquire(t);
        assert(ok && "Recycler: Thread limit reached");
        if(!ok) std::abort();
        return t;
    }

    Bucket& stage_bucket(Epoch e) {
        return *buckets_.get((e + STAGE_OFFSET) & 3);
    }

    Bucket& free_bucket(Epoch e) {
        return *buckets_.get((e + FREE_OFFSET) & 3);
    }

    bool can_advance_epoch(Epoch expected_epoch) const {
        const size_t max_t = ticketing_.max_threads();
        bool active;
        Epoch t_epoch;

        for (size_t i = 0; i < max_t; ++i) {
            //acquire epoch and active state of all threads
            threadRecord_[i].data().snapshot(active, t_epoch);

            //kind of optimization: if the epoch already shifted then it's pointless to check all thread states
            if(epoch_.load() != expected_epoch)
                return false;

            //if any thread is active but stuck on a previous epoch we cannot advance
            if (active && (t_epoch != expected_epoch)) {
                return false;
            }
        }
        return true;
    }

    ThreadCell* threadRecord_;
    Ticketing   ticketing_;
    PtrLookupT  lookup_;

    ALIGNED_CACHE std::atomic<Epoch> epoch_{0};
    CACHE_PAD_TYPES(std::atomic<Epoch>);
    mem::detail::RingSlab buckets_;
    CacheMember& cache_;
};

} // namespace util::hazard::recycler
