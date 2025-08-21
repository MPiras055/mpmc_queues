#pragma once

#include <atomic>
#include <stdexcept>
#include <thread>
#include <memory>
#include <limits>
#include <vector>
#include <IStoragePolicy.hpp>

#if __cplusplus >= 202002L
#include <bit>  // C++20 std::countr_zero
#endif

#ifdef _MSC_VER
#include <intrin.h>
#pragma intrinsic(_BitScanForward64)
#endif

#ifndef DTHREAD_TICKET_MAX
#define DTHREAD_TICKET_MAX 128
#endif

/**
 * @brief Manages dynamic thread tickets for concurrent access in queues (such as for hazard pointers).
 * 
 * The `DynamicThreadTicket` class provides a mechanism to assign a unique, contiguous ID to each thread.
 * Thread IDs are cached per thread to avoid redundant computations. Tickets are assigned using atomic
 * operations and thread-local storage, ensuring efficient multi-threaded behavior.
 */
template <uint64_t MaxInstances = DTHREAD_TICKET_MAX>
class DynamicThreadTicket {
public:
    /**
     * @brief Constructor takes ownership of the StoragePolicy for managing ticket allocation.
     * 
     * @param storage A unique pointer to a storage policy for atomic ticket cells.
     * @throws std::runtime_error if there are too many instances or invalid storage.
     */
    explicit DynamicThreadTicket(std::unique_ptr<IStoragePolicy<std::atomic<uint64_t>>> storage)
        : storage_(std::move(storage))
    {
        // Initialize instance ID and check if the maximum allowed instances have been exceeded.
        instance_id_ = global_instance_counter_.fetch_add(1, std::memory_order_relaxed);
        if (instance_id_ >= MaxInstances) {
            throw std::runtime_error("Too many DynamicThreadTicket instances");
        }

        // Ensure storage is valid (i.e., at least one ticket cell)
        uint64_t total_bits = storage_->capacity() * 64;
        if (total_bits == 0) {
            throw std::invalid_argument("StoragePolicy must provide at least one cell");
        }

        // Initialize all bits to 1 (free)
        for (uint64_t i = 0; i < storage_->capacity(); ++i) {
            storage_->data()[i].store(~uint64_t{0}, std::memory_order_relaxed);
        }

        max_threads_ = total_bits;
    }

    /**
     * @brief Acquires a ticket for the current thread.
     * 
     * If the thread already holds a ticket, it will be reused (cached).
     * Otherwise, the method attempts to find a free ticket in the storage and atomically assigns it.
     * 
     * @param ticket_container A reference to store the assigned ticket ID.
     * @return True if a ticket was successfully acquired; false otherwise.
     */
    bool acquire(uint64_t& ticket_container) {
        uint64_t& local_id = get_thread_local_id();

        // Return the cached ticket if already acquired
        if (local_id != INVALID_ID) {
            ticket_container = local_id;
            return true;
        }

        // Iterate through storage to find a free ticket slot
        const uint64_t num_cells = (max_threads_ + 63) / 64; // ceil(max_threads_ / 64)
        for (uint64_t cell = 0; cell < num_cells; ++cell) {
            uint64_t current = storage_->data()[cell].load(std::memory_order_relaxed);
            if (current == 0) continue;  // Skip empty cells

            // Find the first free bit in the current cell
            int bit_pos = count_trailing_zeros(current);
            if (bit_pos < 0) continue;  // No free bit in this cell

            uint64_t ticket = cell * 64 + bit_pos;
            if (ticket >= max_threads_) continue; // Prevent out-of-bounds ticket assignment

            uint64_t mask = uint64_t{1} << bit_pos;
            // Try to atomically set the ticket as taken (using CAS)
            if (storage_->data()[cell].compare_exchange_weak(current, current & ~mask,
                                                            std::memory_order_acquire,
                                                            std::memory_order_relaxed)) {
                ticket_container = ticket;
                local_id = ticket;
                return true;
            }
        }

        return false; // No free tickets available
    }

