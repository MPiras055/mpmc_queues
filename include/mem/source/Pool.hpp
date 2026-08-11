#pragma once
#include <algo/CacheRing.hpp>
#include <algo/PhasedBucket.hpp>
#include <core/SegmentTraits.hpp>
#include <mem/Handle.hpp>
#include <mem/SingleBlock.hpp>
#include <mem/source/Payload.hpp>
#include <util/bit.hpp>
#include <util/specs.hpp>
#include <util/threading/ThreadRegistry.hpp>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

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
 * ## Reclamation: four buckets in a rotation
 *
 * Every bucket plays each role in turn, and the role is just the epoch modulo four:
 *
 * ```
 *   current(e) = e       retirements land here
 *   grace(e)   = e - 1   late retirements, from a thread pinned one epoch back
 * //@debug we need another grace bucket here because late grace(e) and early free(e) could collide
 *   free(e)    = e - 2   acquire pops here
 *   next(e)    = e + 1   late acquires, from a thread pinned one epoch back
 *
 *   next <-- free <-- @debug(grace_1) <-- grace <-- current <--+
 *     |                                                        |
 *     +--------------------------------------------------------+
 * ```
 * 
 * Race condition with having only grace:
 * suppose thread wants to retire something on epoch e
 * before it retires the epoch is advanced to e + 1
 * grace(e) becomes free(e+1)
 * a thread wants to get something on epoch e+1
 * dequeues from free(e+1) which is grace(e) while the other thread
 * is enqueing (violating the MP/NC invariant). Since rings are allocated
 * as a single block of memory, this could determine that the enqueue or dequeue
 * gets placed on another buffer or goes out of bounds
 *
 * An index retired at epoch *e* sits in `current(e)`; two advances later that same bucket is
 * `free`, and only then can it be handed out. A pinned thread publishes the epoch it saw, and
 * the epoch may advance only when every pinned thread published the current one -- which is
 * what makes "two epochs ago" mean "no live reader can still hold it".
 *
 * ## Why four, and why these buckets
 *
 * Four stages is what lets the buckets be algo::PhasedBucket rather than a general MPMC ring.
 * PhasedBucket is a single `fetch_add` at each end with no per-cell sequence word, but it
 * requires that a bucket is never pushed and popped at the same time. The rotation delivers
 * exactly that:
 *
 *  - a thread pinned at `e - 1` retires into `current(e - 1)`, which is `grace(e)`;
 *  - a thread pinned at `e - 1` acquires from `free(e - 1)`, which is `next(e)`.
 *
 * So `current` and `grace` only ever see pushes, `free` and `next` only pops. The pin is what
 * bounds the shift to a single epoch; without it a thread arbitrarily far behind could push
 * into a bucket being drained.
 *
 * The one dangerous transition is `next -> current`, where pops would meet pushes. It cannot
 * happen: `try_advance` refuses while any pinned thread is behind, so by the time that bucket
 * becomes `current`, no thread pinned at `e - 1` remains to be popping it.
 *
 * @note **The drain belongs to acquire(), not to the advance**, and that is not a stylistic
 *       choice. PhasedBucket resets lazily: a drain phase clears the tail only on a dequeue
 *       that *fails*, and a fill phase clears the head on its first enqueue. A bucket emptied
 *       by successful dequeues alone reports `size() == 0` while its tail still sits at the
 *       old count, so the next fill would write past the end. Having the advancer drain and
 *       reset instead is worse than redundant -- it is unsound. The drain would have to run
 *       before the epoch is published, or pushers race it, and after the decision to advance,
 *       or two advancers race each other; and a thread still draining once another has won
 *       the CAS would have its failing dequeue's `fetch_and` wipe a fresh pusher's tail.
 *       Letting `acquire()` walk `free` then `next` gives the failing dequeue for free, in the
 *       ordinary path, with no extra synchronisation.
 *
 * @note `discard()` does not enter the rotation at all. A segment that was never published has
 *       no observers, so making it wait two advances is pure latency; it goes to an
 *       algo::CacheRing and `acquire()` looks there first. The cache is genuinely MPMC with no
 *       phase discipline, which is why it is a CacheRing and not a fifth bucket -- its
 *       single-word ABA-safe CAS is what makes that cheap.
 *
 * @note The total number of indices is `N`, spread across the cache, the four buckets and the
 *       handles currently held. No container can therefore overflow its capacity, which is the
 *       precondition PhasedBucket asserts rather than defends against.
 */
