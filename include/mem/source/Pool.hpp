#pragma once
/**
 * @file Pool.hpp
 * @brief Segment source over a fixed pool, recycled by a four-stage epoch rotation.
 * @ingroup mem
 */

#include <algo/CacheRing.hpp>
#include <algo/PhasedBucket.hpp>
#include <core/SegmentTraits.hpp>
#include <mem/Handle.hpp>
#include <mem/SingleBlock.hpp>
#include <mem/source/Payload.hpp>
#include <meta/OptionsPack.hpp>
#include <util/bit.hpp>
#include <util/align.hpp>
#include <util/specs.hpp>
#include <util/threading/ThreadRegistry.hpp>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace mem::source {

struct PoolOpt {
    /**
     * @brief Re-checks of the containers while the caller says waiting is still worthwhile.
     *
     * See Pool::acquire. Trades a longer stall under contention against reporting a memory
     * bound that is not real; raise it first if a pooled proxy starts refusing below its
     * actual capacity. **Must stay bounded** -- the spin runs while pinned, and giving up is
     * what releases the pin another thread's rotation is waiting on.
     */
    template <auto N> struct max_acquire_spins {};

    /**
     * @brief Rotation advances one acquire() will attempt.
     *
     * Defaults to the rotation length, which is the point past which a further advance cannot
     * surface anything this thread can reach.
     */
    template <auto N> struct max_advance_attempts {};
};

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
 *   free(e)    = e - 2   acquire pops here
 *   next(e)    = e + 1   late acquires, from a thread pinned one epoch back
 *
 *   next <-- free <-- grace <-- current <--+
 *     |                                    |
 *     +------------------------------------+
 * ```
 *
 * So `current` and `grace` only ever see pushes, `free` and `next` only pops. The pin is what
 * bounds the shift to a single epoch; without it a thread arbitrarily far behind could push
 * into a bucket being drained.
 *
 * @note `discard()` does not enter the rotation at all. A segment that was never published has
 *       no observers, so making it wait two advances is pure latency; it goes to an
 *       algo::CacheRing and `acquire()` looks there first -- before taking a pin at all, since
 *       a slot outside the rotation owes nothing to any epoch. The cache is genuinely MPMC with
 *       no phase discipline, which is why it is a CacheRing and not a fifth bucket -- its
 *       single-word ABA-safe CAS is what makes that cheap.
 *
 * @note The total number of indices is `N`, spread across the cache, the four buckets and the
 *       handles currently held. No container can therefore overflow its capacity, which is the
 *       precondition PhasedBucket asserts rather than defends against.
 */
template <typename S, std::size_t N, typename Payload = NoPayload,
          typename Opt = meta::EmptyOptions>
    requires meta::AcceptsOnly<Opt, meta::ValueOption<PoolOpt::max_acquire_spins>,
                               meta::ValueOption<PoolOpt::max_advance_attempts>>
class Pool {
    static_assert(core::segment_traits<S>::recyclable,
                  "mem::source::Pool reuses segments, but segment_traits<S>::recyclable is "
                  "false: this segment cannot be reopened after being drained");
    static_assert(N >= 2, "a pool needs at least two segments to make progress");

    static constexpr uint8_t kStages = 4;

    /// Bit 7 pinned, bits 0-1 the stage. The epoch scan only ever asks "pinned, and at my
    /// stage?", and the pin bounds the shift to one epoch, so the full 64-bit epoch this used
    /// to carry was never read.
    static constexpr uint8_t kPinned = bit::msb_mask<uint8_t>;
    static constexpr uint8_t kStageMask = kStages - 1;

    /// Advance attempts one acquire() will make; defaults to one rotation window, beyond which
    /// there is nothing left for a rotation to surface that this thread can reach.
    /// Cast rather than trusted: `get` returns the option's own type when one is present.
    static constexpr unsigned kMaxAdvances = static_cast<unsigned>(
        Opt::template get<PoolOpt::max_advance_attempts, unsigned{kStages}>);
    static constexpr unsigned kMaxSpins = static_cast<unsigned>(
        Opt::template get<PoolOpt::max_acquire_spins, unsigned{64}>);

    struct alignas(CACHE_LINE) ThreadData {
        std::atomic<uint8_t> state{0};
        [[no_unique_address]] Payload user{};
    };

    
    using Registry = util::threading::ThreadRegistry<ThreadData>;
    using Node = typename Registry::Node;

public:
    /// @name Tuning in effect
    /// @{
    static constexpr unsigned max_acquire_spins = kMaxSpins;
    static constexpr unsigned max_advance_attempts = kMaxAdvances;
    /// @}

    using handle = mem::VersionedIndex<N>;
    using thread_payload = Payload;
    using session = typename Registry::session;

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

    private:
        friend class Pool;   ///< renew() republishes and has to update the cached stage
        Node* node() const noexcept { return node_; }
        void restage(uint8_t s) noexcept { stage_ = s; }

    public:

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

    /// The pool size: this is the memory bound, and the divisor when a proxy splits a total
    /// capacity across the segments that will exist.
    static constexpr std::size_t live_segments() noexcept { return N; }

    /**
     * @brief No-op: the epoch pin already covers every slot.
     *
     * Deliberately *not* where the epoch is bumped. protect() promises never to invalidate
     * anything, so a caller holding an earlier handle stays safe; moving the pin here would
     * quietly break that for every caller. See renew().
     */
    handle protect(guard&, handle h) noexcept { 
        return h; 
    }

    /**
     * @brief Republish this thread's pin at the current stage.
     *
     * A pin taken at `pin()` and never moved holds `try_advance` back for the whole traversal,
     * and the traversal is longest exactly when the queue is contended -- so the thread that
     * most needs the rotation to turn is the one preventing it.
     *
     * @pre **The caller must not dereference anything it obtained before this call.** After
     *      renewing, this thread sits at a newer stage, and a segment retired under the old one
     *      may now be free. Whatever is still needed must be re-read from a shared anchor
     *      *after* this returns -- see the renew sites in LinkedProxy, which all re-read
     *      head_/tail_ immediately afterwards.
     *
     * @return true if the pin actually moved. False means this thread was already at the
     *         current stage, so it was holding nothing back and its handles remain valid --
     *         the caller can then skip the re-read this otherwise obliges it to do.
     *
     * Cheap when there is nothing to do: one relaxed load and a branch. The republish itself
     * goes through publish(), so the same store/re-read handshake that makes pin() safe against
     * a concurrent advance applies here too.
     */
    bool renew(guard& g) noexcept {
        if (stage() == g.stage()) return false;   // already current: nothing moved
        g.restage(publish(g.node()));
        return true;
    }

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
     * @pre The calling thread holds a pin, and must keep holding it for as long as it intends
     *      to deref the returned handle. Reclamation here is epoch-based, so holding a handle
     *      outside a pin is meaningless: nothing would stop the stage advancing past the point
     *      where the segment behind it becomes reusable. The pool takes its own pin for the
     *      rotation regardless -- see below -- but that one ends when acquire() returns and
     *      protects nothing afterwards.
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
     *       need it either: a rotation sweeps the flipping bucket into the cache, which both exits
     *       below draw from.
     *
     * @note **The reuse cache is read before the pin is taken, deliberately.** The cache sits
     *       outside the rotation, so nothing in it is waiting on an epoch: its entries are
     *       either discards, which were never published and so never reachable by another
     *       thread, or slots a rotation swept out of the flipping bucket -- and that sweep only
     *       runs once a scan has established no pin can still name them. A pin makes neither
     *       case safer, so requiring one charged the common path a thread-local lookup and a
     *       stage read for a guarantee it was not using.
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
        std::size_t idx = 0;

        // Fast path, and deliberately *before* the pin: the reuse cache sits outside the
        // rotation entirely, so a slot taken from it owes nothing to any epoch. Everything in
        // there is either a discard -- never published, so no thread can hold a reference --
        // or a slot a rotation swept out of the flipping bucket, which by then had already
        // served its grace period. Neither becomes safer for being pinned.
        //
        // Coupling the two cost the common case a thread-local lookup and a stage read for a
        // guarantee it was not using. A cache hit is now a single ABA-safe CAS.
        if (cache_.dequeue(idx)) return reopened(idx);

        // Slow path. From here on the phased buckets *are* the rotation, and which bucket is
        // `free` is a function of this thread's published stage -- so a pin is genuinely
        // required, and stays required for the whole loop.
        auto_pin pinned_here{*this};
        // Fixed for the whole call: see the note above on not re-publishing the pin.
        const uint8_t p = pin_stage();

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
        return reopened(idx);
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
        /// initialze the source as unmarked epoch
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
    /**
     * @brief Hand slot @p idx out: reset it, and stamp a fresh version onto the handle.
     *
     * Shared by both exits of acquire(). Needs no pin -- the index has just been taken out of
     * a container, so this thread owns it exclusively and nothing else can reach the segment
     * until the handle is published.
     */
    handle reopened(std::size_t idx) noexcept {
        [[maybe_unused]] const bool ok = segments_[idx]->reopen();
        assert(ok && "Pool::acquire: a segment that should have reopened did not");
        return make_handle(idx);
    }

    bool take(uint8_t p, std::size_t& idx) noexcept {
        if (bucket(rotate(p, 2)).dequeue(idx)) return true;
        // Look in the cache once more. Between entering acquire() and this miss, another
        // thread may have discard()ed a segment that was never published, or finished a
        // rotation -- whose sweep empties the whole flipping bucket into the cache. Both are
        // ordinary rather than exotic, and a miss on an empty CacheRing is a load and a
        // compare.
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
        // Two early-outs, both ahead of the scan, because the scan is the expensive part: it
        // reads one seq_cst line per registered thread, and every thread in acquire() runs it
        // in a loop. Neither changes which rotations are legal -- both conditions are re-checked
        // below, under the claim -- they only stop this thread paying for a scan whose outcome
        // is already decided.
        //
        //  * The rotation already moved past `s`. `s` is the caller's *pinned* stage, fixed for
        //    the whole acquire() call, so once anyone else rotates every remaining attempt in
        //    that call is asking to advance a stage that is no longer current, and the claim
        //    below would reject it after a full scan. This is the common case by a wide margin:
        //    instrumented at 4 producers / 4 consumers, it accounted for the bulk of ~61 scans
        //    per successful acquire.
        //  * Somebody else holds the rotation. Only one thread can drain, so a scan here can at
        //    best duplicate work the winner is already doing.
        if (stage() != s) return false;
        if (rotating_.load(std::memory_order_relaxed)) return false;

        // Ask the thread that refused us last time, before asking everybody. A straggler tends
        // to stay a straggler for its whole traversal while every other thread hammers this
        // function, so the same one usually refuses again -- and one load settles it instead of
        // a walk. Measured at 4 producers / 4 consumers, this absorbs ~93% of the calls that
        // would otherwise scan.
        //
        // Three things make it sound, and all three are load-bearing:
        //
        //  1. *It can only refuse.* A rotation is permitted by the full scan below and by
        //     nothing else, so a stale or outright wrong hint costs a missed opportunity and
        //     can never authorise an illegal advance. Hence the relaxed load.
        //  2. *The pointer stays dereferenceable.* ThreadRegistry recycles detached nodes
        //     through its free list and only deletes them in its destructor, which runs after
        //     this pool's -- the registry is a member below.
        //  3. *A detached node cannot masquerade as a blocker.* This is the subtle one, because
        //     reading the payload directly bypasses the is_active() filter that
        //     ThreadRegistry::all_of applies, and detached nodes keep their payload. What rules
        //     it out is that `state` is only ever non-zero while a guard is alive: ~guard
        //     stores 0 unconditionally. So a detached node reads unpinned and the hint falls
        //     through to the scan. A hint that does read pinned-at-an-older-stage therefore
        //     names a live pin -- possibly a *different* thread that has since taken the node
        //     over, which is an equally genuine blocker, so refusing is still right.
        //
        // Liveness follows from (3): the hint only sticks while some thread really is pinned
        // behind the rotation, which is exactly when the full scan would refuse anyway.
        if (const ThreadData* hint = blocker_.load(std::memory_order_relaxed)) {
            const uint8_t st = hint->state.load(std::memory_order_seq_cst);
            if ((st & kPinned) != 0 && (st & kStageMask) != s) return false;
        }

        // Short-circuits on the first thread that is behind, which is the common reason to
        // refuse: no point asking the rest. Whoever that is gets remembered as the hint above.
        const ThreadData* found = nullptr;
        const bool everyone_current = registry_.all_of([s, &found](const ThreadData& d) noexcept {
            const uint8_t st = d.state.load(std::memory_order_seq_cst);
            if ((st & kPinned) == 0) return true;      // not pinned: holds nothing
            if ((st & kStageMask) == s) return true;   // pinned: must be at this stage
            found = &d;
            return false;
        });
        if (!everyone_current) {
            blocker_.store(found, std::memory_order_relaxed);
            return false;
        }

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

    /// Last thread observed holding the rotation back; a hint for try_advance, never a
    /// substitute for the scan. See there for why an out-of-date value is harmless.
    ///
    /// On its own line: it is written by every failing scan and read by every try_advance, so
    /// sharing with the registry head that those same scans walk would be false sharing between
    /// the two hottest accesses in the rotation.
    CACHE_ALIGN mutable std::atomic<const ThreadData*> blocker_{nullptr};
    CACHE_PAD(std::atomic<const ThreadData*>);

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
    CACHE_ALIGN std::atomic<uint8_t> stage_{0};
    /// Held across a rotation so the drain of the flipping bucket finishes before the new
    /// stage is visible. Rotations are rare -- only when a thread finds nothing to acquire.
    std::atomic<bool> rotating_{false};
    CACHE_PAD(std::atomic<uint8_t>, std::atomic<bool>);
};

} // namespace mem::source
