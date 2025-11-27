#pragma once
#include <cstddef>
#include <cstdint>
#include <specs.hpp>
#include <bit.hpp>
#include <atomic>
#include <cassert>
#include <OptionsPack.hpp>
#include <SequencedCell.hpp>

namespace util::hazard::recycler::details {

using Value = uint64_t;

/// ===========================================
/// Plain-old CASLoop (enqueue/dequeue) bucket
/// ===========================================
///
template<size_t Capacity, typename Opt = meta::EmptyOptions>
class DebugBucket {
    using Cell = cell::SequencedCell<Value, true>;
    static constexpr Value EMPTY = Capacity;
    inline size_t mod(uint64_t i) const noexcept {
        return i % Capacity;
    }

    public:

    explicit DebugBucket() {
        for(size_t i = 0; i < Capacity; i++) {
            buffer[mod(i)].seq.store(i,std::memory_order_relaxed);
            buffer[mod(i)].val.store(EMPTY,std::memory_order_relaxed);
        }
    }
    void enqueue(Value item) {
        uint64_t tailTicket,seq;
        size_t index;

        while(true){
            tailTicket = tail.load(std::memory_order_relaxed);
            index = mod(tailTicket);
            Cell& node = buffer[index];
            seq = node.seq.load(std::memory_order_acquire);

            if(tailTicket == seq) {
                bool success = tail.compare_exchange_weak(
                    tailTicket,
                    (tailTicket + 1),
                    std::memory_order_relaxed
                );
                if(success) {
                    node.val.store(item,std::memory_order_relaxed);
                    node.seq.store(seq + 1,std::memory_order_release);
                    return;
                }
            } else if (tailTicket > seq) {
                assert(false && "Full debugBucket");
            }
        }
    }

    bool dequeue(Value& out) {
        uint64_t headTicket, seq;
        size_t index;
        while(true) {
            headTicket = head.load(std::memory_order_relaxed);
            index = mod(headTicket);
            Cell& node = buffer[index];
            seq = node.seq.load(std::memory_order_acquire);
            int64_t diff = static_cast<int64_t>(seq) - static_cast<int64_t>(headTicket + 1);
            if(diff == 0) {
                if(head.compare_exchange_weak(
                    headTicket,headTicket + 1,
                    std::memory_order_relaxed
                )) {
                    out = node.val.exchange(EMPTY,std::memory_order_acq_rel);
                    assert(out != EMPTY && "DebugBuffer: extracted empty val");
                    node.seq.store(headTicket + Capacity, std::memory_order_release);
                    return true;
                }
            } else if(diff < 0 ) {
                return false;
            }
        }
    }


    private:
    std::atomic_uint64_t tail{0};
    std::atomic_uint64_t head{0};
    Cell buffer[Capacity];


};

/**
 * @brief A high-performance, phased, wait-free, linear MPMC buffer for EBR.
 * * @details
 * Designed for Phased Invariants:
 * 1. Accumulation: Multiple Producers, No Consumers.
 * 2. Reclamation: Multiple Consumers, No Producers.
 * * Uses monotonic indices with a linear layout to maximize cache locality
 * and minimize instruction overhead.
 *
 * Assumption:
 * 1. Bucket never overfills
 * 2. It's used in phases: either MP-NC or NP-MC (reset() to flip the phase)
 */
template<size_t Capacity>
class LimboBuffer {
public:
    static constexpr Value EMPTY_VAL = Capacity;

    explicit LimboBuffer() {
        for(size_t i = 0; i < Capacity; i++) {
            buffer_[i].store(EMPTY_VAL, std::memory_order_relaxed);
        }
    }

    ~LimboBuffer() = default;

    /**
     * @brief Enqueues an item using wait-free synchronization.
     */
    void enqueue(Value item) {
        assert(item < Capacity && "LimboBuffer: Cannot enqueue nullptr/0");

        // 1. Reserve slot (Wait-Free)
        uint64_t idx = tail_.fetch_add(1, std::memory_order_relaxed);

        // 2. Bounds check
        assert(idx < Capacity && "LimboBuffer: Violation of Phased Invarian (BufferOverflow)");

        // 3. Write Data
#ifndef NDEBUG
        // Debug: Verify no overwrites (Phased Invariant check)
        Value old = buffer_[idx].exchange(item, std::memory_order_acq_rel);
        assert(old == EMPTY_VAL && "LimboBuffer: Violation of Phased Invariant (Producer collision or Dirty Buffer)");
#else
        // Release: Fast store (Wait-Free)
        buffer_[idx].store(item, std::memory_order_release);
#endif
    }

