#pragma once
#include <atomic>
#include <cstddef>
#include <vector>
#include <type_traits>
#include <cassert>
#include <specs.hpp>
#include <HazardCell.hpp>

namespace util::hazard {

#ifndef HV_MAX_THREADS
#define HV_MAX_THREADS 256
#endif

#ifndef HV_MAX_HPS
#define HV_MAX_HPS 1
#endif

#ifndef THRESHOLD_R
#define THRESHOLD_R 0
#endif

/**
 * @brief Vector of hazard pointers with per-thread retired lists.
 *
 * Implements a hazard pointer pattern for safe memory reclamation in lock-free structures.
 *
 * @tparam T Pointer type for hazard cells.
 */
template<typename T, typename Meta = void>
class HazardVector {
    static_assert(std::is_pointer_v<T>, "HazardVector requires T to be a pointer type");

    // Prevent accidental copies/moves (would double-free).
    HazardVector(const HazardVector&) = delete;
    HazardVector& operator=(const HazardVector&) = delete;
    HazardVector(HazardVector&&) = delete;
    HazardVector& operator=(HazardVector&&) = delete;

public:
    /**
     * @brief Constructs a HazardVector for a given number of threads.
     *
     * @param maxThreads Maximum number of threads that will use this hazard vector.
     */
    explicit HazardVector(size_t maxThreads)
        : maxThreads_(maxThreads)
    {
        assert(maxThreads_ <= HV_MAX_THREADS && "maxThreads exceeds HV_MAX_THREADS");
        storage_init();
    }

    /**
     * @brief Destructor reclaims any remaining retired objects.
     */
    ~HazardVector() {
        for (size_t i = 0; i < maxThreads_; ++i) {
            for (auto obj : retired_[i].v) {
                delete obj;
            }
        }
    }

    /**
     *  @brief Iterate on all threads metadata
     *  @param fn Lambda that operates on a const reference to metadata
     *  @warning This method can only be used on a HazardVector instance that declares
     *           a metadata type (i.e., not void)
     */
    template<typename Func>
    void metadataIter(Func&& fn) const {
        if constexpr (!std::is_same_v<Meta,void>) {
            for (size_t tid = 0; tid < maxThreads_; ++tid) {
            fn(storage_[tid].get_metadata_ronly_()); // passes const Meta&
        }
        } else {
            static_assert(!sizeof(Meta),"metadataIter is noly available when Meta is non-void");
        }
    }


    /**
     * @brief checks if a raw pointer is being held by any thread
     *
     * @param ptr Pointer to protect.
     * @param tid Thread ID.
     * @param hpid Hazard pointer slot ID (default 0).
     * @param firstToHold reference to be filled with any cell that holds the ptr
     * @return true if any thread holds the pointer false otherwise
     * @warning it doesn't check if the calling thread is holding the pointer
     */
    bool isProtected(T to_check, uint64_t ticket, size_t& firstToHold) {
        for(size_t i = 0; i < maxThreads_; i++) {
            if(i == ticket)
                continue;

            for(size_t j = 0; j < HV_MAX_HPS; j++) {
                if(to_check == storage_[i].data[j].load(std::memory_order_acquire)) {
                    firstToHold = i;
                    return true;
                }
            }
        }
        return false;
    }

    /**
     * @brief checks if a raw pointer is being held by any thread
     *
     * @param ptr Pointer to protect.
     * @param tid Thread ID.
     * @return true if any thread holds the pointer false otherwise
     * @warning it doesn't check if the calling thread is currently
     * holding the pointer
     */
    bool isProtected(T to_check, uint64_t ticket) {
        for(size_t i = 0; i < maxThreads_; i++) {
            if(i == ticket)
                continue;

            for(size_t j = 0; j < HV_MAX_HPS; j++) {
                if(to_check == storage_[i].data[j].load(std::memory_order_acquire)) {
                    return true;
                }
            }
        }
        return false;
    }

    /**
     * @brief checks if a raw pointer is being held by any thread
     *
     * @param ptr Pointer to protect.
     *
     * @return true if any thread holds the pointer false otherwise
     *
     */
    bool isProtected(T to_check) {
        for(size_t i = 0; i < maxThreads_; i++) {
            for(size_t j = 0; j < HV_MAX_HPS; j++) {
                if(to_check == storage_[i].data[j].load(std::memory_order_acquire)) {
                    return true;
                }
            }
        }
        return false;
    }


    /**
     * @brief Protects a raw pointer in a hazard cell.
     *
     * @param ptr Pointer to protect.
     * @param tid Thread ID.
     * @param hpid Hazard pointer slot ID (default 0).
     * @return The protected pointer.
     */
    inline T protect(T ptr, size_t tid, size_t hpid = 0) {
        assert(tid < maxThreads_ && hpid < HV_MAX_HPS);
        storage_[tid].data[hpid].store(ptr, std::memory_order_release);
        return ptr;
    }

