#pragma once
/**
 * @file Buffer.hpp
 * @brief Single-producer / single-consumer buffers: a circular one and a linear one.
 * @ingroup spsc
 *
 * One protocol for both, the wait-free SWSR pointer buffer: **one atomic word per cell, and no
 * shared index at all.** `T{}` means empty.
 *
 * ```
 * producer: if cell[w] != empty -> full        consumer: if cell[r] == empty -> empty
 *           cell[w].store(v, release); ++w               out = cell[r]; cell[r] = empty; ++r
 * ```
 *
 * `w` and `r` are private to their side. A Lamport ring ping-pongs a shared head and tail between
 * two cores on every operation; this one never touches a word the other side writes except the
 * cell being handed over.
 *
 * ### The restriction: `T{}` is reserved
 *
 * A cell holding `T{}` is empty, so `T{}` cannot be enqueued -- in this tree's vocabulary these
 * buffers have `can_store_null == false`. Every item the harness sends is a non-null pointer, so
 * this costs nothing there; it is asserted at the enqueue, because a null that slipped through
 * would be silently swallowed rather than rejected.
 *
 * ### Where the indices live
 *
 * `w_` and `r_` are each alone on a cache line and written `relaxed` by their owner only -- the
 * cell store already carries the release. They exist so size() can read `w - r` without
 * scanning cells, and so a caller that keeps its own private copy (block::AllToAll's handles do)
 * can resume from them. put() / take() work on the caller's copy; enqueue() / dequeue() are the
 * self-contained form that loads the owner's own word, which is on a line only that owner writes.
 *
 * Both buffers **adopt externally placed cells**, the algo::LFring idiom: the caller owns the
 * storage, the buffer initialises it. That is what lets block::AllToAll put all of its buffers'
 * cells in one arena.
 */

#include <cell/PlainCell.hpp>
#include <util/align.hpp>
#include <util/bit.hpp>
#include <util/specs.hpp>
#include <atomic>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <new>
#include <type_traits>

namespace spsc {

/// What a cell can carry: one lock-free word, compared against the reserved empty value `T{}`.
template <typename T>
concept Storable = std::is_trivially_copyable_v<T> && std::default_initializable<T> &&
                   std::equality_comparable<T> && std::atomic<T>::is_always_lock_free;

/**
 * @brief Circular SPSC buffer. Capacity is a power of two; cells are cache-line padded.
 *
 * Padded because producer and consumer chase each other around the ring, so adjacent cells are
 * genuinely contended -- the same reasoning that keeps `algo::CacheRingOpt::no_cell_padding` off
 * by default.
 */
template <Storable T>
class Ring {
public:
    using value_type = T;
    using cell_type = cell::PlainCell<T, true>;
    static constexpr bool circular = true;

    /// @return the capacity a ring asked for @p n items actually has.
    static constexpr std::size_t capacity_for(std::size_t n) noexcept {
        return bit::next_pow2(n == 0 ? std::size_t{1} : n);
    }

    /// @pre @p capacity is a power of two and @p cells holds at least that many cells.
    Ring(std::size_t capacity, cell_type* cells) noexcept : cells_{cells}, mask_{capacity - 1} {
        assert(bit::is_pow2(capacity) && "spsc::Ring: capacity must be a power of two");
        for (std::size_t i = 0; i < capacity; ++i) ::new (static_cast<void*>(cells + i)) cell_type{};
    }

    Ring(const Ring&) = delete;
    Ring& operator=(const Ring&) = delete;

    /// @name Producer side, with the caller's private cursor
    /// @{
    FORCE_INLINE bool put(std::size_t& w, T v) noexcept {
        assert(v != T{} && "spsc::Ring: T{} is the empty marker and cannot be enqueued");
        std::atomic<T>& c = cells_[w & mask_].val;
        // Relaxed is enough: the cell is atomic, so read-write coherence already orders the
        // consumer's read of the previous value before this overwrite.
        if (c.load(std::memory_order_relaxed) != T{}) return false;
        c.store(v, std::memory_order_release);
        w_.store(++w, std::memory_order_relaxed);
        return true;
    }
    /// @}

    /// @name Consumer side, with the caller's private cursor
    /// @{
    FORCE_INLINE bool take(std::size_t& r, T& out) noexcept {
        std::atomic<T>& c = cells_[r & mask_].val;
        const T v = c.load(std::memory_order_acquire);
        if (v == T{}) return false;
        c.store(T{}, std::memory_order_release);
        out = v;
        r_.store(++r, std::memory_order_relaxed);
        return true;
    }
    /// @}

    /// Self-contained forms: the owner's cursor is read back from its own line.
    bool enqueue(T v) noexcept {
        std::size_t w = w_.load(std::memory_order_relaxed);
        return put(w, v);
    }
    bool dequeue(T& out) noexcept {
        std::size_t r = r_.load(std::memory_order_relaxed);
        return take(r, out);
    }

