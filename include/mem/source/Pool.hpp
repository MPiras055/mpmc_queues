#pragma once
#include <core/SegmentTraits.hpp>
#include <mem/Handle.hpp>
#include <mem/SingleBlock.hpp>
#include <mem/detail/RingSlab.hpp>
#include <util/bit.hpp>
#include <util/specs.hpp>
#include <util/threading/DynamicThreadTicket.hpp>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <optional>
#include <vector>

namespace mem::source {

/**
 * @brief Segment source backed by a fixed, epoch-recycled pool.
 *
 * Models core::SegmentSource. Segments are pre-allocated once and reused; `acquire()`
 * returns nullopt when none is free, and **that is the memory bound**. This is why
 * BoundedMemProxy no longer exists as a separate proxy: its ceiling was never an
 * admission rule, it was a source that runs out.
 *
 * Handles are VersionedIndex rather than pointers. A pool slot is reused, so a bare index
 * would let a stale `next` value alias a live segment (ABA); the version half makes reuse
 * observable.
 *
 * ## Reclamation
 *
 * Three limbo buckets and one free list. A retirement lands in `limbo[e]` for the epoch
 * current at the time. When the epoch advances to `e'`, everything in `limbo[e' + 1]` --
 * the same bucket as `e' - 2`, i.e. retired two epochs ago -- is drained into the free
 * list, and only then can be handed out again.
 *
 * A pinned thread publishes the epoch it saw, and the epoch may advance only when every
 * pinned thread published the current one. That is what makes "two epochs ago" mean "no
 * live reader can still hold it".
 *
 * @note The free list is a *separate* ring rather than whichever limbo bucket happens to
 *       be oldest. Reusing a limbo bucket as the free list looks equivalent and is not:
 *       if the epoch moves past the moment that bucket is the free one -- which happens
 *       readily, since acquire() advances the epoch itself when it finds nothing -- its
 *       contents are stranded until the rotation comes round again. Draining on advance
 *       means an index is reusable from the moment it is safe, and stays reusable.
 *
 * @note This owns its reclamation outright. It used to delegate to a `Recycler` that
 *       carried its own ticketing, per-thread metadata, a reuse cache and a lookup-table
 *       template parameter, of which the proxy used almost nothing — the proxy keeps its
 *       own per-thread array, so the metadata plumbing was dead weight. The one part
 *       worth keeping was the lock-free index container, and that is `algo::LFring` via
 *       mem::detail::RingSlab, reused here rather than rewritten.
 */
template <typename S, std::size_t N>
class Pool {
    /**
     * A pooled segment is handed back out after it has held items, so it must be
     * resettable. Without this, pairing the pool with a write-once segment such as
     * FAAArray or HQ would compile and then fail at run time on the first reuse -- which
     * is exactly how the old `assert(false && "TODO")` in open() behaved.
     */
    static_assert(core::segment_traits<S>::recyclable,
                  "mem::source::Pool reuses segments, but segment_traits<S>::recyclable is "
                  "false: this segment cannot be reopened after being drained");
    static_assert(N >= 2, "a pool needs at least two segments to make progress");

    using Ticketing = util::threading::DynamicThreadTicket;

    /// Limbo buckets. Three is the minimum that distinguishes current / grace / safe.
    static constexpr std::size_t kStages = 3;
    /// One more ring holds everything currently reusable.
    static constexpr std::size_t kFreeBucket = kStages;

    /// Per-thread published epoch. MSB set means pinned; the low bits are the epoch.
    struct ALIGNED_CACHE Slot {
        std::atomic<uint64_t> state{0};
        CACHE_PAD_TYPES(std::atomic<uint64_t>);
    };

public:
    using handle = mem::VersionedIndex;

    /// Segments come back from the pool dirty; the proxy must reopen() before reuse.
    static constexpr bool recycles = true;

    static constexpr handle nil() noexcept { return handle{}; }

    /**
     * @brief RAII epoch pin.
     *
     * Protection is epoch-wide rather than per-object, so `protect()` has nothing to
     * publish — holding the pin is what keeps every slot alive.
     */
    class guard {
        Pool* owner_;
        std::size_t tid_;

    public:
        guard(Pool* o, std::size_t tid) noexcept : owner_{o}, tid_{tid} {}
        guard(const guard&) = delete;
        guard& operator=(const guard&) = delete;
        ~guard() { owner_->unpin(tid_); }

        std::size_t tid() const noexcept { return tid_; }
    };

    Pool(std::size_t max_threads, std::size_t segment_capacity)
        : max_threads_{max_threads}, tickets_{max_threads}, threads_(max_threads),
          buckets_{kStages + 1, N}, versions_(N), segments_(N) {
        assert(max_threads != 0);
        assert(segment_capacity != 0);

        for (std::size_t i = 0; i < N; ++i) {
            segments_[i].reset(mem::SingleBlock<S>::create(segment_capacity));
            versions_[i].store(0, std::memory_order_relaxed);
        }
        for (std::size_t i = 0; i < N; ++i) free_list().enqueue(i);
    }

    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    guard pin() noexcept {
        const std::size_t tid = ticket();
        // Publish the epoch we are about to read under. Sequentially consistent so a
        // thread trying to advance cannot miss us while we cannot see its advance.
        threads_[tid].state.store(bit::set_msb(epoch_.load(std::memory_order_acquire)),
                                  std::memory_order_seq_cst);
        return guard{this, tid};
    }