    /**
     * @brief Protects a pointer loaded from an atomic.
     *
     * Spins until the value stabilizes.
     *
     * @param atom Atomic pointer to load and protect.
     * @param tid Thread ID.
     * @param hpid Hazard pointer slot ID (default 0).
     * @return The protected pointer.
     */
    inline T protect(const std::atomic<T>& atom, size_t tid, size_t hpid = 0){
        assert(tid < maxThreads_ && hpid < HV_MAX_HPS);
        while(true) {
            T tmp = atom.load(std::memory_order_acquire);
            storage_[tid].data[hpid].store(tmp,std::memory_order_release);
            if(atom.load(std::memory_order_acquire) == tmp) {
                return tmp;
            }
        }
    }

    /**
     * @brief Clears a hazard pointer.
     *
     * @param tid Thread ID.
     * @param hpid Hazard pointer slot ID (default 0).
     */
    inline void clear(size_t tid, size_t hpid = 0) {
        assert(tid < maxThreads_ && hpid < HV_MAX_HPS);
        storage_[tid].data[hpid].store(nullptr, std::memory_order_release);
    }

    /**
     * @brief getter for the metadata field
     *
     * @note each thread can have some metadata that they can access and modify
     */
    template<
        typename M = Meta,
        typename Enable = std::enable_if_t<!std::is_same_v<M, void>>
    >
    M& getMetadata(uint64_t tid);


    /**
     * @brief Retires a pointer and tries to reclaim memory from the per-thread ReclaimBucket
     *
     * @param ptr Pointer to retire.
     * @param tid Thread ID performing the retire.
     * @param checkThreshold [default = false] Whether to skip reclamation if retired list is below threshold.
     * @return Number of objects deleted during this call.
     */
    size_t retire(T ptr, size_t tid, bool checkThreshold = false) {
        assert(tid < maxThreads_);
        if (!ptr) return 0;

        retired_[tid].v.push_back(ptr);

        return (checkThreshold && retired_[tid].v.size() < THRESHOLD_R) ? 0 : collect(tid);
    }

    /**
     * @brief Reclaims memory from the per-thread RetireBucket if possible
     *
     * @param tid Thread ID performing the retire.
     * @return Number of objects deleted during this call.
     */
    size_t collect(size_t tid) {
        size_t deleted = 0;
        for (size_t i = 0; i < retired_[tid].v.size(); ) {
            T obj = retired_[tid].v[i];
            bool canDelete = true;

            // Scan all hazard pointers
            for (size_t t = 0; t < maxThreads_ && canDelete; ++t) {
                for (size_t h = 0; h < HV_MAX_HPS; ++h) {
                    if (storage_[t].data[h].load(std::memory_order_acquire) == obj) {
                        canDelete = false;
                        break;
                    }
                }
            }

            if (canDelete) {
                std::swap(retired_[tid].v[i], retired_[tid].v.back());
                retired_[tid].v.pop_back();
                delete obj;
                ++deleted;
                continue; // do not increment i, new element at i
            }
            ++i;
        }

        return deleted;
    }

private:

    /**
     * @brief private method to initialize the underlying storage
     */
    void storage_init() {
        for(size_t i = 0; i < maxThreads_; i++) {
            auto& cell = storage_[i];
            auto& data = cell.getData();
            for(size_t j = 0; j < HV_MAX_HPS; j++){
                data[j].store(nullptr,std::memory_order_relaxed);
            }
            if constexpr (
                !(std::is_same_v<Meta,void> ||
                std::is_default_constructible_v<Meta>)
            ) {
                cell.meta.init();
            }
        }
    }


    template<typename T1>
    struct alignas(CACHE_LINE) RetiredBucket {
        std::vector<T1> v;
        char _pad[CACHE_LINE - (sizeof(std::vector<T1>) % CACHE_LINE ? (sizeof(std::vector<T1>) % CACHE_LINE) : 0)];
    };


    size_t maxThreads_; ///< Maximum threads supported

    /// Hazard pointer storage: [thread][hazard slot], aligned to cache line
    alignas(CACHE_LINE) HazardCell<std::atomic<T>[HV_MAX_HPS],Meta> storage_[HV_MAX_THREADS];

    /// Per-thread retired objects, aligned to cache line
    RetiredBucket<T> retired_[HV_MAX_THREADS];
};

    // Definition outside class
    template<typename T, typename Meta>
    template<typename M, typename>
    inline M& HazardVector<T, Meta>::getMetadata(uint64_t tid) {
        return storage_[tid].getMetadata();
    }

}   //namespace util::hazard
