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

struct LimboBufferOpt {
    struct Auto_FixState{};
};

/**
 * @brief A high-performance, phased, linear MPMC buffer for EBR.
 * * @details
 * Designed for Phased Invariants:
 * 1. Accumulation: Multiple Producers, No Consumers.
 * 2. Reclamation: Multiple Consumers, No Producers.
 * * Uses monotonic indices with a linear layout to maximize cache locality
 * and minimize instruction overhead.
 */
template<size_t Capacity, typename Opt = meta::EmptyOptions>
class LimboBuffer {
public:
    static constexpr Value EMPTY_VAL = 0;
    static constexpr bool AUTO_FIXSTATE = Opt::template has<LimboBufferOpt::Auto_FixState>;

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
            // This isn't strictly sequentially consistent but safe in Phased MPMC
            // because no one is validly using indices >= limit.
            // We use store (not CAS) because if we are >= limit, we just want to stop here.

            if constexpr (AUTO_FIXSTATE) {
                internal_reset_();
            }

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
        if constexpr (!AUTO_FIXSTATE) {
            internal_reset_();
        }
    }



    size_t size() const {
        uint64_t t = tail_.load(std::memory_order_relaxed);
        uint64_t h = head_.load(std::memory_order_relaxed);
        if (t < h) return 0;
        return (t > Capacity ? Capacity : t) - h;
    }

private:
    inline void internal_reset_() {
        if(tail_.load(std::memory_order_relaxed) != 0) {
            tail_.store(0,std::memory_order_release);
        }
        head_.store(0,std::memory_order_release);
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
    static constexpr bool POW2  =
        Opt::template has<CacheOpt::Pow2Size> ||
        (bit::is_pow2(Capacity) && Capacity != 1);

    struct Cell {
        std::atomic<uint64_t> sequence;
        Value data;
    };

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
            buffer[i].sequence.store(i, std::memory_order_relaxed);
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

        // 2. Safety Check (Debug Only)
        // If your assumption that "enqueue cannot fail" is wrong, this asserts.
        // In Release builds, this compiles away completely.
        assert(cell.sequence.load(std::memory_order_relaxed) == tailTicket
               && "Enqueue called on full queue!");

        // 3. Write Data
        // Safe non-atomic write because we 'own' this slot via the ticket
        cell.data = item;

        // 4. Publish Sequence (CRITICAL)
        // We MUST set this to (ticket + 1).
        // The dequeue looks for (ticket + 1) to know the data is ready.
        // memory_order_release ensures the 'cell.data' write is visible
        // before the sequence updates.
        cell.sequence.store(tailTicket + 1, std::memory_order_release);
    }

    // -----------------------------------------------------------
    // DEQUEUE (unchanged logic, handles ABA/Stale reads)
    // -----------------------------------------------------------
    bool dequeue(Value& out) {
        while(true) {
            uint64_t headTicket = head_.load(std::memory_order_relaxed);
            size_t index = mod(headTicket);
            Cell& cell = buffer[index];

            // Load sequence with Acquire to synchronize with the Producer's Release
            uint64_t seq = cell.sequence.load(std::memory_order_acquire);

            // We expect the sequence to be exactly (ticket + 1)
            int64_t diff = (int64_t)seq - (int64_t)(headTicket + 1);

            if (diff == 0) {
                // Success case: Data is ready and sequence matches
                if (head_.compare_exchange_weak(
                        headTicket, headTicket + 1,
                        std::memory_order_relaxed)) {

                    out = cell.data;

                    // Reset sequence for the NEXT generation of producers.
                    // This slot will be used again when tail reaches (ticket + size).
                    cell.sequence.store(headTicket + size_, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                // Sequence is behind. The producer hasn't finished writing yet.
                // Since this is a dequeue, we return false (empty).
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