    /// No-op: the epoch pin already covers every slot.
    handle protect(guard&, handle h) noexcept { return h; }

    S* deref(handle h) const noexcept {
        assert(h.index() < N && "Pool: handle out of range");
        return segments_[h.index()].get();
    }

    /// @return nullopt when the pool is exhausted — i.e. the memory bound is reached.
    std::optional<handle> acquire() {
        std::size_t idx = 0;
        if (free_list().dequeue(idx)) return make_handle(idx);

        // Nothing free. If every pinned thread is up to date we can advance, which
        // releases whatever has been sitting in limbo for two epochs.
        if (try_advance(epoch_.load(std::memory_order_acquire)) && free_list().dequeue(idx))
            return make_handle(idx);
        return std::nullopt;
    }

    /**
     * @brief Return a segment that was never published.
     *
     * Straight to the free bucket: no other thread can hold a reference to something
     * that was never linked, so there is nothing to wait for.
     */
    void discard(handle h) noexcept { free_list().enqueue(h.index()); }

    /**
     * @brief Retire a segment that was reachable.
     *
     * Into the current epoch's stage bucket; it becomes reusable two advances later,
     * by which time no thread pinned when it was still linked can remain.
     */
    void retire(handle h) noexcept {
        limbo(epoch_.load(std::memory_order_acquire)).enqueue(h.index());
    }

    [[nodiscard]] bool register_thread() noexcept {
        std::size_t t = 0;
        return tickets_.acquire(t);
    }

    void unregister_thread() noexcept { tickets_.release(); }

    std::size_t ticket() noexcept {
        std::size_t t = 0;
        const bool ok = tickets_.acquire(t);
        assert(ok && "Pool: no ticket available; did the thread call acquire()?");
        if (!ok) std::abort();
        return t;
    }

    std::size_t max_threads() const noexcept { return max_threads_; }

    static constexpr std::size_t pool_size() noexcept { return N; }

    // -- exposed for deterministic testing of the epoch machine ------------------

    /// Current global epoch.
    uint64_t epoch() const noexcept { return epoch_.load(std::memory_order_acquire); }

    /// Attempt one epoch advance. @return true if the epoch moved.
    bool try_advance_epoch() noexcept { return try_advance(epoch_.load(std::memory_order_acquire)); }

    /// Indices currently reusable.
    std::size_t free_count() const noexcept { return free_list().size(); }

private:
    using Ring = mem::detail::RingSlab::Ring;

    Ring& limbo(uint64_t e) const noexcept { return *buckets_.get(e % kStages); }
    Ring& free_list() const noexcept { return *buckets_.get(kFreeBucket); }

    handle make_handle(std::size_t idx) noexcept {
        const uint32_t v = versions_[idx].fetch_add(1, std::memory_order_relaxed) + 1;
        return handle{handle::next_version(v), static_cast<uint32_t>(idx)};
    }

    void unpin(std::size_t tid) noexcept {
        threads_[tid].state.store(0, std::memory_order_release);
    }

    /**
     * @brief Advance the epoch if every pinned thread has already published @p e.
     *
     * A thread pinned at an older epoch may still be reading something retired then, so
     * the epoch must wait for it. Unpinned threads hold nothing.
     */
    bool try_advance(uint64_t e) noexcept {
        for (std::size_t i = 0; i < max_threads_; ++i) {
            const uint64_t st = threads_[i].state.load(std::memory_order_seq_cst);
            if (bit::get_msb(st) == 0) continue;             // not pinned
            if (bit::clear_msb(st) != e) return false;       // pinned, but behind
        }
        uint64_t expected = e;
        if (!epoch_.compare_exchange_strong(expected, e + 1, std::memory_order_acq_rel,
                                            std::memory_order_acquire))
            return false;

        // Exactly one thread wins the CAS, so exactly one drains. Everything retired two
        // epochs ago is now beyond the reach of any pin and becomes reusable.
        Ring& safe = limbo(e + 2);
        std::size_t idx = 0;
        while (safe.dequeue(idx)) free_list().enqueue(idx);
        return true;
    }

    std::size_t max_threads_;
    Ticketing tickets_;
    std::vector<Slot> threads_;
    mutable mem::detail::RingSlab buckets_;
    std::vector<std::atomic<uint32_t>> versions_;
    std::vector<mem::unique_block<S>> segments_;

    ALIGNED_CACHE std::atomic<uint64_t> epoch_{0};
    CACHE_PAD_TYPES(std::atomic<uint64_t>);
};

} // namespace mem::source
