#pragma once

#include <atomic>
#include <array>
#include <stdexcept>

#ifndef STHREAD_TICKET_MAX
#define STHREAD_TICKET_MAX 128
#endif

/**
 * Used in linked queues to associate to every thread a specified id. The id is thread-local
 * cached per instance of StaticThreadTicket
 */
template <size_t MaxInstances = STHREAD_TICKET_MAX>
class StaticThreadTicket {
public:
    explicit StaticThreadTicket(size_t max_threads)
        : max_threads_(max_threads)
    {
        instance_id_ = global_instance_counter_.fetch_add(1, std::memory_order_relaxed);
        if (instance_id_ >= MaxInstances) {
            throw std::runtime_error("Too many StaticThreadTicket instances");
        }
    }

    size_t get_id() {
        //every thread caches the whole array
        thread_local std::array<size_t, MaxInstances> id_cache = [] {
            std::array<size_t, MaxInstances> init{};
            init.fill(INVALID_ID);
            return init;
        }();

        size_t& local_id = id_cache[instance_id_];
        if (local_id != INVALID_ID) return local_id;

        size_t assigned = counter_.fetch_add(1, std::memory_order_relaxed);
        if (assigned >= max_threads_) {
            throw std::runtime_error("Exceeded max threads for this StaticThreadTicket instance");
        }

        local_id = assigned;
        return assigned;
    }

private:
    static constexpr size_t INVALID_ID = 0;

    size_t instance_id_;                   // unique ID for this instance
    size_t max_threads_;                   // max threads allowed for this instance
    std::atomic<size_t> counter_{1};      // counter for thread IDs issued [1-index based]

    static std::atomic<size_t> global_instance_counter_;
};

template <size_t MaxInstances>
std::atomic<size_t> StaticThreadTicket<MaxInstances>::global_instance_counter_{0};
