#pragma once
#include <atomic>
#include <cstddef>
#include <vector>
#include <type_traits>
#include <cassert>

#ifndef HV_MAX_THREADS
#define HV_MAX_THREADS 256
#endif

#ifndef HV_MAX_HPS
#define HV_MAX_HPS 1
#endif

#ifndef THRESHOLD_R
#define THRESHOLD_R 0
#endif

/// Cache line size for alignment
constexpr size_t CACHE_LINE = 64;

/**
 * @brief Vector of hazard pointers with per-thread retired lists.
 * 
 * Implements a hazard pointer pattern for safe memory reclamation in lock-free structures.
 * 
 * @tparam T Pointer type for hazard cells.
 */
template<typename T>
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
     * @brief Protects a raw pointer in a hazard cell.
     * 
     * @param ptr Pointer to protect.
     * @param tid Thread ID.
     * @param hpid Hazard pointer slot ID (default 0).
     * @return The protected pointer.
     */
    T protect(T ptr, size_t tid, size_t hpid = 0) {
        assert(tid < maxThreads_ && hpid < HV_MAX_HPS);
        storage_[tid][hpid].ptr.store(ptr, std::memory_order_release);
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
    T protect(const std::atomic<T>& atom, size_t tid, size_t hpid = 0){
        assert(tid < maxThreads_ && hpid < HV_MAX_HPS);
        T n = nullptr;
        T ret;
        while ((ret = atom.load(std::memory_order_acquire)) != n) {
            storage_[tid][hpid].ptr.store(ret, std::memory_order_release);
            n = ret;
        }
        return ret;
    }

    /**
     * @brief Clears a hazard pointer.
     * 
     * @param tid Thread ID.
     * @param hpid Hazard pointer slot ID (default 0).
     */
    void clear(size_t tid, size_t hpid = 0) {
        assert(tid < maxThreads_ && hpid < HV_MAX_HPS);
        storage_[tid][hpid].ptr.store(nullptr, std::memory_order_release);
    }

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

        return checkThreshold && retired_[tid].v.size() < THRESHOLD_R ? 0 : collect(tid);
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
                    if (storage_[t][h].ptr.load(std::memory_order_acquire) == obj) {
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
     * @brief A cell storing a pointer, padded to a cache line to avoid false sharing.
     * 
     * @tparam T Pointer type stored in the hazard cell.
     */
    template<typename T1>
    struct alignas(CACHE_LINE) HazardCell {
        static_assert(std::is_pointer_v<T1>, "HazardCell requires T to be a pointer type");
        std::atomic<T1> ptr{nullptr};
        // Ensure the struct size is at least one full cache line
        static_assert(sizeof(std::atomic<T1>) <= CACHE_LINE, "atomic<T> bigger than cache line");
        // Force the element size to one full cache line
        char _pad[CACHE_LINE - sizeof(std::atomic<T1>) > 0 ? (CACHE_LINE - sizeof(std::atomic<T1>)) : 0];
    };
    static_assert(alignof(HazardCell<void*>) == CACHE_LINE);
    static_assert(sizeof(HazardCell<void*>) % CACHE_LINE == 0);

    template<typename T1>
    struct alignas(CACHE_LINE) RetiredBucket {
        std::vector<T1> v;
        char _pad[CACHE_LINE - (sizeof(std::vector<T1>) % CACHE_LINE ? (sizeof(std::vector<T1>) % CACHE_LINE) : 0)];
    };


    size_t maxThreads_; ///< Maximum threads supported

    /// Hazard pointer storage: [thread][hazard slot], aligned to cache line
    alignas(CACHE_LINE) HazardCell<T> storage_[HV_MAX_THREADS][HV_MAX_HPS];

    /// Per-thread retired objects, aligned to cache line
    RetiredBucket<T> retired_[HV_MAX_THREADS];
};
