#pragma once

#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <array>

#ifndef DTT_MAX_BITS
#define DTT_MAX_BITS 1024
#endif

#ifndef DTT_MAX_INSTANCES
#define DTT_MAX_INSTANCES 16
#endif

namespace util::threading {

/**
 * @brief Dynamic thread ticket manager.
 * 
 * Manages unique thread tickets for a limited number of threads, with support for
 * a runtime-configurable maximum (up to 1024 threads by default). All storage is 
 * stack-allocated—no heap allocation is used.
 * 
 * @note Dynamic thread ticketing is particularly useful in data structures that need to
 * track a fixed number of threads and allow threads to dynamically attach or detach.
 * A common use case is managing per-thread resources such as hazard pointer slots 
 * in concurrent data structures.
 */
class DynamicThreadTicket {
public:
    /**
     * @brief Constructs a DynamicThreadTicket with a specified maximum number of threads.
     * 
     * @param maxThreads The maximum number of threads/tickets allowed (must be <= DTT_MAX_BITS).
     * @throws std::invalid_argument if maxThreads is zero or exceeds DTT_MAX_BITS.
     * @throws std::runtime_error if more instances than DTT_MAX_INSTANCES are created.
     */
    explicit DynamicThreadTicket(uint64_t maxThreads)
        : maxThreads_(maxThreads)
    {
        if (maxThreads_ == 0 || maxThreads_ > MaxThreads)
            throw std::invalid_argument("maxThreads must be in [1, 1024]");

        // Initialize all bits to 1 (all free)
        for (auto& cell : storage_) {
            cell.store(~uint64_t{0}, std::memory_order_relaxed);
        }

        // Initialize thread-local cache
        for (auto& id : id_cache_) {
            id = INVALID_ID;
        }

        // Assign instance ID
        instance_id_ = global_instance_counter_.fetch_add(1, std::memory_order_relaxed);
        if (instance_id_ >= MaxInstances)
            throw std::runtime_error("Too many DynamicThreadTicket instances");
    }

    /**
     * @brief Acquires a ticket for the calling thread.
     * 
     * If the thread already holds a ticket, the cached value is returned.
     * Otherwise, it searches for a free ticket bit and atomically acquires it.
     * 
     * @param ticket_container Reference to store the assigned ticket ID.
     * @return true if a ticket was successfully acquired; false if none were available.
     */
    bool acquire(uint64_t& ticket_container) {
        uint64_t& local_id = get_thread_local_id();
        if (local_id != INVALID_ID) { ticket_container = local_id; return true; }

        for (uint64_t cell = 0; cell < NumCells; ++cell) {
            uint64_t current = storage_[cell].load(std::memory_order_relaxed);
            if (current == 0) continue;

            int bit_pos = count_trailing_zeros(current);
            if (bit_pos < 0) continue;

            uint64_t ticket = cell * 64 + bit_pos;
            if (ticket >= maxThreads_) continue;

            uint64_t mask = uint64_t{1} << bit_pos;
            if (storage_[cell].compare_exchange_weak(current, current & ~mask,
                                                     std::memory_order_acquire,
                                                     std::memory_order_relaxed)) {
                ticket_container = ticket;
                local_id = ticket;
                return true;
            }
        }
        return false;
    }

    /**
     * @brief Releases the ticket held by the calling thread.
     * 
     * Marks the corresponding ticket bit as free. Calling multiple times is safe.
     */
    void release() {
        uint64_t& local_id = get_thread_local_id();
        if (local_id == INVALID_ID) return;

        uint64_t cell = local_id / 64;
        uint64_t bit_pos = local_id % 64;
        uint64_t mask = uint64_t{1} << bit_pos;
        storage_[cell].fetch_or(mask, std::memory_order_release);
        local_id = INVALID_ID;
    }

private:
    static constexpr uint64_t MaxThreads = DTT_MAX_BITS;  ///< Maximum allowed threads/tickets.
    static constexpr uint64_t NumCells = MaxThreads / 64; ///< Number of 64-bit storage cells.
    static constexpr uint64_t MaxInstances = DTT_MAX_INSTANCES; ///< Maximum number of DynamicThreadTicket instances.
    static constexpr uint64_t INVALID_ID = uint64_t(-1); ///< Represents an invalid or unassigned ticket.

    uint64_t instance_id_;  ///< Unique instance ID for this object.
    uint64_t maxThreads_;   ///< Runtime-configurable maximum threads/tickets.

    std::array<std::atomic<uint64_t>, NumCells> storage_; ///< Ticket storage as bitfields.
    std::array<uint64_t, MaxInstances> id_cache_;         ///< Thread-local cached ticket IDs.

    static std::atomic<uint64_t> global_instance_counter_; ///< Tracks total number of instances.

    /**
     * @brief Returns the cached ticket ID for the calling thread.
     * 
     * @return Reference to the thread-local ticket ID.
     */
    uint64_t& get_thread_local_id() { return id_cache_[instance_id_]; }

    /**
     * @brief Counts the number of trailing zeros in a 64-bit value.
     * 
     * @param x The value to inspect.
     * @return Number of trailing zeros, or 64 if x is zero.
     */
    static unsigned count_trailing_zeros(uint64_t x) {
        if (x == 0) return 64;
#if defined(__GNUC__) || defined(__clang__)
        return __builtin_ctzll(x);
#elif defined(_MSC_VER)
        unsigned index;
        _BitScanForward64(&index, x);
        return index;
#else
        unsigned count = 0;
        while ((x & 1) == 0) { x >>= 1; ++count; }
        return count;
#endif
    }
};

// Static initialization of the global instance counter
std::atomic<uint64_t> DynamicThreadTicket::global_instance_counter_{0};

}
