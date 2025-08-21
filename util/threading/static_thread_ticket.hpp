#pragma once

#include <atomic>
#include <stdexcept>
#include <limits>

/**
 * @brief Associates a unique thread ID for each thread using a static counter.
 * 
 * The `StaticThreadTicket` class provides a mechanism to assign a unique ID to each thread.
 * This ID is cached locally for each thread to avoid recalculating the ID every time it's requested.
 * The ID is generated based on a static counter, which increments for each new thread that calls `get_id()`.
 * The counter will not wrap around due to the use of `std::numeric_limits<uint64_t>::max()`, ensuring a virtually
 * unlimited number of threads can be assigned unique IDs.
 */
class StaticThreadTicket {
public:
    /**
     * @brief Returns the unique thread ID for the current thread.
     * 
     * @return The thread ID assigned to the current thread.
     * @throws std::runtime_error if the maximum number of thread IDs is exceeded.
     */
    uint64_t get_id() {
        // Cache thread-local ID for this instance
        thread_local uint64_t local_id = INVALID_ID;
        
        // If the ID is already cached, return it
        if (local_id != INVALID_ID) return local_id;

        // Generate a new unique ID for the current thread
        uint64_t assigned = counter_.fetch_add(1, std::memory_order_relaxed);

        // Check if we exceeded the maximum possible ID count (std::numeric_limits<uint64_t>::max())
        if (assigned == std::numeric_limits<uint64_t>::max()) {
            throw std::runtime_error("Exceeded maximum number of unique thread IDs.");
        }

        // Cache the assigned ID for this thread
        local_id = assigned;
        return assigned;
    }

private:
    static constexpr uint64_t INVALID_ID = std::numeric_limits<uint64_t>::max();  // ID representing an invalid or unassigned ID

    static std::atomic<uint64_t> counter_;  // Static counter to generate unique thread IDs
};

// Static initialization for the counter across all instances
std::atomic<uint64_t> StaticThreadTicket::counter_{1};
