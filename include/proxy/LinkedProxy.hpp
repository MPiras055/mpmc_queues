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
#include <concepts>
#include <cstdint>
#include <utility>

namespace proxy {

/**
 * @brief The proxy's per-thread bookkeeping, stored in the source's registry node.
 *
 * At namespace scope rather than nested in LinkedProxy, because the alias has to name it to
 * build the source, and the source is one of LinkedProxy's own template parameters -- nesting
 * it would be circular.
 *
 * Not over-aligned: it lives inside the source's `ThreadData`, which already carries the
 * cache-line alignment. Aligning it again would make each node two lines wider for nothing.
 *
 * @tparam H the source's handle type.
 */
template <typename H>
struct ThreadMeta {
    /// Signed: a thread may dequeue more than it enqueued. The sum across threads is the size.
    std::atomic<int64_t> ops{0};
    /// Last tail this thread found closed; feeds the close hint.
    H last_seen{};
};

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

    /// The segment stores its successor as `handle_type`; the source hands out `handle`.
    /// Under a pooled source both are mem::VersionedIndex<N>, and N appears twice -- once
    /// in the segment's IndexHandle<N>, once in the proxy alias -- so a mismatch would
    /// otherwise mean the segment reading the index out of the wrong bits.
    static_assert(std::same_as<typename Segment::handle_type, H>,
                  "the segment's handle_type is not the source's handle: a pooled proxy "
                  "must be given a segment built for the same pool size, e.g. "
                  "MemBounded<T, seg::Vyukov<T, Opt, mem::IndexHandle<N>>, N>");
    static_assert(!Source::recycles || Tr::recyclable,
                  "this source reuses segments, but this segment type cannot be reopened "
                  "(see segment_traits<Segment>::recyclable)");
    static_assert(!Tr::needs_close_hint || core::HintedSegment<Segment, T>,
                  "segment_traits says this segment needs the close hint, but it has no "
                  "enqueue(T, bool) overload");

    using Meta = ThreadMeta<H>;

    /// The proxy's per-thread state rides in the source's registry node, so `pin()`'s
    /// thread-local lookup serves both and the operation path does only one.
    static_assert(std::same_as<typename Source::thread_payload, Meta>,
                  "the source was not given this proxy's ThreadMeta as its payload: build it "
                  "as e.g. mem::source::Hazard<Segment, proxy::ThreadMeta<Segment*>>");

public:
    /**
     * @brief A scope in which the calling thread may use this queue; see join().
     *
     * Wraps the source's session so that a departing thread's tally is not lost with it.
     * `size()` folds over *attached* threads, so a producer that enqueued and then left used
     * to take its count away with it and the queue under-reported for the rest of its life.
     * On the way out this folds the thread's `ops` into a queue-wide total and clears the
     * payload, so whoever inherits the recycled node starts from zero rather than from the
     * last owner's tally and stale close hint.
     */
    class session {
        LinkedProxy* q_ = nullptr;
        Meta* me_ = nullptr; ///< stable while this thread is attached
        typename Source::session inner_{};

    public:
        session() noexcept = default;
        session(LinkedProxy* q, Meta* me, typename Source::session inner) noexcept
            : q_{q}, me_{me}, inner_{std::move(inner)} {}

        session(const session&) = delete;
        session& operator=(const session&) = delete;

        session(session&& o) noexcept
            : q_{std::exchange(o.q_, nullptr)}, me_{std::exchange(o.me_, nullptr)},
              inner_{std::move(o.inner_)} {}

        session& operator=(session&& o) noexcept {
            if (this != &o) {
                hand_back();
                q_ = std::exchange(o.q_, nullptr);
                me_ = std::exchange(o.me_, nullptr);
                inner_ = std::move(o.inner_);
            }
            return *this;
        }

        /// The body runs before `inner_` is destroyed, so the payload is cleaned while this
        /// thread still owns the node -- and only then does the node become reusable.
        ~session() { hand_back(); }

        explicit operator bool() const noexcept { return static_cast<bool>(inner_); }

