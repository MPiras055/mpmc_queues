#pragma once
/**
 * @file LinkedProxy.hpp
 * @brief Michael Scott traversal proxy with enhanced capabilities
 *
 * @ingroup proxy
 */

#include <core/Admission.hpp>
#include <core/Proxy.hpp>
#include <core/Segment.hpp>
#include <core/SegmentTraits.hpp>
#include <core/Source.hpp>
#include <meta/OptionsPack.hpp>
#include <proxy/Admission.hpp>
#include <util/align.hpp>
#include <util/specs.hpp>
#include <atomic>
#include <cassert>
#include <concepts>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace proxy {

struct ProxyOpt {
    /**
     * @brief Count segments entering and leaving service.
     * @note: off by default
     */
    struct segment_stats {};

    /**
     * @brief How many times enqueue() will renew and retry after the source comes up empty.
     *
     * Bounded, and it has to be: under a pooled source "empty" is also how the memory bound is
     * reported, so an unbounded retry here would spin forever on a genuinely full queue instead
     * of refusing. Default 2 -- enough to cover a rotation this thread was itself blocking,
     * which is the case it exists for.
     * @note value option: `ProxyOpt::acquire_retries<4>`
     */
    template <auto N> struct acquire_retries {};
};

namespace detail {

/// Disabled stat
struct SegmentStatsOff {
    static constexpr bool enabled = false;
    constexpr void on_link() const noexcept {}
    constexpr void on_retire() const noexcept {}
    constexpr void on_discard() const noexcept {}
};

/**
 * @brief Enabled: three monotonic counters, relaxed.
 * @note: counters explicitly not aligned to keep the memory footprint low
 */
struct SegmentStatsOn {
    static constexpr bool enabled = true;

    void on_link() noexcept { linked_.fetch_add(1, std::memory_order_relaxed); }
    void on_retire() noexcept { retired_.fetch_add(1, std::memory_order_relaxed); }
    void on_discard() noexcept { discarded_.fetch_add(1, std::memory_order_relaxed); }

    uint64_t linked() const noexcept { return linked_.load(std::memory_order_relaxed); }
    uint64_t retired() const noexcept { return retired_.load(std::memory_order_relaxed); }
    uint64_t discarded() const noexcept { return discarded_.load(std::memory_order_relaxed); }

private:
    std::atomic<uint64_t>
        linked_{0},retired_{0},discarded_{0};
};

} // namespace detail

/**
 * @brief per-thread bookkeeping, stored in the source's registry node.
 * @note: it's explicitly not aligned since it is stored inside the source's `ThreadData`,
 * which is anyway padded
 * @tparam H the source's handle type.
 */
template <typename H>
struct ThreadMeta {
    /// signed counter to accurately track the size of the queue across concurrent threads
    std::atomic<int64_t> ops{0};
    /// last tail (handle type) the thread found closed
    H last_seen{};
};

/**
 * @brief A queue built from linked bounded segments. One traversal, three policies.
 *   - **Admit**  what stops us admitting another item
 *   - **Source** where segments go to and come from (may also implement an implicit admission policy
 *      see: `source/Pool.hpp`)
 *   - **Segment** the algorithm inside each node
 */
template <typename T, typename Segment, typename Admit, typename Source,
          typename Opt = meta::EmptyOptions>
    requires core::LinkedSegment<Segment, T> && core::AdmissionPolicy<Admit> &&
             core::SegmentSource<Source, Segment> &&
             meta::AcceptsOnly<Opt, typename ProxyOpt::segment_stats,
                               meta::ValueOption<ProxyOpt::acquire_retries>>
class LinkedProxy {
    using Tr = core::segment_traits<Segment>;
    using H = typename Source::handle;

