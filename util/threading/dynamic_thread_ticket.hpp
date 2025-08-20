#pragma once

#include <atomic>
#include <stdexcept>
#include <thread>
#include <array>
#include <IStoragePolicy.hpp>

#include <cstdint>
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

template <size_t MaxInstances = DTHREAD_TICKET_MAX>
class DynamicThreadTicket {
public:
    // Constructor takes ownership of the StoragePolicy
    explicit DynamicThreadTicket(std::unique_ptr<IStoragePolicy<std::atomic<uint64_t>>> storage)
        : storage_(std::move(storage))
    {
        instance_id_ = global_instance_counter_.fetch_add(1, std::memory_order_relaxed);
        if (instance_id_ >= MaxInstances) {
            throw std::runtime_error("Too many DynamicThreadTicket instances");
        }

        size_t total_bits = storage_->capacity() * 64;
        if (total_bits == 0) {
            throw std::invalid_argument("StoragePolicy must provide at least one cell");
        }

        // Initialize all bits to 1 (free)
        for (size_t i = 0; i < storage_->capacity(); ++i) {
            storage_->data()[i].store(~uint64_t{0}, std::memory_order_relaxed);
        }

        max_threads_ = total_bits;
    }

    /**
     * @brief acquires a ticket
     * 
     * @note if a first acquire is successful, all the following will use the same cached id until 
     * a `release` is invoked
     */
    bool acquire(size_t& ticket_container) {
        size_t& local_id = get_thread_local_id();
        if (local_id != INVALID_ID) {
            ticket_container = local_id;
            return true;
        }

        const size_t num_cells = (max_threads_ + 63) / 64; // ceil(max_threads_ / 64)

        for (size_t cell = 0; cell < num_cells; ++cell) {
            uint64_t current = storage_->data()[cell].load(std::memory_order_relaxed);
            if (current == 0) continue;

            int bit_pos = count_trailing_zeros(current);
            if (bit_pos < 0) continue;

            size_t ticket = cell * 64 + bit_pos;
            if (ticket >= max_threads_) continue; // Avoid OOB ticket before CAS

            uint64_t mask = uint64_t{1} << bit_pos;
            if (storage_->data()[cell].compare_exchange_weak(current, current & ~mask,
                                                            std::memory_order_acquire,
                                                            std::memory_order_relaxed)) {
                ticket_container = ticket;
                local_id = ticket;
                return true;
            }
            // CAS failed — retry next iteration
        }

        return false; // No free tickets
    }


    /**
     * @brief releases a previously held thread_ticket
     * 
     * @note the method is idempotent
     */
    void release() {
        size_t& local_id = get_thread_local_id();
        if (local_id == INVALID_ID) {
            return;
        }

        size_t cell = local_id / 64;
        size_t bit_pos = local_id % 64;
        uint64_t mask = uint64_t{1} << bit_pos;
        storage_->data()[cell].fetch_or(mask, std::memory_order_release);

        local_id = INVALID_ID;
    }

    size_t active_count() const {
        size_t count = 0;
        size_t total_bits = storage_->capacity() * 64;
        
        // Iterate through each cell in the storage and count active tickets
        for (size_t i = 0; i < storage_->capacity(); ++i) {
            uint64_t current = storage_->data()[i].load(std::memory_order_relaxed);
            count += popcount64(current);
        }

        // Total active tickets should be counted here:
        return total_bits - count;  // subtract the free bits from the total
    }


private:
    static constexpr size_t INVALID_ID = size_t(-1);

    std::unique_ptr<IStoragePolicy<std::atomic<uint64_t>>> storage_;
    size_t instance_id_;
    size_t max_threads_;

    static thread_local std::array<size_t, MaxInstances> id_cache_;

    size_t& get_thread_local_id() {
        return id_cache_[instance_id_];
    }

    static int popcount64(uint64_t x) {
#if defined(__GNUC__) || defined(__clang__)
        return __builtin_popcountll(x);
#elif defined(_MSC_VER)
        return static_cast<int>(__popcnt64(x));
#else
        int count = 0;
        while (x) {
            count += (x & 1);
            x >>= 1;
        }
        return count;
#endif
    }

    static unsigned count_trailing_zeros(uint64_t x) {
        if (x == 0) return 64;

#if __cplusplus >= 202002L
        // Use std::countr_zero if C++20 available
        return std::countr_zero(x);
#elif defined(__GNUC__) || defined(__clang__)
        // GCC/Clang intrinsic
        return __builtin_ctzll(x);
#elif defined(_MSC_VER)
        // MSVC intrinsic
        unsigned long index;
        _BitScanForward64(&index, x);
        return index;
#else
        // Portable fallback loop
        unsigned count = 0;
        while ((x & 1) == 0) {
            x >>= 1;
            ++count;
        }
        return count;
#endif
    }

    static std::atomic<size_t> global_instance_counter_;
};

template <size_t MaxInstances>
thread_local std::array<size_t, MaxInstances> DynamicThreadTicket<MaxInstances>::id_cache_ = [] {
    std::array<size_t, MaxInstances> init{};
    init.fill(DynamicThreadTicket::INVALID_ID);
    return init;
}();

template<size_t MaxInstances>
std::atomic<size_t> DynamicThreadTicket<MaxInstances>::global_instance_counter_{0};
