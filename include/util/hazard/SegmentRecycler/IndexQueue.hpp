#pragma once

#include <HeapStorage.hpp>  //for internal container
#include <PackedSequencedCell.hpp> //includes the cell <atomic> and <specs.hpp>
#include <limits>
#include <cstdint>
#include <cassert>
#include <algorithm>
#include <random>
#include <thread>

namespace util::hazard {


// #define IDXQUEUE_THRESH 1
/**
 * @brief Lockless queue for `uint32_t` index values
 * 
 * @note based off of LPRQ with modification since CAS2 is not required for single word cells 
 */
template<bool pow_2 = false>
class IndexQueue {
    using Cell = PackedSequencedCell;

public:
    IndexQueue(uint32_t size, uint32_t e_val = MAX) noexcept:
        size_{pow_2? bit::next_power_of_two(size) : size},
        shift_{pow_2? bit::log2(size_) : 0},
        storage_{size_},
        e_val_{e_val} {
        assert((size != 0) && "IndexQueue: size non-nullable");
 
        head_.store(size_,std::memory_order_relaxed);
        tail_.store(size_,std::memory_order_relaxed);

        const uint64_t pack = Cell::pack(0,true,e_val_);
        uint32_t seq, val;
        bool safe;
        Cell::unpack(pack,seq,safe,val);

        for(size_t i = 0; i < size_; i++) {
            storage_[i].pack_.store(pack,std::memory_order_release);
        }
    }

    bool enqueue(uint32_t idx) {
        uint64_t tailTicket;
        uint32_t tailCycle;
        uint32_t tailMod;
        uint64_t c_state, c_new;
        uint32_t c_val, c_seq;
        bool c_safe;
        while(1) {
            tailTicket = tail_.fetch_add(1,std::memory_order_acq_rel);
            if constexpr (pow_2) {
                tailCycle = static_cast<uint32_t>(tailTicket >> shift_);
                tailMod   = static_cast<uint32_t>(tailTicket & (size_ - 1));
            } else {
                tailCycle = static_cast<uint32_t>(tailTicket / size_);  //compute the tailCycle
                tailMod = tailTicket % size_;
            }
            Cell& cell = storage_[tailMod];
            while(1) {
                c_state = cell.pack_.load(std::memory_order_acquire);
                Cell::unpack(c_state,c_seq,c_safe,c_val);
                if(c_state != cell.pack_.load(std::memory_order_acquire))
                    continue;

                /**
                 * enqueues take cells with old sequence numbers and empty
                 * and "renew" their sequence number
                 */
                if( (c_seq < tailCycle) && (c_val == e_val_) &&
                    (c_safe || (head_.load() <= tailTicket))
                ) {
                    c_new = Cell::pack(tailCycle,true,idx);
                    if(!cell.pack_.compare_exchange_strong(
                        c_state,
                        c_new,
                        std::memory_order_acq_rel
                    )) continue;    //inner loop    //reload the same cell [incoherent state]
#ifdef IDXQUEUE_THRESH  // threshold to avoid livelock
                    if(threshold.load(std::memory_order_relaxed) != (3 * size_) - 1)
                        threshold.store((3 * size_) - 1, std::memory_order_release);
#endif
                    return true;
                } else {
                    break;  // inner loop
                }

            }

            if(tailTicket >= head_.load(std::memory_order_acquire) + size_) {
                return false;   //full queue
            }

        }
    }

    bool dequeue(uint32_t& out) {
#ifdef IDXQUEUE_THRESH
        if(threshold.load(std::memory_order_acquire) < 0){
            return false;
        }
#endif
        uint64_t headTicket;
        uint32_t headCycle;
        uint32_t headMod;
        uint64_t c_state, c_new;
        uint32_t c_val, c_seq;
        bool c_safe;

        while(1) {
            headTicket  = head_.fetch_add(1,std::memory_order_acq_rel);
            if constexpr (pow_2) {
                headCycle   = static_cast<uint32_t>(headTicket >> shift_);
                headMod     = static_cast<uint32_t>(headTicket & (size_ - 1));
            } else {
                headCycle   = static_cast<uint32_t>(headTicket / size_);   //compute the headCycle
                headMod     = headTicket % size_;
            }
            Cell& cell = storage_[headMod];
            while(1) {
                c_state = cell.pack_.load(std::memory_order_acquire);
                Cell::unpack(c_state, c_seq, c_safe, c_val);
                if(c_seq == headCycle) {
                    cell.pack_.store(Cell::pack(c_seq,c_safe,e_val_));
                    out = c_val;
                    return true;
                }
                c_new = Cell::pack(c_seq,false,c_val);
                if(c_val == e_val_)
                    c_new = Cell::pack(headCycle,c_safe,e_val_);
                if((c_seq < headCycle)){
                    if(!cell.pack_.compare_exchange_strong(c_state,c_new))
                        continue;
                }
                break;
            }

            uint64_t tail = tail_.load(std::memory_order_acquire);
            if(tail <= headTicket + 1) {
#ifdef IDXQUEUE_THRESH
                threshold.fetch_sub(1,std::memory_order_release);
#endif
                catchup(tail, headTicket + 1);
                return false;
            }
#ifdef IDXQUEUE_THRESH
            if(threshold.fetch_sub(1) <= 0)
                return false;
#endif
        }

    }


private:

    void catchup(uint64_t tail, uint64_t head) {
        while(!tail_.compare_exchange_strong(tail,head)) {
            head = head_.load();
            tail = tail_.load();
            if(tail >= head)
                break;
        }
    }

    static constexpr size_t PAD_SIZE = CACHE_LINE - sizeof(std::atomic<uint32_t>);
    static constexpr uint32_t MAX = std::numeric_limits<uint32_t>::max();
    alignas(CACHE_LINE) std::atomic<uint64_t> tail_; //start at first cycle
    char pad_tail_[PAD_SIZE];
    alignas(CACHE_LINE) std::atomic<uint64_t>  head_; //start at first cycle
    char pad_head_[PAD_SIZE];
#ifdef IDXQUEUE_THRESH
    alignas(CACHE_LINE) std::atomic<int64_t> threshold{-1};
    char pad_thresh_[PAD_SIZE];
#endif
    const size_t size_;      //size of the queue
    const size_t shift_;
    util::memory::HeapStorage<Cell> storage_;
    const uint32_t e_val_;  //empty value
};

class IndexQueueTest {
public:
    IndexQueueTest(uint32_t size)
        : queue_(size),
          insertions_(0),
          extractions_(0) {}

    void testConcurrency() {
        const size_t num_producers = 2;
        const size_t num_consumers = 2;
        const size_t ops_per_thread = 100000;

        // Per-thread results
        std::vector<std::vector<uint32_t>> producer_results(num_producers);
        std::vector<std::vector<uint32_t>> consumer_results(num_consumers);

        // Random number generator seeds
        std::random_device rd;
        std::mt19937 seed_gen(rd());

        // Producer lambda
        auto producer = [this, &producer_results, ops_per_thread](size_t id, uint32_t seed) {
            std::mt19937 rng(seed);
            std::uniform_int_distribution<uint32_t> dist(1, 1000);
            auto& local_vec = producer_results[id];
            local_vec.reserve(ops_per_thread);

            for (size_t i = 0; i < ops_per_thread; ++i) {
                uint32_t val = dist(rng);
                while (!queue_.enqueue(val)) {
                    // spin until enqueued
                }
                local_vec.push_back(val);
                insertions_.fetch_add(1, std::memory_order_relaxed);
            }
        };

        // Consumer lambda
        auto consumer = [this, &consumer_results, ops_per_thread](size_t id) {
            auto& local_vec = consumer_results[id];
            local_vec.reserve(ops_per_thread);
            uint32_t value;

            for (size_t i = 0; i < ops_per_thread; ++i) {
                while (!queue_.dequeue(value)) {
                    std::this_thread::yield();
                }
                local_vec.push_back(value);
                extractions_.fetch_add(1, std::memory_order_relaxed);
            }
        };

        // Launch producers
        std::vector<std::thread> producers;
        for (size_t i = 0; i < num_producers; ++i) {
            uint32_t seed = seed_gen(); // independent seed
            producers.emplace_back(producer, i, seed);
        }

        // Launch consumers
        std::vector<std::thread> consumers;
        for (size_t i = 0; i < num_consumers; ++i) {
            consumers.emplace_back(consumer, i);
        }

        // Join threads
        for (auto& t : producers) t.join();
        for (auto& t : consumers) t.join();

        // Merge producer/consumer results
        std::vector<uint32_t> inserted_values;
        std::vector<uint32_t> extracted_values;
        for (auto& v : producer_results) {
            inserted_values.insert(inserted_values.end(), v.begin(), v.end());
        }
        for (auto& v : consumer_results) {
            extracted_values.insert(extracted_values.end(), v.begin(), v.end());
        }

        // --- Validation ---

        // 1. Counts must match
        assert(inserted_values.size() == extracted_values.size() &&
               "Mismatch between insertions and extractions");

        // 2. Multiset equality (ignoring order)
        std::sort(inserted_values.begin(), inserted_values.end());
        std::sort(extracted_values.begin(), extracted_values.end());
        assert(inserted_values == extracted_values &&
               "Mismatch between inserted and extracted values");

        std::cout << "Test passed: "
                  << insertions_.load() << " inserted, "
                  << extractions_.load() << " extracted, "
                  << "all values matched." << std::endl;
    }

private:
    util::hazard::IndexQueue<true> queue_;
    std::atomic<size_t> insertions_;
    std::atomic<size_t> extractions_;
};


}
