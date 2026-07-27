#pragma once
#include <core/Admission.hpp>
#include <core/Proxy.hpp>
#include <core/Segment.hpp>
#include <core/SegmentTraits.hpp>
#include <core/Source.hpp>
#include <proxy/Admission.hpp>
#include <util/specs.hpp>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <vector>

namespace proxy {

/**
 * @brief A queue built from linked bounded segments. One traversal, three policies.
 *
 * This replaces UnboundedProxy, BoundedCounterProxy, BoundedChunkProxy and
 * BoundedMemProxy -- roughly 1133 lines across four files, each of which had
 * independently re-implemented the same Michael-Scott traversal, ticket handling and
 * per-thread size accounting. The genuine variation was only ever three-dimensional:
 *
 *   - **Admit**  what stops us admitting another item
 *   - **Source** where segments come from and go back to (which also carries the
 *                handle type: a raw pointer, or an index into a fixed pool)
 *   - **Segment** the algorithm inside each node
 *
 * @note There is no virtual dispatch anywhere on this path. Every call below is
 *       statically bound, and the per-segment capability checks are `if constexpr` over
 *       core::segment_traits, so a segment that does not need the close hint does not
 *       branch on it at runtime.
 */
template <typename T, typename Segment, typename Admit, typename Source>
    requires core::LinkedSegment<Segment, T> && core::AdmissionPolicy<Admit> &&
             core::SegmentSource<Source, Segment>
class LinkedProxy {
    using Tr = core::segment_traits<Segment>;
    using H = typename Source::handle;

    static_assert(!Source::recycles || Tr::recyclable,
                  "this source reuses segments, but this segment type cannot be reopened "
                  "(see segment_traits<Segment>::recyclable)");
    static_assert(!Tr::needs_close_hint || core::HintedSegment<Segment, T>,
                  "segment_traits says this segment needs the close hint, but it has no "
                  "enqueue(T, bool) overload");

    /// Per-thread proxy bookkeeping, indexed by the source's ticket.
    struct alignas(CACHE_LINE) Meta {
        /// Signed: a thread may dequeue more than it enqueued. The sum is the size.
        std::atomic<int64_t> ops{0};
        /// Last tail this thread found closed; feeds the close hint.
        H last_seen{};
    };

public:
    /**
     * @param segment_capacity capacity of each segment
     * @param max_threads      concurrent participants
     * @param chunks           bound, in segments, for a bounded Admit; ignored by None
     */
    LinkedProxy(std::size_t segment_capacity, std::size_t max_threads, std::size_t chunks = 4)
        : seg_capacity_{segment_capacity}, chunks_{chunks},
          source_{max_threads, segment_capacity},
          admit_{segment_capacity * chunks, segment_capacity}, meta_(max_threads) {
        assert(segment_capacity != 0 && "LinkedProxy: segment capacity must be non-null");
        assert(max_threads != 0 && "LinkedProxy: max_threads must be non-null");

        // The sentinel. Registering is required because acquire() takes a ticket.
        const bool reg = source_.register_thread();
        assert(reg && "LinkedProxy: could not register the constructing thread");
        (void)reg;
        auto sentinel = source_.acquire();
        assert(sentinel && "LinkedProxy: could not obtain a sentinel segment");
        head_.store(*sentinel, std::memory_order_relaxed);
        tail_.store(*sentinel, std::memory_order_relaxed);
        source_.unregister_thread();
    }

    LinkedProxy(const LinkedProxy&) = delete;
    LinkedProxy& operator=(const LinkedProxy&) = delete;

    ~LinkedProxy() {
        (void)source_.register_thread();
        T ignore{};
        while (dequeue(ignore)) {}
        // Whatever the traversal left linked is now unreachable; hand it back.
        H h = head_.load(std::memory_order_relaxed);
        while (h != Source::nil()) {
            Segment* s = source_.deref(h);
            const H nx = s->next();
            source_.discard(h);
            h = nx;
        }
        source_.unregister_thread();
    }

