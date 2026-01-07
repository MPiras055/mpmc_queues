#pragma once
#include <cstddef>
#include <cstdint>
#include <specs.hpp>
#include <bit.hpp>
#include <atomic>
#include <cassert>
#include <OptionsPack.hpp>
#include <SequencedCell.hpp>
#include <iostream>

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
                    Value old = node.val.exchange(item,std::memory_order_acq_rel);
                    assert(old == EMPTY && "DebugBucket: overwrite non empty val");
                    node.seq.store(seq + 1,std::memory_order_release);
                    return;
                }
            } else if (tailTicket > seq) {
                std::cout << "TAIL " << tailTicket << " SEQ " << seq << " CAPACITY " << Capacity << "\n";
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
            } else if(diff < 0) {
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
 * Uses a single packed 64-bit atomic state for indices:
 * [ High 32 Bits: Tail (Write Index) | Low 32 Bits: Head (Read Index) ]
 * * Implements "Ping-Pong" Lazy Resets:
 * - Enqueue (Fill Phase): Increments Tail. If Head is dirty, wipes Head.
 * - Dequeue (Drain Phase): Increments Head. If Empty, wipes Tail.
 * * Invariants:
 * 1. Accumulation: Multiple Producers, No Consumers.
 * 2. Reclamation: Multiple Consumers, No Producers.
 * 3. Bucket never overfills (Capacity guaranteed by caller).
 */
template<size_t Capacity>
class PhasedBucket {
public:
    // Defined as uintptr_t to hold pointers or integers
    using Value = std::uintptr_t;

    // Sentinel value for empty slots
    static constexpr Value EMPTY_VAL = static_cast<Value>(Capacity);

    // -----------------------------------------------------------
    // PACKING CONSTANTS (Bit Shift Masks)
    // -----------------------------------------------------------
    static constexpr uint64_t ONE = 1ULL;
    static constexpr uint64_t SHIFT = 32;

    // Head (Low Bits)
    static constexpr uint64_t HEAD_INC  = ONE;
    static constexpr uint64_t HEAD_MASK = (ONE << SHIFT) - 1;

    // Tail (High Bits)
    static constexpr uint64_t TAIL_INC  = ONE << SHIFT;
    static constexpr uint64_t TAIL_MASK = ~HEAD_MASK;

    // -----------------------------------------------------------
    // SAFETY CHECKS
    // -----------------------------------------------------------
    static_assert(Capacity < (1ULL << 32), "PhasedBucket: Capacity must fit in 32 bits");
    static_assert(std::atomic<uint64_t>::is_always_lock_free, "PhasedBucket: uint64_t atomics must be lock-free");

    explicit PhasedBucket() {
        // Initialize state to 0 (Head=0, Tail=0)
        state_.store(0, std::memory_order_relaxed);

        for(size_t i = 0; i < Capacity; i++) {
            buffer_[i].store(EMPTY_VAL, std::memory_order_relaxed);
        }
    }

    ~PhasedBucket() = default;

    /**
     * @brief Enqueues an item using wait-free synchronization and lazy reset.
     * * @details
     * Fetches and adds to the Tail (High bits).
     * If it detects the Low bits (Head) are non-zero (dirty from previous drain),
     * it atomically wipes them to 0 using fetch_and.
     */
    void enqueue(Value item) {
        assert(item != EMPTY_VAL && "PhasedBucket: Cannot enqueue EMPTY_VAL sentinel");

        // 1. Reserve slot (Wait-Free): Increment High Bits (Tail)
        //    'old_state' captures the index BEFORE our increment.
        uint64_t old_state = state_.fetch_add(TAIL_INC, std::memory_order_acq_rel);

        // 2. Extract Indices
        //    Shift down High bits to get the integer index
        uint32_t tail_idx = static_cast<uint32_t>(old_state >> SHIFT);

        // 3. Lazy Reset Logic (The "Ping" Check)
        //    Check if Low Bits (Head) are dirty from the previous phase
        if (old_state & HEAD_MASK) {
            // We detected dirty Head bits. Wipe them.
            // Use bitwise AND with TAIL_MASK (111..000) to clear low bits only.
            state_.fetch_and(TAIL_MASK, std::memory_order_relaxed);
        }

        // 4. Bounds Check
        assert(tail_idx < Capacity && "PhasedBucket: Violation of Phased Invariant (BufferOverflow)");

        // 5. Write Data
#ifndef NDEBUG
        // Debug: Verify no overwrites (Phased Invariant check)
        Value old_val = buffer_[tail_idx].exchange(item, std::memory_order_acq_rel);
        assert(old_val == EMPTY_VAL && "PhasedBucket: Violation of Phased Invariant (Producer collision or Dirty Buffer)");
#else
        // Release: Fast store (Wait-Free)
        buffer_[tail_idx].store(item, std::memory_order_release);
#endif
    }

    /*
    * @brief Dequeues an item using wait-free synchronization and lazy reset.
    * * @details
    * Fetches and adds to the Head (Low bits).
    * If Head >= Tail (Empty), it atomically wipes the Tail (High bits)
    * to 0 using fetch_and, preparing for the next Enqueue phase.
    */
    [[nodiscard]] bool dequeue(Value& out) {
        // 1. Reserve slot (Wait-Free): Increment Low Bits (Head)
        uint64_t old_state = state_.fetch_add(HEAD_INC, std::memory_order_acq_rel);

        // 2. Extract Indices
        uint32_t head_idx = static_cast<uint32_t>(old_state & HEAD_MASK);

        // 3. Fast Path: Physical Bounds Check & Sentinel Access.
        if (head_idx < Capacity) {
            // Retrieve Data and Clear Slot
            Value val = buffer_[head_idx].exchange(EMPTY_VAL, std::memory_order_relaxed);

            // If val is not EMPTY_VAL, we found data.
            if (val != EMPTY_VAL) {
                out = val;
                return true;
            }
        }

        // 4. Slow Path: Empty or Overflow (Head >= Tail or Head >= Capacity)
        //    We detected the queue is empty or exhausted.

        // Lazy Reset Logic (The "Pong" Check):
        // Prepare for the NEXT Enqueue phase by wiping the Tail (High bits).
        // Only fetch_and if High bits are actually dirty (non-zero).
        if (old_state & TAIL_MASK) {
            // Wipe High Bits (Tail), Keep Low Bits (Head)
            state_.fetch_and(HEAD_MASK, std::memory_order_release);
        }

        return false;
    }


    /**
     * @brief Hard reset of the bucket.
     * Can be used to manually flip phases or initialize.
     */
    inline void reset() {
        state_.store(0, std::memory_order_release);
    }

    /**
     * @brief Returns the number of items currently available to dequeue.
     */
    size_t size() const {
        uint64_t s = state_.load(std::memory_order_relaxed);
        uint32_t t = static_cast<uint32_t>(s >> SHIFT);
        uint32_t h = static_cast<uint32_t>(s & HEAD_MASK);

        if (t < h) return 0;
        return (t > Capacity ? Capacity : t) - h;
    }

private:
    // Single Packed State: [ Tail (32) | Head (32) ]
    // Aligned to cache line to prevent false sharing with other buckets
    ALIGNED_CACHE std::atomic<uint64_t> state_;

    // Aligning the buffer start effectively pads the previous member (state_)
    // to the cache line boundary, preventing false sharing between state_ and buffer_.
    ALIGNED_CACHE std::atomic<Value> buffer_[Capacity];
};

struct CacheOpt {
    struct Pow2Size{};
    struct DisablePad{};
};

template<size_t Capacity, typename Opt = meta::EmptyOptions>
class Cache {
    static constexpr Value EMPTY = Capacity;
    static constexpr bool POW2  =
        Opt::template has<CacheOpt::Pow2Size> ||
        (bit::is_pow2(Capacity) && Capacity != 1);
    static constexpr bool PAD   = !Opt::template has<CacheOpt::DisablePad>;

    using Cell = cell::SequencedCell<Value,PAD>;

    size_t mod(uint64_t i) const {
        if constexpr (POW2) {
            return i & (size_ - 1);
        } else {
            return i % size_;
        }
    }

public:
    Cache(): buffer{new Cell[size_]} {
        for(size_t i = 0; i < size_; i++) {
            // Initial sequence must match the initial ticket indices (0, 1, 2...)
            // buffer[i].sequence.store(i, std::memory_order_relaxed);
#ifndef NDEBUG
            buffer[i].val.store(EMPTY,std::memory_order_relaxed);
#endif
            buffer[i].seq.store(i,std::memory_order_relaxed);
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
#ifndef NDEBUG
        Value old = cell.val.exchange(item,std::memory_order_relaxed);
        assert(old == EMPTY && "Cache: enqueue (cell non-empty)");
#else
        cell.val.store(item,std::memory_order_release);
#endif

        // 4. Publish Sequence (CRITICAL)
        // We MUST set this to (ticket + 1).
        // The dequeue looks for (ticket + 1) to know the data is ready.
        // memory_order_release ensures the 'cell.data' write is visible
        // before the sequence updates.
        cell.seq.store(tailTicket + 1, std::memory_order_release);
    }

    // -----------------------------------------------------------
    // DEQUEUE (unchanged logic, handles ABA/Stale reads)
    // -----------------------------------------------------------
    bool dequeue(Value& out) {
        uint64_t headTicket, seq;
        size_t index;
        while(true) {
            headTicket = head_.load(std::memory_order_relaxed);
            index = mod(headTicket);
            Cell& node = buffer[index];
            seq = node.seq.load(std::memory_order_acquire);
            int64_t diff = static_cast<int64_t>(seq) - static_cast<int64_t>(headTicket + 1);
            if(diff == 0) {
                if(head_.compare_exchange_weak(
                    headTicket,headTicket + 1,
                    std::memory_order_relaxed
                )) {
#ifndef NDEBUG
                    out = node.val.exchange(EMPTY,std::memory_order_acq_rel);
                    assert(out != EMPTY && "DebugBuffer: extracted empty val");
#else
                    out = node.val.load(std::memory_order_acquire);
#endif
                    node.seq.store(headTicket + Capacity, std::memory_order_release);
                    return true;
                }
            } else if(diff < 0) {
                return false;
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