    /// Published cursors, for a handle resuming on this buffer.
    std::size_t write_index() const noexcept { return w_.load(std::memory_order_relaxed); }
    std::size_t read_index() const noexcept { return r_.load(std::memory_order_relaxed); }

    /// Approximate under concurrency; exact when quiescent.
    std::size_t size() const noexcept {
        const std::size_t r = r_.load(std::memory_order_relaxed);
        const std::size_t w = w_.load(std::memory_order_relaxed);
        return w > r ? w - r : 0;
    }
    std::size_t capacity() const noexcept { return mask_ + 1; }
    const cell_type* cells() const noexcept { return cells_; }

private:
    cell_type* cells_;
    std::size_t mask_;
    CACHE_LINE_MEMBER(std::atomic<std::size_t>, w_, {0});
    CACHE_LINE_MEMBER(std::atomic<std::size_t>, r_, {0});
};

/**
 * @brief Non-circular SPSC buffer: fills once, then refuses every enqueue. Cells are packed.
 *
 * Unpadded because producer and consumer do not chase each other here: they sweep the buffer
 * once, in the same direction, so the only line they can share is the moving frontier. Padding
 * every cell would cost `CACHE_LINE / sizeof(T)` times the memory to avoid that one line.
 *
 * Because no cell is ever written twice, the consumer does not clear what it reads -- which also
 * keeps it off the frontier line entirely. reset() restores a quiescent buffer for reuse
 * (spsc::Unbounded recycles its chunks through it).
 */
template <Storable T>
class Linear {
public:
    using value_type = T;
    using cell_type = cell::PlainCell<T, false>;
    static constexpr bool circular = false;

    static constexpr std::size_t capacity_for(std::size_t n) noexcept { return n == 0 ? 1 : n; }

    /// @pre @p cells holds at least @p capacity cells.
    Linear(std::size_t capacity, cell_type* cells) noexcept : cells_{cells}, capacity_{capacity} {
        assert(capacity != 0 && "spsc::Linear: capacity must be non-zero");
        for (std::size_t i = 0; i < capacity; ++i) ::new (static_cast<void*>(cells + i)) cell_type{};
    }

    Linear(const Linear&) = delete;
    Linear& operator=(const Linear&) = delete;

    FORCE_INLINE bool put(std::size_t& w, T v) noexcept {
        assert(v != T{} && "spsc::Linear: T{} is the empty marker and cannot be enqueued");
        if (w == capacity_) return false;
        cells_[w].val.store(v, std::memory_order_release);
        w_.store(++w, std::memory_order_relaxed);
        return true;
    }

    FORCE_INLINE bool take(std::size_t& r, T& out) noexcept {
        if (r == capacity_) return false;
        const T v = cells_[r].val.load(std::memory_order_acquire);
        if (v == T{}) return false;
        out = v;
        r_.store(++r, std::memory_order_relaxed);
        return true;
    }

    bool enqueue(T v) noexcept {
        std::size_t w = w_.load(std::memory_order_relaxed);
        return put(w, v);
    }
    bool dequeue(T& out) noexcept {
        std::size_t r = r_.load(std::memory_order_relaxed);
        return take(r, out);
    }

    /**
     * @brief The consumer has read every cell. Consumer-side only.
     *
     * Implies the producer wrote every cell, so it is done with this buffer for good -- the fact
     * spsc::Unbounded frees chunks on.
     */
    bool drained() const noexcept { return r_.load(std::memory_order_relaxed) == capacity_; }

    /// Make a drained buffer reusable. @pre neither side is using it.
    void reset() noexcept {
        for (std::size_t i = 0; i < capacity_; ++i)
            cells_[i].val.store(T{}, std::memory_order_relaxed);
        w_.store(0, std::memory_order_relaxed);
        r_.store(0, std::memory_order_relaxed);
    }

    std::size_t write_index() const noexcept { return w_.load(std::memory_order_relaxed); }
    std::size_t read_index() const noexcept { return r_.load(std::memory_order_relaxed); }

    std::size_t size() const noexcept {
        const std::size_t r = r_.load(std::memory_order_relaxed);
        const std::size_t w = w_.load(std::memory_order_relaxed);
        return w > r ? w - r : 0;
    }
    std::size_t capacity() const noexcept { return capacity_; }
    const cell_type* cells() const noexcept { return cells_; }

private:
    cell_type* cells_;
    std::size_t capacity_;
    CACHE_LINE_MEMBER(std::atomic<std::size_t>, w_, {0});
    CACHE_LINE_MEMBER(std::atomic<std::size_t>, r_, {0});
};

} // namespace spsc