    /**
     * @name Where the admission policy is asked
     *
     * Exactly one of these is true for a bounded policy, so one call site compiles away
     * entirely. See core::AdmitPoint: a policy that reserves must be asked before the
     * traversal commits, while one that counts segments cannot answer until the tail has
     * actually refused an item.
     * @{
     */
    /// @see ProxyOpt::acquire_retries
    static constexpr unsigned kAcquireRetries = static_cast<unsigned>(
        Opt::template get<ProxyOpt::acquire_retries, unsigned{2}>);

    static constexpr bool admits_up_front =
        Admit::bounded && Admit::admit_point == core::AdmitPoint::Enqueue;
    static constexpr bool admits_on_link =
        Admit::bounded && Admit::admit_point == core::AdmitPoint::SegmentLink;
    /// @}

    /// Check if stats are enabled
    static constexpr bool stats_enabled = Opt::template has<typename ProxyOpt::segment_stats>;
    using Stats = std::conditional_t<stats_enabled, detail::SegmentStatsOn,
                                     detail::SegmentStatsOff>;

    ///static check for mem-proxy handle type and source
    static_assert(std::same_as<typename Segment::handle_type, H>,
                  "the segment's handle_type is not the source's handle: a pooled proxy "
                  "must be given a segment built for the same pool size, e.g. "
                  "MemBounded<T, seg::Vyukov<T, Opt, mem::IndexHandle<N>>, N>");

    //static check for the segment to be recyclable if the source demands it
    static_assert(!Source::recycles || Tr::recyclable,
                  "this source reuses segments, but this segment type cannot be reopened "
                  "(see segment_traits<Segment>::recyclable)");

    //check that the segment implements it the close_hint overload
    static_assert(!Tr::needs_close_hint || core::HintedSegment<Segment, T>,
                  "segment_traits says this segment needs the close hint, but it has no "
                  "enqueue(T, bool) overload");

    using Meta = ThreadMeta<H>;

    /// check that the source was given the ThreadMeta as payload
    static_assert(std::same_as<typename Source::thread_payload, Meta>,
                  "the source was not given this proxy's ThreadMeta as its payload: build it "
                  "as e.g. mem::source::Hazard<Segment, proxy::ThreadMeta<Segment*>>");

public:
    /// Tuning, exposed the way the sources expose theirs: so a test can assert an option
    /// actually reached the member it names. @see ProxyOpt::acquire_retries
    static constexpr unsigned acquire_retries = kAcquireRetries;

    /**
     * @brief A scope in which the calling thread may use this queue; see join().
     *
     * @note: some methods, specifically `size()` keeps quiescent data in thread_local memory.
     * When thread stops referencing the queue (detachment), it has to update the queue shared
     * state, in order to do this we rely on `session`
     */
    class session {
        LinkedProxy* q_ = nullptr;
        Meta* me_ = nullptr;
        typename Source::session inner_{};

    public:
        session() noexcept = default;
        session(LinkedProxy* q, Meta* me, typename Source::session inner) noexcept
            : q_{q}, me_{me}, inner_{std::move(inner)} {}

        /// Delete copy constructor and copy assignment
        session(const session&) = delete;
        session& operator=(const session&) = delete;

        /// Move constructor
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

        /// Session destructor
        ~session() noexcept {
            hand_back();
        }

        /// Check if session was correctly initialized
        explicit operator bool() const noexcept { return static_cast<bool>(inner_); }

    private:
        /// @brief: cleanup method
        ///
        /// records all the operations of the session holder in the proxy internal
        /// counter and re-initializes the ThreadMeta assoicated to the session holder
        /// for future reuse
        void hand_back() noexcept {
            if(q_ && me_) {
                q_-> departed_ops_.fetch_add(
                    me_->ops.exchange(0,std::memory_order_relaxed),
                    std::memory_order_relaxed
                );
                me_->last_seen = Source::nil();
                q_   = nullptr;
                me_  = nullptr;
            }
        }
    };