    private:
        void hand_back() noexcept {
            if (q_ && me_) {
                q_->departed_ops_.fetch_add(me_->ops.exchange(0, std::memory_order_relaxed),
                                            std::memory_order_relaxed);
                me_->last_seen = Source::nil();
            }
            q_ = nullptr;
            me_ = nullptr;
        }
    };

    /**
     * @param segment_capacity capacity of each segment
     * @param chunks           bound, in segments, for a bounded Admit; ignored by None
     *
     * @note There is no thread count. The source's registry sizes itself, so a participant
     *       limit would be a number the queue does not need and could only get wrong.
     */
    explicit LinkedProxy(std::size_t segment_capacity, std::size_t chunks = 4)
        : seg_capacity_{segment_capacity},
          source_{segment_capacity},
          admit_{Admit::config(segment_capacity, chunks)} {
        assert(segment_capacity != 0 && "LinkedProxy: segment capacity must be non-null");

        // The sentinel. Joining is required because the source hands out segments per thread.
        auto s = source_.join();
        assert(s && "LinkedProxy: could not register the constructing thread");
        // A pooled source reclaims by epoch, so acquiring a segment is only meaningful under
        // a pin. Nothing else runs yet, but the precondition is asserted rather than assumed.
        auto g = source_.pin();
        auto sentinel = source_.acquire();
        assert(sentinel && "LinkedProxy: could not obtain a sentinel segment");
        head_.store(*sentinel, std::memory_order_relaxed);
        tail_.store(*sentinel, std::memory_order_relaxed);
    }

    LinkedProxy(const LinkedProxy&) = delete;
    LinkedProxy& operator=(const LinkedProxy&) = delete;

    ~LinkedProxy() {
        // Destroyed before `source_`, since it is a local in this body and members go after.
        [[maybe_unused]] auto guard = source_.join();
        T ignore{};
        while (dequeue(ignore)) {}
        // Whatever the traversal left linked is now unreachable; hand it back.
        //
        // The pin covers the discard loop only, and starts after the drain: `dequeue()` pins
        // internally, and a pooled source's pin is *not* re-entrant -- an inner guard's
        // destructor clears the published epoch outright, which would leave this loop
        // running unpinned with no diagnostic.
        auto g = source_.pin();
        H h = head_.load(std::memory_order_relaxed);
        while (h != Source::nil()) {
            Segment* s = source_.deref(h);
            const H nx = s->next();
            source_.discard(h);
            h = nx;
        }
    }

    bool enqueue(T item) noexcept {
        auto g = source_.pin(); // RAII: protection drops on every exit path below
        Meta& me = g.payload(); // same lookup the pin already did

        if constexpr (Admit::bounded) {
            // Reserves where the policy can, so concurrent producers cannot each pass a
            // check and then all commit past the ceiling.
            if (!admit_.try_admit()) return false;
        }

        H tail = source_.protect(g, tail_.load(std::memory_order_relaxed));
        // Bounds the retry below: one more attempt per distinct tail. Deliberately a local
        // and not `me.last_seen` -- that field is the close hint, and writing a retry into it
        // would make the next enqueue skip a segment it never saw closed.
        H last_failed = Source::nil();

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

            if (try_enqueue(s, tail, me, item)) break;

            // This segment is full or closed. Get another.
            auto fresh = source_.acquire();
            if (!fresh) {
                // The source has nothing -- but it may have had nothing only because another
                // producer took the last segment and is about to link it. Losing that race is
                // not the same as the queue being full, so look once more before reporting a
                // bound. `last_failed` makes this terminate: a tail that fails twice really
                // has no successor coming.
                if (last_failed != tail) {
                    last_failed = tail;
                    if (const H nx = s->next(); nx != Source::nil()) {
                        tail = source_.protect(g, nx);
                        continue;
                    }
                }
                if constexpr (Admit::bounded) admit_.cancel_admit();
                return false; // genuinely exhausted: this *is* the memory bound
            }

            // No reopen here. Recycling is the source's business -- a source that hands back
            // a used segment is the one that knows it needs resetting, and the proxy should
            // not have to ask. See mem::source::Pool::acquire.
            Segment* ns = source_.deref(*fresh);
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

            // Someone else linked first. Nobody ever saw ours, so it needs no scan -- but
            // `item` is already *in* it, and the retry below will place `item` again. Take
            // the copy back out before handing the segment over, or the queue holds two.
            //
            // Under an allocating source that is invisible: discard destroys the segment and
            // the stray goes with it. Under a pooled source the segment comes straight back,
            // and whether the stray survives depends on how much of itself that segment's
            // reopen() rebuilds -- SCQ, FAAArray and HQ happen to wipe their cells, PRQ
            // realigns indices only and delivers the stray a second time. Measured on
            // mem-prq at capacity 8: 200048 items consumed against 200000 produced.
            //
            // This segment is ours alone, so the dequeue is uncontended and cannot fail.
            T stray{};
            [[maybe_unused]] const bool drained = ns->dequeue(stray);
            assert(drained && "LinkedProxy: the segment we alone hold refused the item back");
            source_.discard(*fresh);
            tail = source_.protect(g, s->next());
        }