template <typename S, std::size_t N, typename Payload = NoPayload>
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

    /// Four: current / grace / free / next. Three cannot separate "still being filled" from
    /// "safe to drain" while a thread may be one epoch behind.
    static constexpr uint8_t kStages = 4;

    /// Bit 7 pinned, bits 0-1 the stage. The epoch scan only ever asks "pinned, and at my
    /// stage?", and the pin bounds the shift to one epoch, so the full 64-bit epoch this used
    /// to carry was never read.
    static constexpr uint8_t kPinned = 0x80;
    static constexpr uint8_t kStageMask = kStages - 1;

    /// Advance attempts one acquire() will make: one rotation window and no more. Beyond a
    /// full lap there is nothing left for a rotation to surface that this thread can reach.
    static constexpr unsigned kMaxAdvances = kStages;
    /// Re-checks of the containers while the caller says waiting is still worthwhile. Trades
    /// a longer stall under contention against reporting a memory bound that is not real;
    /// raise it first if a pooled proxy starts refusing below its actual capacity.
    static constexpr unsigned kMaxSpins = 64;

    struct alignas(CACHE_LINE) ThreadData {
        std::atomic<uint8_t> state{0};
        [[no_unique_address]] Payload user{};
    };

    /**
     * The epoch scan only ever needs to ask "is any *attached* thread pinned behind me",
     * so the per-thread words live in a registry rather than in a fixed array. Two things
     * follow. The scan is O(attached threads), which matters
     * because acquire() runs it whenever the pool comes up dry. And a thread that dies
     * while pinned no longer wedges the epoch forever: its node leaves the active list, so
     * it stops being consulted, where a fixed slot array kept its stale pinned word
     * visible for the lifetime of the pool.
     */
    using Registry = util::threading::ThreadRegistry<ThreadData>;
    using Node = typename Registry::Node;