    /**
     * @brief How many segments will be live at once, and therefore the capacity divisor.
     *
     * Neither component knows this alone: for a chunk-bounded queue the admission policy caps
     * it, for a pooled one the source does, and for an unbounded one neither. Each answers 0
     * for "I do not bound this", so the binding limit is the smaller non-zero of the two.
     */
    static constexpr std::size_t split_across() noexcept {
        constexpr std::size_t a = Admit::live_segments();
        constexpr std::size_t b = Source::live_segments();
        if constexpr (a == 0 && b == 0) return 1;   // unbounded: the argument is the segment size
        else if constexpr (a == 0) return b;
        else if constexpr (b == 0) return a;
        else return a < b ? a : b;
    }

    /// How many segments this queue can have live at once. Compile-time, because both sides now
    /// declare it as a template parameter -- which is the point: a chunk-bounded queue and a
    /// pooled one built from the same constant can no longer disagree about their geometry.
    static constexpr std::size_t live_segments = split_across();

    /// The per-segment size a total of @p capacity resolves to, after the segment's rounding.
    static constexpr std::size_t per_segment(std::size_t capacity) noexcept {
        constexpr std::size_t d = live_segments;
        const std::size_t share = (capacity + d - 1) / d;   // ceil: never rounds to zero
        // Floored at two before the segment sees it. A ring that distinguishes laps by
        // `seq == t + capacity` degenerates at capacity 1 -- it never reports itself full, so
        // a linked segment never gets a successor and the traversal spins. The segments floor
        // themselves too; this keeps the proxy from ever asking in the first place, and is why
        // capacity() can exceed a request smaller than 2 * split_across().
        return Segment::capacity_for(share < 2 ? 2 : share);
    }

    /// What one segment actually holds. Read back from the source, which is what built them at
    /// that size, rather than kept as a second copy here.
    std::size_t segment_capacity() const noexcept { return source_.segment_capacity(); }

    /**
     * @param capacity total items the queue should hold, **split across the segments that will
     *        exist** -- `live_segments` of them, which is a template parameter of the admission
     *        policy or of the source, whichever binds.
     *
     * One argument, deliberately. The segment count used to be a second, defaulted one, and
     * because nothing ever passed it a chunk-bounded queue quietly took 4 while a pooled one
     * took its pool size. Making it part of the type removes the thing there was to forget.
     *
     * @note `capacity()` may exceed @p capacity. Segments round their own size up -- SCQ and
     *       LFring always to a power of two -- so the split is computed, handed to the segment,
     *       and then read back through `Segment::capacity_for`. Reporting the *achievable*
     *       figure is what keeps `capacity()` a number the queue can actually reach.
     *
     * @note There is no thread count either. The source's registry sizes itself, so a
     *       participant limit would be a number the queue does not need and could only get
     *       wrong.
     */
    explicit LinkedProxy(std::size_t capacity)
        : source_{per_segment(capacity)},
          admit_{Admit::config(source_.segment_capacity())} {
        assert(capacity != 0 && "LinkedProxy: capacity must be non-null");

        // source session
        auto s = source_.join();
        assert(s && "LinkedProxy: could not register the constructing thread");
        //pin the caller to the source (unnecessary but good practice)
        auto g = source_.pin();
        auto sentinel = source_.acquire();
        assert(sentinel && "LinkedProxy: could not obtain a sentinel segment");
        //initialize head and tail
        head_.store(*sentinel, std::memory_order_relaxed);
        tail_.store(*sentinel, std::memory_order_relaxed);
        stats_.on_link();   //record the segment as linked
    }

    /// Delete copy constructor and copy assignment
    LinkedProxy(const LinkedProxy&) = delete;
    LinkedProxy& operator=(const LinkedProxy&) = delete;

    ~LinkedProxy() {
        /// join the source
        [[maybe_unused]] auto guard = source_.join();
        T ignore{};
        //drain all the segments (potentially returns them to the source)
        while (dequeue(ignore));
        auto g = source_.pin();     //pin the caller as active
        H h = head_.load(std::memory_order_relaxed);
        //loop body executed only once (almost to all cases)
        while (h != Source::nil()) {
            Segment* s = source_.deref(h);
            const H nx = s->next();
            source_.discard(h);
            h = nx;
        }
    }