        admit_.on_enqueue();
        me.ops.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    bool dequeue(T& out) noexcept {
        auto g = source_.pin();
        Meta& me = g.payload();

        H head = source_.protect(g, head_.load(std::memory_order_relaxed));
        for (;;) {
            const H observed = head_.load(std::memory_order_acquire);
            if (head != observed) {
                head = source_.protect(g, observed);
                continue;
            }

            Segment* s = source_.deref(head);
            if (s->dequeue(out)) {
                took_one(me);
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
                took_one(me);
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

    /**
     * @brief Approximate under concurrency; exact when quiescent.
     *
     * Two parts, because a per-thread counter stops being reachable when its thread leaves:
     * the running total of everyone who has departed, plus a fold over the threads still
     * attached. A scan only visits attached threads, so without the first part a producer
     * that enqueued and then detached took its count with it and the queue under-reported
     * permanently -- see ProxyAccountingTest.
     *
     * The fold is over threads that actually attached rather than a fixed array sized for
     * the worst case, so its cost tracks real concurrency.
     */
    std::size_t size() const noexcept {
        int64_t total = departed_ops_.load(std::memory_order_relaxed);
        total += source_.reduce_payloads(int64_t{0}, [](int64_t acc, const Meta& m) {
            return acc + m.ops.load(std::memory_order_relaxed);
        });
        return total > 0 ? static_cast<std::size_t>(total) : 0;
    }

    /// Item ceiling. What that means is the policy's business, not the traversal's.
    std::size_t capacity() const noexcept { return admit_.capacity(seg_capacity_); }

    /**
     * @brief Attach the calling thread for the duration of the returned scope.
     *
     * Replaces a manual acquire/release pair, which had already grown an early-return path
     * that skipped the release. It also un-collides two names: the source's `acquire()`
     * hands out a *segment*, which is a different thing entirely.
     */
    [[nodiscard]] session join() {
        auto inner = source_.join();
        if (!inner) return session{};
        // One lookup, here rather than on every operation: the payload's address is fixed
        // for as long as this thread holds the node.
        Meta* me = nullptr;
        {
            auto g = source_.pin();
            me = &g.payload();
        }
        return session{this, me, std::move(inner)};
    }

private:
    void took_one(Meta& me) noexcept {
        admit_.on_dequeue();
        me.ops.fetch_sub(1, std::memory_order_relaxed);
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
    FORCE_INLINE bool try_enqueue(Segment* s, H h, Meta& me, T item) noexcept {
        if constexpr (Tr::needs_close_hint) {
            H& last = me.last_seen;
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

    /// Everything counted by threads that have since left. Kept apart from the per-thread
    /// counters because those become unreachable the moment their thread detaches.
    ALIGNED_CACHE std::atomic<int64_t> departed_ops_{0};
    CACHE_PAD_TYPES(std::atomic<int64_t>);

    const std::size_t seg_capacity_;
    Source source_;
    [[no_unique_address]] Admit admit_;
};

} // namespace proxy