public:
    /// Sized by the pool, so all but log2(N) bits of the word are ABA counter.
    using handle = mem::VersionedIndex<N>;
    using thread_payload = Payload;
    /// RAII thread registration; see join().
    using session = typename Registry::session;

    /// Segments come back from the pool dirty; the proxy must reopen() before reuse.
    static constexpr bool recycles = true;

    static constexpr handle nil() noexcept { return handle{}; }

    /**
     * @brief RAII epoch pin.
     *
     * Protection is epoch-wide rather than per-object, so `protect()` has nothing to
     * publish — holding the pin is what keeps every slot alive.
     *
     * @warning **Not re-entrant.** The destructor stores 0 unconditionally, so an inner
     *          guard ending clears the epoch the outer one published and leaves it running
     *          unpinned, with nothing to report it. `LinkedProxy`'s destructor is written
     *          around this: its pin covers the discard loop only, after the drain, because
     *          `dequeue()` pins internally.
     */
    class guard {
        Node* node_;
        uint8_t stage_;

    public:
        guard(Node* n, uint8_t s) noexcept : node_{n}, stage_{s} {}

        /// The stage this pin published. Every bucket role is derived from *this*, never
        /// from the global stage -- see the note on Pool::acquire.
        uint8_t stage() const noexcept { return stage_; }
        guard(const guard&) = delete;
        guard& operator=(const guard&) = delete;
        ~guard() { node_->data.state.store(0, std::memory_order_release); }

        /// This thread's caller-owned state. No second lookup: the pin already found it.
        Payload& payload() const noexcept { return node_->data.user; }
    };

    /**
     * @brief Pins for the enclosing scope, unless the caller already holds a pin.
     *
     * `guard`'s destructor clears the published stage unconditionally, so pinning
     * unconditionally inside a public entry point would silently unpin an outer scope and let
     * the rotation run away from a caller that is still holding handles. The conditional is
     * what makes `acquire()` and `retire()` callable whether or not the caller pinned.
     */
    class auto_pin {
        Pool* owner_; ///< non-null only when *this* took the pin

    public:
        explicit auto_pin(Pool& p) noexcept : owner_{p.pinned() ? nullptr : &p} {
            if (owner_) (void)owner_->publish(owner_->node());
        }
        auto_pin(const auto_pin&) = delete;
        auto_pin& operator=(const auto_pin&) = delete;
        ~auto_pin() {
            if (owner_)
                owner_->registry_.self()->data.state.store(0, std::memory_order_release);
        }
    };

    /**
     * @param segment_capacity capacity handed to each of the N pooled segments
     *
     * Only the buckets need a runtime size: their capacity is N, the number of slot indices
     * that can be in flight. The slot arrays are sized by the template parameter, so they
     * are members rather than allocations and nothing here needs a length argument.
     */
    explicit Pool(std::size_t segment_capacity) {
        assert(segment_capacity != 0);

        for (std::size_t i = 0; i < N; ++i) {
            segments_[i].reset(mem::SingleBlock<S>::create(segment_capacity));
            versions_[i].store(0, std::memory_order_relaxed);
        }
        // Everything starts in the cache: no index has been published, so none of them owes
        // the rotation a grace period.
        for (std::size_t i = 0; i < N; ++i) (void)cache_.enqueue(i);
    }

    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    guard pin() noexcept {
        Node* n = node();
        // Publish the stage we are about to read under. Sequentially consistent so a thread
        // trying to advance cannot miss us while we cannot see its advance.
        return guard{n, publish(n)};
    }

    /// No-op: the epoch pin already covers every slot.
    handle protect(guard&, handle h) noexcept { return h; }

    S* deref(handle h) const noexcept {
        assert(h.index() < N && "Pool: handle out of range");
        return segments_[h.index()].get();
    }

    /**
     * @brief Take a segment, reopened and ready to use.
     * @return nullopt when the pool is exhausted — i.e. the memory bound is reached.
     *
     * Reopening happens here rather than in the proxy. A source that hands back a segment
     * which has already held items is the one that knows it needs resetting; making the
     * caller ask `if constexpr (Source::recycles)` first put that knowledge in the wrong
     * place. `recycles` remains, because LinkedProxy still static_asserts on it.
     *
     * @pre The calling thread holds a pin. Reclamation here is epoch-based, so taking a
     *      segment outside one is meaningless: nothing would stop the epoch advancing past
     *      the point where this handle is safe to deref.
     * 
     * @note **A full rotation, not one step.** A slot retired at stage `r` only reaches a
     *       thread pinned at `r + 2`, so a single advance cannot surface it; giving up after
     *       one made a pool with free capacity report exhaustion, which `admit::None` turns
     *       into a refused enqueue. The loop is bounded by the rotation length, so a pool that
     *       really is full still reports the memory bound rather than spinning on it.
     *
     * @note This does **not** re-publish the pin after a rotation, and must not. The pin is
     *       the only protection there is -- `protect()` is a no-op, because it is epoch-wide --
     *       so moving it forward would let the stage pass the point where a segment the
     *       *caller* is still holding becomes reusable. `LinkedProxy::enqueue` holds a
     *       protected tail across this call and keeps using it afterwards. Progress does not
     *       need it either: a rotation sweeps the flipping bucket into the cache, and `take()`
     *       looks there first.
     * @note **Whether to wait is the caller's question, not ours.** Coming up empty means
     *       "nothing free at this instant", which is not the same fact as "the pool is full",
     *       and nothing inside the pool can tell the two apart. `@p worth_waiting` is how the
     *       caller says which one it is looking at: `LinkedProxy` asks whether the tail it
     *       decided to extend is still the tail with still no successor, because once that
     *       stops holding there may be no need for a segment at all. The nullary overload
     *       does not wait, which is the right default for a caller with no opinion.
     *
     * @warning The spin runs **while pinned**, so it must stay bounded. Two threads pinned a
     *          stage apart can each be waiting for the other to advance, and neither can:
     *          what breaks the tie is one of them giving up, because returning drops its pin
     *          and that release is exactly what unblocks the other's `try_advance`. An
     *          unbounded spin here, or a default predicate that waits, reintroduces that
     *          livelock rather than merely being impolite.
     *
     * @param worth_waiting called when the pool is dry and the rotation will not move;
     *        returning false abandons the attempt.
     */
    template <typename Retry>
    std::optional<handle> acquire(Retry&& worth_waiting) {
        auto_pin pinned_here{*this};
        // Fixed for the whole call: see the note above on not re-publishing the pin.
        const uint8_t p = pin_stage();

        std::size_t idx = 0;
        unsigned advances = 0;
        bool got = false;

        for (unsigned spin = 0;; ++spin) {
            if (take(p, idx)) { got = true; break; }
            // Nothing reachable from this thread's stage. If every pinned thread is up to
            // date the rotation can move, which sweeps the flipping bucket into the cache and
            // brings the bucket retired two stages ago within reach. Capped at one rotation
            // window: past that, a thread that still cannot find anything is not waiting on
            // the rotation, it is waiting on another thread to give a segment back.
            if (advances < kMaxAdvances) {
                ++advances;
                if (try_advance(p)) continue;
            }
            if (spin >= kMaxSpins || !worth_waiting()) break;
            SPIN_HINT();
        }

        if (!got) return std::nullopt;
        bool op = segments_[idx]->reopen();
#ifdef NDEBUG
        (void) op;
#else
        assert(op && "Pool::acquire: segment that could have been reopened didnt");
#endif
        return make_handle(idx);
    }

    /// Take a segment if one is free right now, without waiting for anybody.
    std::optional<handle> acquire() { return acquire([]() noexcept { return false; }); }

    /**
     * @brief Return a segment that was never published.
     *
     * Straight to the cache, bypassing the rotation entirely: no other thread can hold a
     * reference to something that was never linked, so there is nothing to wait for, and
     * making it sit out two advances would be latency for nothing.
     *
     * @note No pin needed, unlike acquire() and retire(). The cache sits outside the rotation
     *       entirely, so nothing here depends on the stage and there is no epoch to protect.
     */
    void discard(handle h) noexcept {
        // Cannot fail: the cache holds N and there are only N indices in existence.
        const bool ok = cache_.enqueue(h.index());
        assert(ok && "Pool::discard: the reuse cache refused an index it must have room for");
        (void)ok;
    }

    /**
     * @brief Retire a segment that was reachable.
     *
     * Into the current stage's bucket; it becomes reusable two rotations later, by which time
     * no thread pinned when it was still linked can remain.
     */
    void retire(handle h) noexcept {
        auto_pin pinned_here{*this};
        // Into `current` for the **global** stage -- deliberately not this thread's published
        // one, which is what pops use.
        //
        // The grace period has to be measured from when the segment stopped being reachable,
        // and that is now. Retiring into the retiring thread's own stage measures it from
        // where that thread happens to be: a straggler pinned at `p-1` would file a segment
        // into `bucket(p-1)`, two rotations from free, while a reader pinned at `p` is still
        // holding it -- and a pin at `p` permits the stage to reach `p+1`, which is exactly
        // when that bucket is handed out. That is a live segment recycled under a reader, and
        // it shows up as duplicates rather than a crash.
        //
        // Still phase-safe: fills land only in `bucket(g)`, pops come from `bucket(p+2)` for
        // `p` in `{g-1, g}`, i.e. `{g+1, g+2}`. A rotation between this load and the enqueue
        // moves the push into `grace`, which is also a fill-phase bucket, and cannot happen
        // twice -- a straggler's own pin blocks the second.
        bucket(stage()).enqueue(h.index());
    }

    /// Fold @p op over every attached thread's caller-owned payload. See Hazard's copy.
    template <typename Acc, typename Op>
    Acc reduce_payloads(Acc init, Op op) const noexcept {
        return registry_.reduce(std::move(init), [&op](Acc a, const ThreadData& d) {
            return op(std::move(a), d.user);
        });
    }

    /**
     * @brief Attach the calling thread for the duration of the returned scope.
     *
     * A recycled node keeps its previous owner's payload. For the retire lists in
     * source::Hazard that is exactly what is wanted; for an epoch word it is not, so this
     * starts unpinned rather than inheriting whatever the last owner left.
     */
    [[nodiscard]] session join() {
        session s = registry_.join();
        registry_.self()->data.state.store(0, std::memory_order_release);
        return s;
    }

    /// Non-allocating join, for a caller that must not touch the allocator.
    [[nodiscard]] session try_join() noexcept {
        session s = registry_.try_join();
        if (s) registry_.self()->data.state.store(0, std::memory_order_release);
        return s;
    }

    static constexpr std::size_t pool_size() noexcept { return N; }

    // -- exposed for deterministic testing of the epoch machine ------------------

    /// Current stage, 0..kStages-1. Wraps: EBR has no ABA to protect against here, because a
    /// thread is either pinned at the current stage or it blocks the advance outright.
    uint8_t epoch() const noexcept { return stage(); }

    /// Attempt one advance. @return true if the stage moved.
    bool try_advance_epoch() noexcept { return try_advance(stage()); }

    /// Indices that acquire() could hand out right now, without rotating.
    ///
    /// @note Deliberately unpinned, and therefore approximate under concurrency: it reads a
    ///       bucket chosen from the global stage, which may move under it. Pinning inside a
    ///       const accessor would perturb the very epoch state the tests inspect through it.
    std::size_t free_count() const noexcept {
        return cache_.size() + bucket(rotate(stage(), 2)).size();
    }

    /// Indices parked in the reuse cache, i.e. discarded rather than retired.
    std::size_t cache_count() const noexcept { return cache_.size(); }