    bool enqueue(T item) noexcept {
        auto g = source_.pin(); // RAII protection for source memory reclamation
        Meta& me = g.payload();

        if constexpr (admits_up_front) {
            // try admit policy which may reseve a slot for the calling thread
            // a reserving policy has to claim here, before the traversal commits
            if (!admit_.try_admit()) return false;
        }

        //protect the current tail
        H tail = source_.protect(g, tail_.load(std::memory_order_relaxed));
        unsigned retries = 0;   //renew-and-retry budget after the source comes up empty

        for (;;) {
            //update protection on stale tail
            const H observed = tail_.load(std::memory_order_acquire);
            if (tail != observed) { // tail moved under us; re-protect and restart
                //abandoning `tail`, so move forward first and then re-read the anchor
                source_.renew(g);
                tail = source_.protect(g, tail_.load(std::memory_order_acquire));
                continue;
            }

            Segment* s = source_.deref(tail);

            //advance the tail to next pointer if exist and update protection
            if (const H nx = s->next(); nx != Source::nil()) {
                H expect = tail;
                (void) tail_.compare_exchange_strong(expect, nx, std::memory_order_acq_rel,
                                                    std::memory_order_acquire);
                //moving off this segment for good, so renew and re-anchor rather than
                //carrying `nx` -- which was read at the old epoch -- across the bump
                source_.renew(g);
                tail = source_.protect(g, tail_.load(std::memory_order_acquire));
                //recheck for tail stale reference
                continue;
            }

            // try enqueue on the current segment, fails if the segment is closed
            if (try_enqueue(s, tail, me, item)) break;

            //lambda which is checked while trying to get a new segment
            //aims to prevent unnecessary contention in the source.acquire()
            //path, if we're sure that the segment is going to be throwed out
            //anyway
            const auto unchanged = [&]() noexcept {
                //tail has changed or a new next pointer has been linked
                //new segment would be throwed anyway
                return tail_.load(std::memory_order_acquire) == tail &&
                       s->next() == Source::nil();
            };
            if constexpr (admits_on_link) {
                if (!admit_.try_admit()) return false;  //segment ceiling reached
            }

            auto fresh = source_.acquire(unchanged);
            if (!fresh) {   //no segment could have been got
                if (!unchanged()) { //another segment was successfuly linked
                    tail = source_.protect(g, tail_.load(std::memory_order_acquire));
                    continue;
                }
                // Coming up empty under an epoch source is frequently self-inflicted: the only
                // thing that can free a slot is a rotation, and a rotation is refused while any
                // thread sits pinned at an older stage -- this one included, since it has been
                // holding the stage it pinned at since the top of enqueue(). Retrying from here
                // without renewing just re-asks a question whose answer this thread is itself
                // preventing.
                //
                // So renew, which drops the stale stage and republishes at the current one, and
                // start the traversal over. The thread stops blocking everybody else's
                // try_advance, and the segments it was holding back become reclaimable.
                //
                // Renewing rather than unpinning outright is what makes this safe. `s` is used
                // after acquire() returns -- link_next() is called on it -- so protection has
                // to stay live across the call; dropping it would leave `s` free to be recycled
                // underneath the link. Renewal keeps the thread protected while releasing what
                // it was protecting, hence the mandatory re-read of the anchor below, and hence
                // `continue` rather than falling through to `s`.
                //
                // Gated on renew() reporting that it actually moved, which is what separates
                // the two cases the source cannot distinguish for us. If this thread was
                // already at the current stage it was blocking nothing, so the empty pool is
                // the real memory bound and retrying would just re-walk the queue to be told
                // so again -- measured at 73k pointless retries per 2M items before the gate.
                // A no-op renew also leaves `tail` protected, so falling through is safe.
                if (retries < kAcquireRetries && source_.renew(g)) {
                    ++retries;
                    tail = source_.protect(g, tail_.load(std::memory_order_acquire));
                    continue;
                }
                //only a reserving policy has anything outstanding; a link-point one tested
                if constexpr (admits_up_front) admit_.cancel_admit();
                return false; // genuinely exhausted: stop try and fail fast
            }

            Segment* ns = source_.deref(*fresh);
            //enqueue item in isolation
            const bool placed = ns->enqueue(item);
            assert(placed && "LinkedProxy: a fresh segment refused the first item");
            (void)placed;

            //installed reference which is to be setted if `link_next` fails
            H installed{};
            if (s->link_next(*fresh, installed)) { //try to link segment
                H expect = tail;
                //link was successful so try to update the global tail segment
                (void)tail_.compare_exchange_strong(expect, *fresh, std::memory_order_acq_rel,
                                                    std::memory_order_acquire);
                //signal on the admission policy
                admit_.on_segment_linked();
                stats_.on_link();
                break;
            }

            if constexpr (Source::recycles) {
                //if source recycles segments we need to discard the previous enqueue
                //to keep the segment consistent
                T stray{};
                [[maybe_unused]] const bool drained = ns->dequeue(stray);
                assert(drained && "LinkedProxy: the segment we alone hold refused the item back");
            }

            source_.discard(*fresh);                //discard the previously acquired segment
            stats_.on_discard();                    //update the discard stats
            source_.renew(g);                       //renew protection on the tail
            tail = source_.protect(g, installed);
        }

        admit_.on_enqueue();
        me.ops.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    /// @copydoc core::Queue::try_enqueue
    /// A proxy never blocks: it refuses, or links a successor and retries. Same operation.
    bool try_enqueue(T item) noexcept { return enqueue(item); }
    /// @copydoc core::Queue::try_dequeue
    bool try_dequeue(T& out) noexcept { return dequeue(out); }

    bool dequeue(T& out) noexcept {
        auto g = source_.pin(); //RAII protection for the source memeory reclamation
        Meta& me = g.payload();

        //protect the current global head
        H head = source_.protect(g, head_.load(std::memory_order_relaxed));
        for (;;) {
            //update protection on stale head
            const H observed = head_.load(std::memory_order_acquire);
            if (head != observed) {
                source_.renew(g);
                head = source_.protect(g, head_.load(std::memory_order_acquire));
                continue;
            }

            Segment* s = source_.deref(head);
            if (s->dequeue(out)) {
                //fast path: dequeue happened on the current segment
                took_one(me);
                return true;
            }

            //check if successor exists and try to advance it
            const H nx = s->next();
            if (nx == Source::nil()) return false; //empty

            if constexpr (Tr::needs_dequeue_prepare) {
                //some segments require special treatment when a successor is found
                static_assert(core::PreparableSegment<Segment>,
                              "segment_traits says this segment needs dequeue preparation, "
                              "but it has no prepare_dequeue_after_link()");
                s->prepare_dequeue_after_link();
            }

            //retry dequeue on the current segment to assert
            // the segment is truly empty and nobody will link again
            if (s->dequeue(out)) {
                took_one(me);
                return true;
            }

            //static check for segments which don't support lock-free enqueue/dequeue
            if constexpr (Tr::needs_inflight_drain) {
                static_assert(core::DrainableSegment<Segment>,
                              "segment_traits says this segment needs an in-flight drain, "
                              "but it has no has_inflight()");
                if (s->has_inflight()) continue;
            }

            //try to unlink the current head (make it unreachable for future threads)
            H expect = head;
            if (head_.compare_exchange_strong(expect, nx, std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
                //Retire first, then renew: `expect` is only passed by value here, never
                //dereferenced, so handing it over before moving epoch is safe -- and renewing
                //first would let this thread's own advance free it under a slower reader.
                source_.retire(expect); // retire the current head
                admit_.on_segment_retired();
                stats_.on_retire();
                source_.renew(g);
                head = source_.protect(g, head_.load(std::memory_order_acquire));
            } else {
                source_.renew(g);
                head = source_.protect(g, head_.load(std::memory_order_acquire));
            }
        }
    }

    /**
     * @brief: size method (estimate)
     *
     * @note: performs a reduction on thread-local memory over all the threads attached
     *
     * @warning: this is an under-approximation over concurrent threads but becomes exact
     * if no threads are currently performing enqueue/dequeue
     */
    std::size_t size() const noexcept {
        int64_t total = departed_ops_.load(std::memory_order_relaxed);
        total += source_.reduce_payloads(int64_t{0}, [](int64_t acc, const Meta& m) {
            return acc + m.ops.load(std::memory_order_relaxed);
        });
        return total > 0 ? static_cast<std::size_t>(total) : 0;
    }

    /// @brief: get a conservative item ceiling bound
    /// @warning: this value is only an estimation, it is really possible
    /// that under extreme contention the queue may accept far less items or a few
    /// more (generally a fraction of the bound capacity)
    /**
     * @brief Items this queue can hold.
     *
     * Ask the policy; if it has no opinion, the ceiling is structural. A policy that counts
     * items answers exactly, one that counts segments answers in segments, and `admit::None`
     * answers 0 -- at which point how many segments can be live is the only bound there is.
     *
     * The pooled case needs no branch of its own: `live_segments` already took the smaller of
     * the policy's limit and the source's, so `admit::None` over a `Pool<N>` gives `N` segments'
     * worth and over an allocating source gives one.
     */
    std::size_t capacity() const noexcept {
        const std::size_t stated = admit_.capacity(segment_capacity());
        return stated != 0 ? stated : live_segments * segment_capacity();
    }

    /*
     * @brief: get the number of segments successfuly linked
     * @note: this method is only enabled if segments_stat options are enabled
     */
    uint64_t segments_linked() const noexcept
        requires(stats_enabled)
    {
        return stats_.linked();
    }

    /*
     * @brief: get the number of segments that were linked to the queue and given back to the source
     * @note: this method is only enabled if segments_stat options are enabled
     */
    uint64_t segments_retired() const noexcept
        requires(stats_enabled)
    {
        return stats_.retired();
    }

    /*
     * @brief: get the number of segments unsuccessfuly linked
     * @note: this method is only enabled if segments_stat options are enabled
     * @note: a thread may get a segment and try to link it and then give it back to the source
     */
    uint64_t segments_discarded() const noexcept
        requires(stats_enabled)
    {
        return stats_.discarded();
    }
    /// @}

    /**
     * @brief: attach the calling thread to the queue
     * @returns: a session RAII object which gets destroyed at hte end of scope
     */
    [[nodiscard]] session join() {
        auto inner = source_.join();
        if (!inner) return session{};
        //lookup of the per-thread metadata
        Meta* me = nullptr;
        {
            auto g = source_.pin();
            me = &g.payload();
        }
        return session{this, me, std::move(inner)};
    }

private:

    /**
     * @brief: what to do when an item is successfuly dequeued
     */
    void took_one(Meta& me) noexcept {
        admit_.on_dequeue();
        me.ops.fetch_sub(1, std::memory_order_relaxed);
    }


    /**
     * @brief: try enqueue, consitent with segment policy
     *
     * @note: for some segments, trying to enqueue an item on a previously closed
     * segment continously may result in livelock.
     *
     * @note: threads record the last tail handle they found closed and feed it
     * to the segment enqueue method if the segment needs a close_hint
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

    CACHE_LINE_MEMBER(std::atomic<H>, head_, {});
    CACHE_LINE_MEMBER(std::atomic<H>, tail_, {});

    /// Everything counted by threads that have since left. Kept apart from the per-thread
    /// counters because those become unreachable the moment their thread detaches.
    CACHE_LINE_MEMBER(std::atomic<int64_t>, departed_ops_, {0});

    Source source_;
    [[no_unique_address]] Admit admit_; //may be optimized out if source implements an admission policy internally
    [[no_unique_address]] mutable Stats stats_{};
};

} // namespace proxy
