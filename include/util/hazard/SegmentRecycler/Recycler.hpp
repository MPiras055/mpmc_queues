#pragma once
#include <IndexQueue.hpp>
#include <CASLoopSegment.hpp>
#include <HazardCell.hpp>
#include <SingleWriterCell.hpp>
#include <PtrLookup.hpp>

namespace util::hazard {

/**
 * @brief A thread-aware recycler/quarantine for indices (tracked memory locations).
 *
 * Recycler provides:
 *  - Fast-path allocation via a per-Recycle-cache (optional).
 *  - Quarantine semantics based on epochs and per-thread protection (hazard epochs).
 *  - Reclaiming items when they become safe (no thread protects the epoch that would touch them).
 *
 * Template parameters:
 *  - S: element/storage type (the actual object type associated with tracked slots).
 *  - Meta: optional metadata type (unused here, placeholder for future).
 *  - use_cache: compile-time toggle to enable fast-path cache for free indices.
 *
 * Design notes and assumptions:
 *  - There are exactly 4 internal buckets (circular): current, next, grace, free.
 *  - Each thread must 'protect' an epoch before accessing objects that may be retired.
 *  - Storage for S objects is created at construction and owned by Recycler (RAII).
 *  - The external types IndexQueue, CASLoopQueue, PtrLookup, HeapStorage are expected to
 *    provide minimal interfaces described in comments above.
 * 
 *  the recycler uses 4 buckets for any epoch e:
 *  current_bucket   = `buckets[e % 4]`    -> buckets where retire Tmls
 *  grace_bucket     = `buckets[(e+1) % 4]`-> internal bucket used to account for epoch shifts
 *  free_bucket      = `buckets[(e+2) % 4]`-> buckets where to reclaim free Tmls
 *  next_bucket      = `buckets[(e+3) % 4]`-> internal bucket used to account for epoch shifts
 *
 * Thread-safety:
 *  - Public methods are thread-safe following the algorithm's design; many assertions
 *    represent debug-time invariants only (they are not release-time error handling).
 *
 * Warning:
 *  - This implementation uses assertions for bucket overflow assumptions.
 */
template<typename S, typename Meta = void, bool use_cache = true>
class Recycler {
public:
    using Tml = uint32_t;
private:
    using ThreadHazard = HazardCell<SingleWriterCell,void>;   //padded HazardCells
    using Bucket = IndexQueue<true>;
    using Cache = CASLoopQueue<Tml,true>;
public:
    /**
   * @brief Construct a Recycler.
   * @param tracked Number of tracked slots (must be > 0 and <= UINT32_MAX).
   * @param thread_count Number of threads / thread slots used for thread-local hazard cells.
   * @param tml_args Arguments forwarded to construct each S instance (variadic).
   *
   * The constructor initializes:
   *  - (index -> ptr) translation struct
   *  - per-thread hazard cells,
   *  - four buckets,
   *  - the optional fast-path cache (if use_cache == true),
   *  - the reclaim-available counter.
   *
   * @warning this methods uses assertions
   */
    template<typename... Args>
    Recycler(size_t tracked, size_t thread_count, Args&&... tml_args)
            :   epoch{0},
                reclaim_avail_{0},
                thread_local_storage_{thread_count},
                buckets_(4,tracked),
                lookup_{init_lookup(tracked,std::forward<Args>(tml_args)...)},
                cache_{tracked},
                tracked_count_{static_cast<Tml>(tracked)} {
        static_assert(std::is_same_v<Tml,uint32_t>,"Tml must be uint32_t");

        assert(tracked < static_cast<size_t>(UINT32_MAX) && "Recycler: Cannot track more than (UINT32_MAX - 1) elements");
        assert(tracked > 0 && "Recycler: tracked count must be non-null");
        assert(buckets_.capacity() == 4 && "Recycler: buckets must be 4");

        /**
         *  Initialize the Tml status.
         * 
         *  If cache is abilited, then all the Tmls are in cache
         *  else all the Tmls are in the FreeBucket and the available counter is
         *  adjusted
         * 
         *  @note cache and/or free_bucket capacity is always assumed sufficient
         */
        for(Tml i = 0; i < tracked_count_; ++i) {
            if constexpr(use_cache) {
                bool ok = cache_.enqueue(i);
                assert(ok && "Recycler: initial cache enqueue failed");
            } else {
                bool ok = free_bucket_for_epoch(epoch.load(std::memory_order_relaxed)).enqueue(i);
                assert(ok && "Recycler: initial free bucket enqueue failed");
            }
        }

        if constexpr(!use_cache) {
            // if Tmls start in the free_bucket set the available counter
            reclaim_avail_.store(tracked_count_, std::memory_order_release);
        }
    }

    // default constructor and delete every copy/move
    ~Recycler() noexcept = default;
    Recycler(const Recycler&) = delete;
    Recycler& operator=(const Recycler&) = delete;
    Recycler(Recycler&&) = delete;
    Recycler& operator=(Recycler&&) = delete;