private:
    using Bucket = algo::PhasedBucket<N>;
    using Cache = algo::CacheRing<N>;

    // ---- stage arithmetic -----------------------------------------------------
    //
    // Every role is the stage plus an offset, modulo four. Named rather than inlined because
    // an off-by-one here is a silent reclamation bug, not a crash.

    uint8_t stage() const noexcept { return stage_.load(std::memory_order_acquire); }

    static constexpr uint8_t rotate(uint8_t s, uint8_t by) noexcept {
        return static_cast<uint8_t>((s + by) & kStageMask);
    }

    /**
     * @brief The stage this thread published when it pinned.
     *
     * **Every bucket role is derived from this, never from the global stage.** Pins span at
     * most two stages -- `try_advance` refuses while anyone is further behind -- so with roles
     * taken from the pinned stage `p`:
     *
     * ```
     *   fills go to  bucket(p)      for p in {g-1, g}   ->  buckets {g-1, g}
     *   pops come from bucket(p+2)  for p in {g-1, g}   ->  buckets {g+1, g+2}
     * ```
     *
     * Those two sets are disjoint, which is the entire reason there are four stages and what
     * lets the buckets be algo::PhasedBucket. Deriving the roles from the *global* stage
     * instead breaks it: a thread pinned at `g` would pop `bucket(g+1)`, and the moment the
     * rotation moves that same bucket becomes `current(g+1)` and is filled -- a pop meeting a
     * push, which is precisely what PhasedBucket forbids. That is not theoretical; it trips
     * PhasedBucket's own assertion within a few hundred milliseconds of stress.
     */
    uint8_t pin_stage() noexcept {
        return static_cast<uint8_t>(registry_.self()->data.state.load(std::memory_order_relaxed) &
                                    kStageMask);
    }

    /**
     * @brief Publish a pin at the current stage, and make sure it landed in time.
     *
     * The loop is not belt and braces. Between loading the stage and storing it, this thread
     * is **invisible** to `try_advance`: its state word still reads unpinned, so it blocks
     * nothing. The rotation can therefore move more than once in that window, and the thread
     * would go on to publish a stage two or more behind -- at which point pins no longer span
     * two stages, the fill and pop sets stop being disjoint, and a push lands in a bucket
     * somebody is draining. That is not theoretical: it corrupts a bucket within a few hundred
     * milliseconds of stress, and surfaces as a stale cell one whole rotation later.
     *
     * Re-reading after the store closes it. The store and the advance scan are both sequentially
     * consistent, so they are totally ordered: either the scan sees this pin and refuses, or
     * this re-read sees the advance and tries again. Once the re-read agrees, any later advance
     * can move by one at most, which is the invariant everything else rests on.
     */
    uint8_t publish(Node* n) noexcept {
        for (;;) {
            const uint8_t s = stage();
            n->data.state.store(static_cast<uint8_t>(kPinned | s), std::memory_order_seq_cst);
            if (stage() == s) return s;
        }
    }


    Bucket& bucket(uint8_t s) const noexcept { return buckets_[s]; }

    /**
     * @brief Take a reusable index for a thread pinned at @p p, cheapest source first.
     *
     * The failing dequeue on `bucket(p + 2)` is load-bearing beyond just reporting empty:
     * algo::PhasedBucket clears its tail only on a dequeue that fails, and that bucket becomes
     * `current` two rotations later. Testing `size()` instead would leave the tail where the
     * drain left it, and the next fill would write past the end of the array.
     */
    bool take(uint8_t p, std::size_t& idx) noexcept {
        if (cache_.dequeue(idx)) return true;   // never published: no grace owed
        if (bucket(rotate(p, 2)).dequeue(idx)) return true;
        // Look once more. Between the two misses above, another thread may have discard()ed a
        // segment that was never published, or finished a rotation -- whose sweep empties the
        // whole flipping bucket into the cache. Both are ordinary rather than exotic, and a
        // miss on an empty CacheRing costs a load and a compare.
        return cache_.dequeue(idx);
    }

    /// Is the calling thread inside a pin()?
    bool pinned() noexcept {
        const Node* n = registry_.self();
        return n && (n->data.state.load(std::memory_order_relaxed) & kPinned) != 0;
    }

    /// Stamp a fresh version onto @p idx. The counter is free-running; the handle folds it
    /// into the bits it has and skips 0, which is reserved for nil.
    handle make_handle(std::size_t idx) noexcept {
        const uint64_t n = versions_[idx].fetch_add(1, std::memory_order_relaxed) + 1;
        return handle{handle::to_version(n), static_cast<typename handle::index_type>(idx)};
    }

    /**
     * @brief This thread's registry node, attaching on first use.
     *
     * Three tiers, cheapest first: a thread-local read; then a lock-free claim of a node that
     * is already free; then, only if the registry has none, an allocation. The last tier can
     * throw, and this is reached from `pin()`, which is noexcept -- so an allocation failure
     * terminates. That is the same bargain `retire()` already makes, and it is why `join()`
     * exists: holding a session for the life of the thread keeps every later operation on the
     * first tier.
     */
    Node* node() {
        if (Node* n = registry_.self()) return n;
        if (registry_.try_attach()) return registry_.self();
        registry_.attach();
        return registry_.self();
    }

    /**
     * @brief Rotate the roles by one, if it is safe and if it would achieve anything.
     *
     * Two refusals, for different reasons:
     *
     *  - **A pinned thread is behind.** It may still be reading something retired at its
     *     stage, so the bucket that would become `free` is not safe yet. This is the property
     *     the whole scheme rests on, and it is also what keeps `next -> current` from turning
     *     into pops meeting pushes.
     *  - **Another thread is already rotating.** The drain of the flipping bucket has to be
     *     exclusive and has to complete before the new stage is published.
     *
     * @note No bucket work happens here. Nothing is drained, nothing is reset; the rotation
     *       is a single CAS on the stage. See the class note.
     */
    bool try_advance(uint8_t s) noexcept {
        // Short-circuits on the first thread that is behind, which is the common reason to
        // refuse: no point asking the rest.
        const bool everyone_current = registry_.all_of([s](const ThreadData& d) noexcept {
            const uint8_t st = d.state.load(std::memory_order_seq_cst);
            if ((st & kPinned) == 0) return true;      // not pinned: holds nothing
            return (st & kStageMask) == s;             // pinned: must be at this stage
        });
        if (!everyone_current) return false;

        // Take the rotation exclusively. The drain below has to finish *before* the new stage
        // is published, or threads pinning at s + 1 start filling the very bucket a straggling
        // drainer is still popping. A second CAS is the cheapest way to say "one winner"; the
        // alternative, draining after publishing, is the overlap PhasedBucket forbids.
        bool expected_flag = false;
        if (!rotating_.compare_exchange_strong(expected_flag, true, std::memory_order_acquire,
                                               std::memory_order_relaxed))
            return false;
        if (stage() != s) { // somebody rotated between the scan and the claim
            rotating_.store(false, std::memory_order_release);
            return false;
        }

        // Empty the bucket that is about to flip from draining to filling. Its contents are
        // free indices that only a thread a stage behind could otherwise reach -- and the scan
        // above just established there is no such thread, so without this they would be
        // stranded until the rotation came round again, which is itself blocked on this
        // bucket. Moving them to the cache is safe: nothing fills bucket(s + 1) until the
        // store below publishes s + 1, and this claim makes us the only drainer.
        Bucket& flipping = bucket(rotate(s, 1));
        std::size_t idx = 0;
        while (flipping.dequeue(idx)) (void)cache_.enqueue(idx);
        // That final failing dequeue is also what clears the bucket's tail for the fill phase
        // it is about to enter.

        stage_.store(rotate(s, 1), std::memory_order_release);
        rotating_.store(false, std::memory_order_release);
        return true;
    }

    Registry registry_;
    /// The rotation. Sized at compile time, so these are members: no slab, no allocation.
    mutable Bucket buckets_[kStages];
    /// Discarded indices, outside the rotation entirely. Genuinely MPMC, hence a ring rather
    /// than a fifth phased bucket.
    mutable Cache cache_;
    /// Per-slot reuse counters, feeding the version half of every handle.
    std::atomic<uint64_t> versions_[N]{};
    /// The pooled segments themselves. N is a template parameter, so these are members
    /// rather than a heap vector: one allocation per segment and none for the array.
    mem::unique_block<S> segments_[N];
    ALIGNED_CACHE std::atomic<uint8_t> stage_{0};
    /// Held across a rotation so the drain of the flipping bucket finishes before the new
    /// stage is visible. Rotations are rare -- only when a thread finds nothing to acquire.
    std::atomic<bool> rotating_{false};
    CACHE_PAD_TYPES(std::atomic<uint8_t>, std::atomic<bool>);
};

} // namespace mem::source
