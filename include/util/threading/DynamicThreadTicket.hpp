#pragma once
#include <atomic>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <specs.hpp>

#ifndef DTT_MAX_BITS
#define DTT_MAX_BITS 1024
#endif

#ifndef DTT_MAX_INSTANCES
#define DTT_MAX_INSTANCES 16
#endif

namespace util::threading {

/**
 * @brief Dynamic per-thread ticket manager (header-only, no heap).
 *
 * - Each thread can acquire one ticket per manager instance.
 * - If a thread already holds a ticket, repeated acquire() returns the same ticket (fast TLS path).
 * - If a thread doesn't hold a ticket yet, it always acquires the *smallest available* ticket.
 * - release() frees the ticket and clears the TLS cache for that instance.
 * - Up to DTT_MAX_INSTANCES independent manager instances; each maintains its own tickets.
 *
 * Storage invariants:
 * - storage_[i] is a 64-bit bitset (1=free, 0=held).
 * - At construction, only the first `maxThreads_` bits are initialized to 1; the rest are 0.
 */
class DynamicThreadTicket {
public:
    /// Compile-time caps.
    static constexpr std::uint64_t MaxThreads   = DTT_MAX_BITS;
    static constexpr std::uint64_t MaxInstances = DTT_MAX_INSTANCES;
    static constexpr std::uint64_t NumCells     = (MaxThreads + 63) / 64;

    /// Sentinel for "no ticket".
    static constexpr std::uint64_t INVALID_ID   = std::uint64_t(-1);

    using Ticket = uint64_t;

    /**
     * @brief Construct a manager with a runtime cap on tickets.
     * @param maxThreads number of usable tickets (1..MaxThreads)
     * @throws std::invalid_argument if maxThreads is out of range
     * @throws std::runtime_error if no instance slot is available
     */
    explicit DynamicThreadTicket(std::uint64_t maxThreads)
        : instance_id_(allocate_instance_id()),
          maxThreads_(maxThreads)
    {
        if (maxThreads_ == 0 || maxThreads_ > MaxThreads) {
            free_instance_id(instance_id_);
            throw std::invalid_argument("maxThreads out of range");
        }

        // Initialize the ticket bitset: first maxThreads_ bits set to 1 (free), remainder 0.
        std::uint64_t remaining = maxThreads_;
        for (std::uint64_t i = 0; i < NumCells; ++i) {
            std::uint64_t bits = 0;
            if (remaining >= 64) {
                bits = ~std::uint64_t{0};
                remaining -= 64;
            } else if (remaining > 0) {
                bits = (std::uint64_t{1} << remaining) - 1; // lower 'remaining' bits set
                remaining = 0;
            } else {
                bits = 0;
            }
            storage_[i].store(bits, std::memory_order_relaxed);
        }

        // Ensure this thread's TLS slot for this instance is initialized.
        tls_id_cache()[instance_id_] = INVALID_ID;
    }

    /**
     * @brief Destructor: returns instance ID to the global pool.
     * @note Callers should ensure threads release their tickets before destruction.
     */
    ~DynamicThreadTicket() {
        free_instance_id(instance_id_);
    }

    DynamicThreadTicket(const DynamicThreadTicket&) = delete;
    DynamicThreadTicket& operator=(const DynamicThreadTicket&) = delete;
    DynamicThreadTicket(DynamicThreadTicket&&) = delete;
    DynamicThreadTicket& operator=(DynamicThreadTicket&&) = delete;

    /**
     * @brief Acquire a ticket for the calling thread.
     *
     * Fast path: if the thread already holds a ticket for this instance, return the cached value.
     * Slow path: pick the *smallest available* ticket by scanning the bitset and clearing the first 1-bit.
     *
     * @param out_ticket written with the acquired ticket on success
     * @return true if a ticket was acquired (or reused), false if none available
     */
    bool acquire(Ticket& out_ticket) {
        std::uint64_t& local_id = tls_id_cache()[instance_id_];

        // Fast path: already have a ticket for this instance.
        if (local_id != INVALID_ID) {
            out_ticket = local_id;
            return true;
        }

        // Slow path: find the smallest available ticket.
        for (std::uint64_t cell = 0; cell < NumCells; ++cell) {
            std::uint64_t cur = storage_[cell].load(std::memory_order_relaxed);

            // While there exists at least one free bit in this cell.
            while (cur != 0) {
                unsigned bit = count_trailing_zeros(cur);   // index of lowest set bit
                std::uint64_t ticket = cell * 64 + bit;
                if (ticket >= maxThreads_) break;          // outside runtime range

                const std::uint64_t mask    = (std::uint64_t{1} << bit);
                const std::uint64_t desired = (cur & ~mask); // 1 -> 0 (claim)

                // Try to claim this bit.
                if (storage_[cell].compare_exchange_weak(
                        cur, desired,
                        std::memory_order_acquire,
                        std::memory_order_relaxed)) {
                    local_id   = ticket;     // cache in TLS for this thread
                    out_ticket = ticket;
                    return true;
                }
                // On CAS failure, 'cur' has been updated; loop to try the next 1-bit.
            }
        }

        // No free tickets.
        return false;
    }