    /**
     * @brief Dequeues an item using wait-free synchronization.
     * * @details
     * Uses fetch_add on head. If the buffer is empty (out of bounds),
     * it effectively "fixes" the state by clamping head to tail to prevent
     * indefinite growth of the head index.
     */
    [[nodiscard]] bool dequeue(Value& out) {
        // 1. Reserve slot (Wait-Free)
        uint64_t idx = head_.fetch_add(1, std::memory_order_relaxed);

        // 2. Bounds Check vs Tail (Where producers stopped)
        // Acquire guarantees we see the data writes from the producer phase.
        uint64_t limit = tail_.load(std::memory_order_acquire);

        if (idx >= limit) {
            // Empty / Overshot
            // Fix State: Clamp head to limit to prevent runaway index.

            return false;
        }

        // 3. Retrieve Data and Clear Slot
        // Uses exchange to grab value and reset slot for the next epoch reuse.
        Value val = buffer_[idx].exchange(EMPTY_VAL, std::memory_order_acq_rel);

        // Logic Check: In a correct Phased system with barriers, data MUST be visible.
        // If val is EMPTY_VAL, it means:
        // a) A Producer crashed after fetch_add but before store.
        // b) The Phase Transition barrier was missing/weak.
        assert(val != EMPTY_VAL && "LimboBuffer: Violation of Phased Invariant (Slot empty on dequeue)");

        out = val;
        return true;
    }

    inline void reset() {
        tail_.store(0,std::memory_order_release);
        head_.store(0,std::memory_order_release);
    }



    size_t size() const {
        uint64_t t = tail_.load(std::memory_order_relaxed);
        uint64_t h = head_.load(std::memory_order_relaxed);
        if (t < h) return 0;
        return (t > Capacity ? Capacity : t) - h;
    }

private:
    inline void internal_reset_() {
        tail_.store(0,std::memory_order_relaxed);
        head_.store(0,std::memory_order_relaxed);
    }

    // Align to cache lines to prevent false sharing
    ALIGNED_CACHE std::atomic<uint64_t> tail_{0};
    CACHE_PAD_TYPES(std::atomic_uint64_t);
    ALIGNED_CACHE std::atomic<uint64_t> head_{0};
    CACHE_PAD_TYPES(std::atomic_uint64_t);
    std::atomic<Value> buffer_[Capacity];
};

struct CacheOpt {
    struct Pow2Size{};
};

template<size_t Capacity, typename Opt = meta::EmptyOptions>
class Cache {
    static constexpr Value EMPTY = Capacity;
    static constexpr bool POW2  =
        Opt::template has<CacheOpt::Pow2Size> ||
        (bit::is_pow2(Capacity) && Capacity != 1);

    // struct Cell {
    //     std::atomic<uint64_t> sequence;
    //     Value data;
    // };
    //
    using Cell = std::atomic<Value>;

    size_t mod(uint64_t i) const {
        if constexpr (POW2) {
            return i & (size_ - 1);
        } else {
            return i % size_;
        }
    }

public:
    Cache() {
        buffer = new Cell[size_];
        for(size_t i = 0; i < size_; i++) {
            // Initial sequence must match the initial ticket indices (0, 1, 2...)
            // buffer[i].sequence.store(i, std::memory_order_relaxed);
            buffer[i].store(EMPTY,std::memory_order_release);
        }
    }

    ~Cache() {
        delete[] buffer;
    }

    // -----------------------------------------------------------
    // OPTIMIZED ENQUEUE (No Spin)
    // Assumption: Queue is never full.
    // -----------------------------------------------------------
    void enqueue(Value item) {
        // 1. Reserve ticket
        // relaxed is fine here because the 'release' on sequence
        // will order everything correctly relative to the consumer.
        uint64_t tailTicket = tail_.fetch_add(1, std::memory_order_relaxed);

        size_t index = mod(tailTicket);
        Cell& cell = buffer[index];

        // 3. Write Data
        // Safe non-atomic write because we 'own' this slot via the ticket
        cell.store(item,std::memory_order_release);

        // 4. Publish Sequence (CRITICAL)
        // We MUST set this to (ticket + 1).
        // The dequeue looks for (ticket + 1) to know the data is ready.
        // memory_order_release ensures the 'cell.data' write is visible
        // before the sequence updates.
        // cell.sequence.store(tailTicket + 1, std::memory_order_release);
    }

    // -----------------------------------------------------------
    // DEQUEUE (unchanged logic, handles ABA/Stale reads)
    // -----------------------------------------------------------
    bool dequeue(Value& out) {
        uint64_t headTicket = head_.load(std::memory_order_acquire);
        while(true) {
            size_t index = mod(headTicket);
            Cell& cell = buffer[index];
            Value item = cell.load(std::memory_order_acquire);
            if(item == EMPTY)
                return false;
            //if fails then resets the headTicket
            else if(head_.compare_exchange_weak(
                headTicket,headTicket + 1,
                std::memory_order_relaxed
            )) {
#ifdef NDEBUG
                cell.store(EMPTY,std::memory_order_release);
#else
                Value cmp = cell.exchange(EMPTY,std::memory_order_acq_rel);
                assert(item == cmp && "CacheBucket: store invariant violated");
#endif
                out = item;
                return true;
            }
        }
    }

private:
    const size_t size_ = []() consteval {
        if constexpr (POW2 &&
            (!bit::is_pow2(Capacity) || (Capacity == 1))
        ) {
            return bit::next_pow2(Capacity);
        } else {
            return Capacity;
        }
    }();

    ALIGNED_CACHE std::atomic_uint64_t tail_{0};
    ALIGNED_CACHE std::atomic_uint64_t head_{0};
    Cell* buffer;
};

}   //namespace util::hazard::recycler