    /**
   * @brief Protect the current epoch for the calling thread-ticket.
   *
   * The caller must hold a thread 'ticket' (index in thread_local_storage_).
   * Protecting an epoch stores a snapshot of the current global epoch in the
   * thread-local hazard cell so that reclaim logic will not advance past it
   * while this thread is active.
   *
   * @param ticket Thread-local index (0 <= ticket < thread_local_storage_.size()).
   */
    void protect_epoch(uint64_t ticket) {
        const uint64_t snapshot = epoch.load(std::memory_order_acquire);
        thread_local_cell(ticket).activate(snapshot);
    }

    /**
     * @brief Protect epoch and load an atomic value in a stable manner.
     *
     * Repeatedly protects the epoch and reads the given atomic until two successive
     * reads are equal (ensuring a stable read while holding an epoch protection).
     * 
     * @warning this method sets the calling thread as active on the first epoch when
     * the value is found stable
     * 
     * @tparam T Atomic inner type.
     * @param ticket thread ticket index.
     * @param atom reference to atomic to read.
     * @return a stable value loaded from atom
     */
    template<typename T1>
    T1 protect_epoch_and_load(uint64_t ticket, std::atomic<T1>& atom) {
        T1 val;
        do {
            protect_epoch(ticket);
            val = atom.load(std::memory_order_acquire); 
        } while(val != atom.load(std::memory_order_acquire));
        return val;
    }

    /**
     * @brief Clear epoch protection for the given thread ticket.
     *
     * @param ticket thread-local index.
     */
    void clear_epoch(uint64_t ticket) {
        thread_local_cell(ticket).deactivate();
    }

    /**
     * @brief Try to put a free tracked index into the fast-path cache.
     * @param in tracked index to push into cache.
     * @return true if enqueued successfully, false otherwise.
     */
    bool get_cache(Tml& out) noexcept {
        if constexpr (use_cache) {
            return cache_.dequeue(out);
        } else {
            //if cache is disabled
            return false;
        }
    }

    /**
     * @brief Try to put a free tracked index into the fast-path cache.
     * @param in tracked index to push into cache.
     * @return true if enqueued successfully, false otherwise.
     * 
     * @warning the tracked index must translate to a pointer that is private to the caller 
     */
    bool put_cache(Tml in) noexcept {
        if constexpr (use_cache) {
            bool ok = cache_.enqueue(in);
            assert(ok && "Recycler::put_cache: cache is never supposed to be full");
            return true;
        } else {
            //if cache is disabled, callers should call `retire` instead
            return false; 
        }
    }

    /**
     * @brief Convert a Tml index into a pointer to S.
     * @param in index to decode.
     * @return S* pointer to the associated object. Non-owning pointer.
     */
    S* decode(Tml in) const noexcept {
        return lookup_.getPtr(in);  // DEBUG: have to move to index operator
    }

    /**
     * @brief Retire (quarantine) a tracked index.
     *
     * The caller must be protecting an epoch (i.e., must have called protect_epoch(ticket))
     * prior to retiring an index. Retiring pushes the index into the Current bucket
     * (for that epoch) so it will become reclaimable once no thread protects the epoch.
     *
     * @param in tracked index to retire.
     * @param ticket thread-local index of the retiring thread.
     * @param drop_protection when true, the thread's protection is dropped after retiring.
     */
    void retire(Tml in, uint64_t ticket, bool dropProtection = true) {
        assert(thread_local_cell(ticket).isActive() && 
            "Recycler::retire: thread must protect an epoch to retire a TML");

        const uint64_t e = epoch.load(std::memory_order_acquire);
        
        bool ok = current_bucket_for_epoch(epoch.load(std::memory_order_acquire)).enqueue(in);
        assert(ok && 
            "Recycler::retire: CurrentBucket should always allow for enqueues");

        //increment the available counter
        reclaim_avail_.fetch_add(1ul,std::memory_order_acq_rel);

        if(dropProtection) {    //drop protection after retiring a segment
            thread_local_cell(ticket).deactivate();
        }
    }