    /**
     * @brief Releases a previously held ticket, marking it as available for other threads.
     * 
     * This method is idempotent: calling it multiple times will have no adverse effect.
     */
    void release() {
        uint64_t& local_id = get_thread_local_id();
        if (local_id == INVALID_ID) {
            return; // No ticket to release
        }

        uint64_t cell = local_id / 64;
        uint64_t bit_pos = local_id % 64;
        uint64_t mask = uint64_t{1} << bit_pos;
        // Mark the ticket as available by setting the corresponding bit
        storage_->data()[cell].fetch_or(mask, std::memory_order_release);

        // Reset the local thread ID to INVALID_ID
        local_id = INVALID_ID;
    }

    /**
     * @brief Returns the number of active tickets (tickets that have been acquired by threads).
     * 
     * @return The number of active tickets.
     */
    uint64_t active_count() const {
        uint64_t count = 0;
        uint64_t total_bits = storage_->capacity() * 64;

        // Count active tickets by examining all storage cells
        for (uint64_t i = 0; i < storage_->capacity(); ++i) {
            uint64_t current = storage_->data()[i].load(std::memory_order_relaxed);
            count += popcount64(current);
        }

        return total_bits - count;  // Subtract free bits from total to get active tickets
    }

private:
    static constexpr uint64_t INVALID_ID = uint64_t(-1);  // ID representing an invalid or unassigned ticket

    std::unique_ptr<IStoragePolicy<std::atomic<uint64_t>>> storage_;  // Storage policy for ticket cells
    uint64_t instance_id_;  // Unique instance ID for this thread ticket instance
    uint64_t max_threads_;  // Maximum number of threads (max tickets)

    static thread_local std::array<uint64_t, MaxInstances> id_cache_;  // Cached thread IDs

    /**
     * @brief Gets the cached thread ID for the current thread.
     * 
     * @return A reference to the cached thread ID.
     */
    uint64_t& get_thread_local_id() {
        return id_cache_[instance_id_];
    }

    /**
     * @brief Counts the number of 1-bits in the given 64-bit value.
     * 
     * @param x The 64-bit value to count the bits of.
     * @return The number of 1-bits in the value.
     */
    static int popcount64(uint64_t x) {
#if defined(__GNUC__) || defined(__clang__)
        return __builtin_popcountll(x);
#elif defined(_MSC_VER)
        return static_cast<int>(__popcnt64(x));
#else
        // Fallback for compilers without built-in popcount
        int count = 0;
        while (x) {
            count += (x & 1);
            x >>= 1;
        }
        return count;
#endif
    }

    /**
     * @brief Counts the number of trailing zeros in the given 64-bit value.
     * 
     * @param x The 64-bit value to count trailing zeros in.
     * @return The number of trailing zeros.
     */
    static unsigned count_trailing_zeros(uint64_t x) {
        if (x == 0) return 64;

#if __cplusplus >= 202002L
        // Use std::countr_zero if C++20 is available
        return std::countr_zero(x);
#elif defined(__GNUC__) || defined(__clang__)
        return __builtin_ctzll(x);
#elif defined(_MSC_VER)
        unsigned long index;
        _BitScanForward64(&index, x);
        return index;
#else
        unsigned count = 0;
        while ((x & 1) == 0) {
            x >>= 1;
            ++count;
        }
        return count;
#endif
    }

    // Static instance counter to keep track of the number of instances created
    static std::atomic<uint64_t> global_instance_counter_;
};

// Static initialization for the counter across all instances
template <uint64_t MaxInstances>
thread_local std::array<uint64_t, MaxInstances> DynamicThreadTicket<MaxInstances>::id_cache_ = [] {
    std::array<uint64_t, MaxInstances> init{};
    init.fill(DynamicThreadTicket<MaxInstances>::INVALID_ID);
    return init;
}();

template<uint64_t MaxInstances>
std::atomic<uint64_t> DynamicThreadTicket<MaxInstances>::global_instance_counter_{0};