    bool enqueue(T item) noexcept {
        auto g = source_.pin(); // RAII: protection drops on every exit path below
        const std::size_t tid = g.tid();

        if constexpr (Admit::bounded) {
            // Reserves where the policy can, so concurrent producers cannot each pass a
            // check and then all commit past the ceiling.
            if (!admit_.try_admit()) return false;
        }

        H tail = source_.protect(g, tail_.load(std::memory_order_relaxed));
        for (;;) {
            const H observed = tail_.load(std::memory_order_acquire);
            if (tail != observed) { // tail moved under us; re-protect and restart
                tail = source_.protect(g, observed);
                continue;
            }

            Segment* s = source_.deref(tail);

            if (const H nx = s->next(); nx != Source::nil()) {
                H expect = tail; // a successor exists: help publish it, then retry there
                (void)tail_.compare_exchange_strong(expect, nx, std::memory_order_acq_rel,
                                                    std::memory_order_acquire);
                tail = source_.protect(g, nx);
                continue;
            }

            if (try_enqueue(s, tail, tid, item)) break;

            // This segment is full or closed. Get another.
            auto fresh = source_.acquire();
            if (!fresh) { // pool exhausted: this *is* the memory bound
                if constexpr (Admit::bounded) admit_.cancel_admit();
                return false;
            }

            Segment* ns = source_.deref(*fresh);
            if constexpr (Source::recycles) {
                if (!ns->reopen()) { // cannot be reused; do not publish it
                    source_.discard(*fresh);
                    if constexpr (Admit::bounded) admit_.cancel_admit();
                    return false;
                }
            }
            const bool placed = ns->enqueue(item);
            assert(placed && "LinkedProxy: a fresh segment refused the first item");
            (void)placed;

            if (s->link_next(*fresh)) {
                H expect = tail;
                (void)tail_.compare_exchange_strong(expect, *fresh, std::memory_order_acq_rel,
                                                    std::memory_order_acquire);
                admit_.on_segment_linked();
                break;
            }

            // Someone else linked first. Nobody ever saw ours, so it needs no scan --
            // and `item` is still ours, so nothing is lost by retrying.
            source_.discard(*fresh);
            tail = source_.protect(g, s->next());
        }

        admit_.on_enqueue();
        meta_[tid].ops.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    bool dequeue(T& out) noexcept {
        auto g = source_.pin();
        const std::size_t tid = g.tid();

        H head = source_.protect(g, head_.load(std::memory_order_relaxed));
        for (;;) {
            const H observed = head_.load(std::memory_order_acquire);
            if (head != observed) {
                head = source_.protect(g, observed);
                continue;
            }

            Segment* s = source_.deref(head);
            if (s->dequeue(out)) {
                took_one(tid);
                return true;
            }

            const H nx = s->next();
            if (nx == Source::nil()) return false; // no successor: genuinely empty

            // A successor exists, so this segment will never grow again and must be
            // drained exhaustively before it can be unlinked.
            if constexpr (Tr::needs_dequeue_prepare) {
                static_assert(core::PreparableSegment<Segment>,
                              "segment_traits says this segment needs dequeue preparation, "
                              "but it has no prepare_dequeue_after_link()");
                s->prepare_dequeue_after_link();
            }
            if (s->dequeue(out)) {
                took_one(tid);
                return true;
            }

            // A segment whose insert is not atomic may hold an item that has been
            // claimed but not yet published; unlinking now would strand it. Retrying is
            // bounded -- the producer is a few instructions from publishing.
            if constexpr (Tr::needs_inflight_drain) {
                static_assert(core::DrainableSegment<Segment>,
                              "segment_traits says this segment needs an in-flight drain, "
                              "but it has no has_inflight()");
                if (s->has_inflight()) continue;
            }

            H expect = head;
            if (head_.compare_exchange_strong(expect, nx, std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
                head = source_.protect(g, nx);
                source_.retire(expect); // was published; defer until unobserved
                admit_.on_segment_retired();
            } else {
                head = source_.protect(g, expect);
            }
        }
    }

    /// Approximate under concurrency; exact when quiescent.
    std::size_t size() const noexcept {
        int64_t total = 0;
        for (const Meta& m : meta_) total += m.ops.load(std::memory_order_relaxed);
        return total > 0 ? static_cast<std::size_t>(total) : 0;
    }

    /// Item ceiling for a bounded proxy; the segment capacity for an unbounded one.
    std::size_t capacity() const noexcept {
        if constexpr (Admit::bounded) return seg_capacity_ * chunks_;
        else return seg_capacity_;
    }

    [[nodiscard]] bool acquire() noexcept { return source_.register_thread(); }
    void release() noexcept { source_.unregister_thread(); }

private:
    void took_one(std::size_t tid) noexcept {
        admit_.on_dequeue();
        meta_[tid].ops.fetch_sub(1, std::memory_order_relaxed);
    }

    /**
     * @brief Enqueue, supplying the close hint when the segment wants it.
     *
     * Re-entering some segments' enqueue loops after they have closed is not merely
     * wasted work: for PRQ it drives consumers down the unsafe-cell path and can
     * livelock both sides on one segment. The hint is "you told me you were full last
     * time and the tail has not moved since", which is exactly what the per-thread
     * last_seen records.
     */
    FORCE_INLINE bool try_enqueue(Segment* s, H h, std::size_t tid, T item) noexcept {
        if constexpr (Tr::needs_close_hint) {
            H& last = meta_[tid].last_seen;
            const bool hint = (last == h);
            const bool ok = s->enqueue(item, hint);
            last = ok ? Source::nil() : h;
            return ok;
        } else {
            return s->enqueue(item);
        }
    }

    ALIGNED_CACHE std::atomic<H> head_{};
    CACHE_PAD_TYPES(std::atomic<H>);
    ALIGNED_CACHE std::atomic<H> tail_{};
    CACHE_PAD_TYPES(std::atomic<H>);

    const std::size_t seg_capacity_;
    const std::size_t chunks_;
    Source source_;
    [[no_unique_address]] Admit admit_;
    std::vector<Meta> meta_;
};

} // namespace proxy