    /**
     * @brief Best-effort attempt to reclaim a tracked index from quarantine.
     *
     * The caller must not be currently protecting an epoch (i.e., not inside
     * protect_epoch). The method tries to obtain a free index that is safe to reuse.
     * 
     * @note in order to minimize wasted epoch cycles due to concurrent updates, each caller
     * protects the current epoch in order to try and advance it, this allows for fairness, but
     * shorter protection windows could optimize the system for better responsiveness
     *
     * @param out[out] reclaimed tracked index if true returned.
     * @param ticket thread-local index used to protect while scanning.
     * @return true if an index was reclaimed and written into `out`, false otherwise.
     */
    bool reclaim(Tml& out,uint64_t ticket) {
        //used to try and reclaim something from the quarantine
        assert(!thread_local_cell(ticket).isActive() && 
            "Cannot reclaim while protecting an epoch");

        bool got = false;

        for(;;) {
            // global threshold to bound spins
            if(!available()) return false;

            // protect current epoch while inspecting free bucket for that epoch
            uint64_t snapshot = protect_epoch_and_load(ticket,epoch);
            got = get_from_free_bucket(out,snapshot);
            clear_epoch(ticket);
            if(got) return true;
        
            //don't protect in here
            snapshot = epoch.load(std::memory_order_acquire);
            // check if the epoch can be safely advanced (no active thread holds a prior epoch),
            // attempt to CAS increment epoch by 1
            if(can_advance_epoch(snapshot)) { 

                uint64_t nextEpoch = snapshot + 1;
                (void)epoch.compare_exchange_strong(snapshot,nextEpoch,std::memory_order_acq_rel);
                snapshot = protect_epoch_and_load(ticket,epoch);
                got = get_from_free_bucket(out,snapshot);
                clear_epoch(ticket);
                if(got) return true;
            }

            // spin again and check if any more items are available
            
        }

        //clear the epoch before exiting
        return false;


    }


private:

    /**
     * @brief Initialize underlying S* storage and return a PtrLookup wrapper.
     *
     * @param size number of items to construct
     * @param tml_args argument forward from the constructor 
     * @return PtrLookup<S*> with ownership of the storage
     */
    template<typename... Args>
    static inline PtrLookup<S*> init_lookup(size_t size, Args&&... tml_args) {
        util::memory::HeapStorage<S*> underlying_(size);
        for(size_t i = 0; i < size; i++) {
            underlying_[i] = new S(std::forward<Args>(tml_args)...);
        }
        return PtrLookup<S*>(std::move(underlying_));
    }

    /**
     * @brief Get the index of the free bucket for a given epoch.
     * @warning caller should have protected the epoch
     */
    inline Bucket& free_bucket_for_epoch(uint64_t epoch) noexcept {
        return buckets_[(epoch + 2) % 4];
    }

    /**
     * @brief Get the index of the current bucket for a given epoch.
     * @warning caller should have protected the epoch
     */
    inline Bucket& current_bucket_for_epoch(uint64_t epoch) {
        return buckets_[(epoch) % 4];
    }

    /**
     * @brief helper to get a thread_local SWL for a given ticket
     */
    inline SingleWriterCell& thread_local_cell(uint64_t ticket) {
        return thread_local_storage_[ticket].data;
    }

    /**
     * @brief helper to get a constant view of a thread_local SWL for a given ticket
     */
    inline const SingleWriterCell& thread_local_cell_const(uint64_t ticket) const {
        return thread_local_storage_[ticket].data;
    }

    /**
     * @brief helper to get from free bucker and decrement the available counter
     * @warning caller should have protected the epoch
     */
    inline bool get_from_free_bucket(Tml& out,uint64_t epoch) {
        if(free_bucket_for_epoch(epoch).dequeue(out)) {
            uint64_t old = reclaim_avail_.fetch_sub(1ul,std::memory_order_acq_rel);
            assert(old != 0 && "Recycler: available counter underflow");
            (void)old;
            return true;
        }
        return false;
        
    }

    /**
     * @brief helper to check for reclaimable Tmls
     */
    inline bool available() const noexcept {
        return reclaim_avail_.load(std::memory_order_acquire) > 0;
    }


    /**
     * @brief check whether an epoch can safely advance
     * @par snapshot of the global epoch
     * 
     * The epoch can be advanced only if every actrive thread either:
     * - is inactive or
     * - has the same snapshot value (so advancing won't break their invariants)
     * 
     * @returns true if the epoch can be safely advanced, false otherwise
     * 
     * @warning the following method returns false if the calling thread is active on
     * a prior epoch
     */
    inline bool can_advance_epoch(uint64_t snapshot) const noexcept{
        uint64_t t_epoch = 0;
        bool t_active = true;
        for(size_t i = 0; i < thread_local_storage_.capacity(); i++) {
            thread_local_cell_const(i).snapshot(t_active, t_epoch);
            if(t_active && (t_epoch != snapshot))
                return false;
        }
        return true;
    }

    // -------------------------
    // Member variables
    // -------------------------

    std::atomic<uint64_t> epoch{0};             ///< global epoch counter
    std::atomic<uint32_t> reclaim_avail_{0};    ///< how many items in quarantine
    util::memory::HeapStorage<ThreadHazard> thread_local_storage_;///< per-thread hazard cells (SingleWriter)
    util::memory::HeapStorage<Bucket> buckets_;                   ///< circular set of 4 buckets
    PtrLookup<S*> lookup_;                      ///< index => pointer translation (immutable)
    Cache cache_;                               ///< optional fast path cache
    const uint32_t tracked_count_{};            ///< how many globally tracked Tmls
                                            
};
}   //namespace util::hazard