    /**
     * @brief Release the ticket held by the calling thread (if any).
     * Safe to call multiple times (idempotent).
     */
    void release() {
        std::uint64_t& local_id = tls_id_cache()[instance_id_];
        if (local_id == INVALID_ID) return;

        const std::uint64_t cell = local_id / 64;
        const std::uint64_t bit  = local_id % 64;
        storage_[cell].fetch_or(std::uint64_t{1} << bit, std::memory_order_release);

        // Clear TLS so the next acquire (for this thread) recomputes from smallest free.
        local_id = INVALID_ID;
    }

    /// @return runtime-configured maximum number of tickets for this instance.
    std::uint64_t max_threads() const noexcept { return maxThreads_; }

    /// @return instance identifier in [0, MaxInstances).
    std::uint64_t instance_id() const noexcept { return instance_id_; }

private:
    // ===== TLS =====

    /**
     * @brief Per-thread cache of ticket IDs, one slot per instance.
     * cache[i] is the ticket held by this thread for instance i, or INVALID_ID.
     */
    static std::array<std::uint64_t, MaxInstances>& tls_id_cache() {
        static thread_local std::array<std::uint64_t, MaxInstances> cache;
        static thread_local bool initialized = false;
        if (!initialized) {
            cache.fill(INVALID_ID);
            initialized = true;
        }
        return cache;
    }


    // ===== Instance ID management =====

    static std::uint64_t allocate_instance_id() {
        std::uint64_t cur = instance_bitmap_.load(std::memory_order_relaxed);
        while (cur != 0) {
            unsigned bit = count_trailing_zeros(cur);     // pick lowest free instance slot
            const std::uint64_t mask = (std::uint64_t{1} << bit);
            if (instance_bitmap_.compare_exchange_weak(
                    cur, cur & ~mask,
                    std::memory_order_acquire,
                    std::memory_order_relaxed)) {
                return bit;
            }
            // cur updated by CAS failure; retry
        }
        throw std::runtime_error("Too many DynamicThreadTicket instances");
    }

    static void free_instance_id(std::uint64_t id) {
        if (id >= MaxInstances) return;
        instance_bitmap_.fetch_or(std::uint64_t{1} << id, std::memory_order_release);
    }

    // ===== bit ops =====
    static unsigned count_trailing_zeros(std::uint64_t x) {
        if (x == 0) return 64;
#if defined(__GNUC__) || defined(__clang__)
        return static_cast<unsigned>(__builtin_ctzll(x));
#elif defined(_MSC_VER) && defined(_M_X64)
        unsigned long idx;
        _BitScanForward64(&idx, x);
        return static_cast<unsigned>(idx);
#else
        unsigned c = 0;
        while ((x & 1u) == 0u) { x >>= 1u; ++c; }
        return c;
#endif
    }

private:
    // ===== Data =====
    std::uint64_t instance_id_;   ///< Unique instance identifier [0..MaxInstances).
    std::uint64_t maxThreads_;    ///< Runtime cap on tickets for this instance.

    // Ticket bitset: storage_[i] has 1-bits for free tickets, 0-bits for held tickets.
    ALIGNED_CACHE std::array<std::atomic<std::uint64_t>, NumCells> storage_{};

    // Global bitset of free instance IDs: 1=free, 0=used (low MaxInstances bits used).
    static inline std::atomic<std::uint64_t> instance_bitmap_{
        (MaxInstances >= 64) ? ~std::uint64_t{0}
                             : ((std::uint64_t{1} << MaxInstances) - 1)
    };
};

} // namespace util::threading
