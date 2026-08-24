/**
 * @file PoolReclamationTest.cpp
 * @brief The pooled source's epoch machine, driven deterministically.
 *
 * `mem::source::Pool` now owns its reclamation outright rather than delegating to the old
 * Recycler. Reclamation is the one place in this tree where a mistake is silent and
 * catastrophic — a slot handed back while somebody still holds it produces corruption far
 * from the cause — so the safety property is pinned down here from a single thread, where
 * the epoch can be stepped by hand and every claim is checkable.
 *
 * The property being asserted throughout is the one that makes epoch reclamation correct:
 *
 *     a slot retired while a pin was live is not handed out again until that pin is gone
 *     and the epoch has advanced far enough that no reader can still be looking at it.
 *
 * This is not a substitute for running the multi-threaded suite; it is the part of the
 * argument that can be made deterministically.
 */
#include <gtest/gtest.h>

#include <algo/FAAArray.hpp>
#include <algo/Vyukov.hpp>
#include <core/Source.hpp>
#include <mem/source/Hazard.hpp>
#include <mem/source/Pool.hpp>

#include <atomic>
#include <optional>
#include <set>
#include <thread>
#include <vector>

namespace {

struct Data {
    uint64_t seq;
};
using Item = Data*;

constexpr std::size_t kPool = 4;
using Seg = seg::Vyukov<Item, meta::EmptyOptions, mem::IndexHandle<kPool>>;
using TestPool = mem::source::Pool<Seg, kPool>;

/// A pool with the calling thread registered, as every user of it must be.
struct Fixture : ::testing::Test {
    TestPool pool{/*segment_capacity=*/16};
    /// Declared after `pool` so it is destroyed first: the thread detaches before the
    /// registry that owns its node does.
    TestPool::session joined{};

    void SetUp() override {
        joined = pool.join();
        ASSERT_TRUE(joined);
    }

    /// acquire() and discard() require a pin, because reclamation here is epoch-based.
    /// The pin is scoped to the call rather than held: most of what follows asserts on
    /// try_advance_epoch(), which is refused while anything is pinned behind the epoch.
    std::optional<TestPool::handle> take_one() {
        auto g = pool.pin();
        return pool.acquire();
    }

    void give_back(TestPool::handle h) {
        auto g = pool.pin();
        pool.discard(h);
    }

    void give_up(TestPool::handle h) {
        auto g = pool.pin();
        pool.retire(h);
    }

    /// Drain the free list, returning the handles.
    std::vector<TestPool::handle> take_all() {
        std::vector<TestPool::handle> out;
        while (auto h = take_one()) out.push_back(*h);
        return out;
    }
};

TEST_F(Fixture, StartsWithEverySlotAvailable) {
    const auto all = take_all();
    EXPECT_EQ(all.size(), kPool);

    std::set<uint32_t> indices;
    for (auto h : all) indices.insert(h.index());
    EXPECT_EQ(indices.size(), kPool) << "the same slot was handed out twice";
}

TEST_F(Fixture, ExhaustionIsReportedNotFabricated) {
    const auto all = take_all();
    ASSERT_EQ(all.size(), kPool);
    // The bound of a pooled proxy *is* this nullopt.
    EXPECT_FALSE(take_one().has_value());
}

TEST_F(Fixture, DiscardReturnsASlotImmediately) {
    auto h = take_one();
    ASSERT_TRUE(h);
    const auto before = pool.free_count();
    const auto cached = pool.cache_count();
    give_back(*h); // never published, so nothing can be observing it
    // Straight to the reuse cache rather than into the rotation -- a segment nobody ever saw
    // owes no grace period, and making it wait two advances would be latency for nothing.
    EXPECT_EQ(pool.free_count(), before + 1);
    EXPECT_EQ(pool.cache_count(), cached + 1) << "a discarded slot entered the rotation";
}

TEST_F(Fixture, RetireDoesNotReturnASlotImmediately) {
    auto h = take_one();
    ASSERT_TRUE(h);
    const auto before = pool.free_count();
    give_up(*h);
    // It went into `current`, two rotations away from being handed out: a reader pinned
    // before the retire may still be looking at it.
    EXPECT_EQ(pool.free_count(), before) << "a retired slot became available at once";
}

TEST_F(Fixture, RetiredSlotBecomesAvailableAfterEnoughAdvances) {
    auto h = take_one();
    ASSERT_TRUE(h);
    const auto drained = take_all(); // exhaust the rest
    ASSERT_EQ(pool.free_count(), 0u);

    give_up(*h);
    // free_count() is used throughout rather than acquire(): acquire() advances the epoch
    // itself when it finds nothing free, which would move the very state under test.
    EXPECT_EQ(pool.free_count(), 0u) << "a retirement was reusable immediately";

    // One advance moves it from current to grace -- still not reusable, because a thread
    // pinned one epoch back could have been reading it.
    ASSERT_TRUE(pool.try_advance_epoch());
    EXPECT_EQ(pool.free_count(), 0u)
        << "released after a single advance; a reader one epoch behind could still hold it";

    // The second rotates it into `free`, which is where acquire() looks.
    ASSERT_TRUE(pool.try_advance_epoch());
    ASSERT_EQ(pool.free_count(), 1u) << "never became reusable";

    auto again = take_one();
    ASSERT_TRUE(again);
    EXPECT_EQ(again->index(), h->index());
}

TEST_F(Fixture, EpochCannotAdvancePastALivePin) {
    // This is the property everything else rests on.
    // Stages wrap, so every expectation below is modulo the stage count.
    const auto e0 = pool.epoch();
    const auto next = [](uint8_t s) { return static_cast<uint8_t>((s + 1) % 4); };
    {
        auto g = pool.pin();
        // The pin published e0, so the first advance is legal: everyone active is current.
        ASSERT_TRUE(pool.try_advance_epoch());
        EXPECT_EQ(pool.epoch(), next(e0));
        // A second would strand this pin one epoch behind, so it must be refused.
        EXPECT_FALSE(pool.try_advance_epoch())
            << "advanced past a thread still pinned at an older epoch";
        EXPECT_EQ(pool.epoch(), next(e0));
    }
    // Once the pin is released nothing is holding the epoch back.
    EXPECT_TRUE(pool.try_advance_epoch());
    EXPECT_EQ(pool.epoch(), next(next(e0)));
}

TEST_F(Fixture, SlotRetiredUnderAPinIsNotReusedWhileThatPinLives) {
    auto h = take_one();
    ASSERT_TRUE(h);
    const auto rest = take_all();
    ASSERT_FALSE(take_one().has_value());

    {
        auto g = pool.pin();
        pool.retire(*h); // already pinned: must NOT take another, pin() is not re-entrant
        // Our own pin blocks the second advance, so the slot cannot complete the two hops
        // it needs while we are still able to be looking at it.
        pool.try_advance_epoch();
        pool.try_advance_epoch();
        pool.try_advance_epoch();
        EXPECT_EQ(pool.free_count(), 0u)
            << "a slot retired under a live pin became reusable";
    }

    // Pin dropped: nothing holds the epoch back any more.
    for (int i = 0; i < 4 && pool.free_count() == 0; ++i) pool.try_advance_epoch();
    EXPECT_GT(pool.free_count(), 0u) << "never recovered after the pin was dropped";
}

TEST_F(Fixture, TheRotationSurvivesManyFullCycles) {
    // The case the four-bucket rewrite is most likely to fail, and it fails silently.
    //
    // Each bucket plays every role in turn, and algo::PhasedBucket resets itself lazily: a
    // drain phase clears its tail only on a dequeue that *fails*, and a fill phase clears its
    // head on the first enqueue. Take every slot, retire every slot, rotate twice, repeat --
    // and if a bucket ever enters a fill phase with its tail where the last drain left it, it
    // starts writing past the end of its array. In a debug build that trips PhasedBucket's own
    // assertion; in a release build the pool just quietly stops handing slots back.
    for (int cycle = 0; cycle < 64; ++cycle) {
        auto held = take_all();
        ASSERT_EQ(held.size(), kPool) << "the pool shrank by cycle " << cycle;

        for (auto h : held) give_up(h);
        ASSERT_TRUE(pool.try_advance_epoch()) << "cycle " << cycle;
        ASSERT_TRUE(pool.try_advance_epoch()) << "cycle " << cycle;
        ASSERT_EQ(pool.free_count(), kPool) << "slots lost on cycle " << cycle;
    }
}

TEST_F(Fixture, MixingDiscardsAndRetiresKeepsEverySlot) {
    // The two return paths are different -- the cache and the rotation -- and a slot must end
    // up on exactly one of them. Alternating is what catches a slot going to both, or neither.
    for (int cycle = 0; cycle < 32; ++cycle) {
        auto held = take_all();
        ASSERT_EQ(held.size(), kPool) << "cycle " << cycle;

        for (std::size_t i = 0; i < held.size(); ++i) {
            if (i % 2 == 0) give_back(held[i]); // cache
            else give_up(held[i]);              // rotation
        }
        pool.try_advance_epoch();
        pool.try_advance_epoch();
        ASSERT_EQ(pool.free_count(), kPool) << "cycle " << cycle;
    }
}

TEST_F(Fixture, RotatingSweepsTheFlippingBucketRatherThanStrandingIt) {
    // `next -> current` is the one transition that flips a bucket from draining to filling,
    // and whatever is still in it at that moment is free slots that only a thread a stage
    // behind could reach. Since a rotation is refused while any such thread exists, leaving
    // them there would strand them until the rotation came round again -- which is itself
    // waiting on that bucket. The rotation therefore sweeps them into the cache on the way
    // past, exclusively and before publishing the new stage.
    const auto all = take_all();
    ASSERT_EQ(all.size(), kPool);
    for (auto h : all) give_up(h); // all four land in `current`

    // current -> grace -> free: now they are where acquire() looks.
    ASSERT_TRUE(pool.try_advance_epoch());
    ASSERT_TRUE(pool.try_advance_epoch());
    ASSERT_EQ(pool.free_count(), kPool) << "two rotations should have made them reusable";

    // free -> next: still reachable, but only via the sweep from here on.
    ASSERT_TRUE(pool.try_advance_epoch());

    // The rotation past `next` must not lose them.
    ASSERT_TRUE(pool.try_advance_epoch()) << "refused to rotate, stranding the free slots";
    EXPECT_EQ(pool.cache_count(), kPool) << "the flipping bucket was not swept";
    EXPECT_EQ(take_all().size(), kPool) << "slots were lost crossing next -> current";
}

TEST_F(Fixture, TheRotationNeverStallsWithSlotsInHand) {
    // The liveness property behind the sweep: however many times the rotation turns, a slot
    // that has been retired always becomes acquirable again. Without the sweep this deadlocks
    // -- the slots sit in `next`, no thread is behind enough to pop them, and the rotation
    // that would free them is refused because they are there.
    for (int cycle = 0; cycle < 40; ++cycle) {
        auto held = take_all();
        ASSERT_EQ(held.size(), kPool) << "the pool stalled on cycle " << cycle;
        for (auto h : held) give_up(h);
        // However far it turns, everything comes back.
        for (int k = 0; k < 4; ++k) pool.try_advance_epoch();
    }
}


TEST_F(Fixture, VersionChangesOnEveryReuseOfASlot) {
    // The version half of the handle is what stops a stale `next` aliasing a live
    // segment after the slot is recycled.
    auto first = take_one();
    ASSERT_TRUE(first);
    using Handle = TestPool::handle;
    // The split is sized by the pool: four slots need two index bits, so the version gets
    // the other 62 rather than the 32 a fixed split would have left it.
    static_assert(Handle::index_bits == 2);
    static_assert(Handle::version_bits == 62);

    const auto idx = first->index();

    std::set<Handle::version_type> versions{first->version()};
    for (int cycle = 0; cycle < 8; ++cycle) {
        give_back(*first); // straight back to free, so we get the same slot again
        auto again = take_one();
        ASSERT_TRUE(again);
        if (again->index() == idx) versions.insert(again->version());
        first = again;
    }
    EXPECT_GT(versions.size(), 1u) << "the handle version never changed across reuses";
    EXPECT_NE(TestPool::nil(), *first) << "a live handle must not equal nil";
}

TEST_F(Fixture, NoSlotIsLostOverManyCycles) {
    // A leak here shows up as a pool that quietly shrinks until the proxy can never
    // link another segment.
    for (int cycle = 0; cycle < 50; ++cycle) {
        auto held = take_all();
        ASSERT_EQ(held.size(), kPool) << "pool shrank by cycle " << cycle;
        for (auto h : held) give_up(h);
        // Two advances with nothing pinned returns the whole generation.
        ASSERT_TRUE(pool.try_advance_epoch());
        ASSERT_TRUE(pool.try_advance_epoch());
    }
    EXPECT_EQ(take_all().size(), kPool);
}

TEST_F(Fixture, DerefIsStableForAGivenIndex) {
    auto a = take_one();
    ASSERT_TRUE(a);
    Seg* first = pool.deref(*a);
    ASSERT_NE(first, nullptr);
    give_back(*a);

    auto b = take_one();
    ASSERT_TRUE(b);
    // Braced deliberately: EXPECT_EQ with a streamed message expands to an if/else, so an
    // unbraced enclosing `if` leaves that `else` ambiguously bound. Harmless today only
    // because this `if` has no else of its own -- adding one later would attach it wrongly.
    if (b->index() == a->index()) {
        EXPECT_EQ(pool.deref(*b), first) << "the same index resolved to a different segment";
    }
}

/**
 * @brief renew() moves a pinned thread forward, and does nothing when it is already current.
 *
 * The reason it exists: a pin taken at pin() and never moved holds try_advance back for the
 * whole traversal, and the traversal is longest exactly when the queue is contended -- so the
 * thread that most needs the rotation to turn is the one preventing it. Before renew() the only
 * way to move a thread's stage was to drop the pin entirely.
 *
 * Both halves matter. That it advances is the point; that it is a no-op when already current is
 * what keeps it off the fast path, since LinkedProxy calls it on every retry.
 */
TEST_F(Fixture, RenewMovesAPinnedThreadToTheCurrentStage) {
    auto g = pool.pin();
    const uint8_t pinned_at = g.stage();

    // Nothing has moved, so renew must not either.
    pool.renew(g);
    EXPECT_EQ(g.stage(), pinned_at) << "renew republished while already current";

    // Advance the rotation underneath this thread. It is the only pinned thread and it is
    // published at the current stage, so the advance is permitted.
    ASSERT_TRUE(pool.try_advance_epoch());
    ASSERT_NE(pool.epoch(), pinned_at) << "the fixture could not move the stage";

    pool.renew(g);
    EXPECT_EQ(g.stage(), pool.epoch())
        << "renew left the thread behind: it would keep blocking every other advance";
}

/**
 * @brief A thread that never renews still holds the rotation back -- the behaviour renew fixes.
 *
 * Kept as the counterpart to the test above so the mechanism is pinned from both sides: with a
 * stale pin outstanding, try_advance must refuse.
 */
TEST_F(Fixture, AStalePinBlocksTheRotation) {
    auto g = pool.pin();
    ASSERT_TRUE(pool.try_advance_epoch());   // first advance: this thread is current
    // Now this thread is a stage behind and has not renewed.
    EXPECT_FALSE(pool.try_advance_epoch())
        << "the rotation moved past a thread still pinned at an older stage";
    pool.renew(g);
    EXPECT_TRUE(pool.try_advance_epoch()) << "renewing did not unblock the rotation";
}

/**
 * @brief The reuse cache is serviceable when the rotation cannot move at all.
 *
 * This is the invariant that lets `acquire()` check the cache *before* taking a pin. The cache
 * sits outside the rotation, so a slot in it is owed no grace period by anybody, and reaching
 * it must not depend on the epoch making progress. Deliberately structured so the rotation is
 * genuinely frozen -- a second thread holds a stale pin, because `pin()` republishes at the
 * current stage and a nested pin on *this* thread would quietly unfreeze it.
 *
 * The `EXPECT_FALSE` before the discard is what gives the test its teeth: it establishes that
 * the buckets have nothing to give, so the acquire that follows can only have come from the
 * cache.
 */
TEST_F(Fixture, ACacheHitIsServiceableWhileTheRotationIsFrozen) {
    auto all = take_all();
    ASSERT_EQ(all.size(), kPool);

    std::atomic<bool> pinned{false}, release{false};
    std::thread stale{[&] {
        auto s = pool.join();
        ASSERT_TRUE(s);
        auto g = pool.pin();
        pinned.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
        (void) g;   // held, and never renewed, for the whole body below
    }};
    while (!pinned.load(std::memory_order_acquire)) std::this_thread::yield();

    // One advance succeeds -- both threads are current. That leaves the helper a stage behind,
    // and with it still pinned there the rotation is stuck until it goes away.
    ASSERT_TRUE(pool.try_advance_epoch());
    ASSERT_FALSE(pool.try_advance_epoch()) << "the rotation moved past a stale pin";

    // Nothing in the buckets is reachable, and no rotation can make anything reachable.
    EXPECT_FALSE(take_one().has_value()) << "the buckets gave up a slot with the rotation frozen";

    // A discard goes straight to the cache, bypassing the rotation entirely...
    give_back(all.back());
    all.pop_back();
    ASSERT_EQ(pool.cache_count(), 1u) << "a discarded slot entered the rotation";

    // ...so it is available immediately, frozen rotation or not.
    EXPECT_TRUE(take_one().has_value())
        << "a cached slot was unreachable because the epoch could not advance";

    release.store(true, std::memory_order_release);
    stale.join();
}

/**
 * @brief renew() reports whether protection actually moved.
 *
 * The bool is not cosmetic: LinkedProxy::enqueue gates its retry on it, using false to mean
 * "this thread was not the one holding the rotation back, so an empty pool is the real memory
 * bound rather than a convoy". A renew() that always claimed to have moved would turn every
 * genuine exhaustion into a wasted re-traversal; one that never did would disable the retry.
 */
TEST_F(Fixture, RenewReportsWhetherProtectionMoved) {
    auto g = pool.pin();
    EXPECT_FALSE(pool.renew(g)) << "a pin taken at the current stage claimed to have moved";

    ASSERT_TRUE(pool.try_advance_epoch());   // leaves this thread a stage behind
    EXPECT_TRUE(pool.renew(g)) << "a stale pin was not reported as moved";
    EXPECT_FALSE(pool.renew(g)) << "renewing twice claimed to move the second time";
}

/// The other half of the contract: a renew that returns false changed nothing, so whatever the
/// caller was already holding is still good. This is what lets the proxy skip the re-read that a
/// true return obliges it to do.
TEST_F(Fixture, ANoOpRenewLeavesExistingHandlesUsable) {
    auto g = pool.pin();
    auto h = pool.acquire();
    ASSERT_TRUE(h);
    const auto* before = pool.deref(*h);

    ASSERT_FALSE(pool.renew(g)) << "precondition: this thread is already current";
    EXPECT_EQ(pool.deref(*h), before) << "a no-op renew invalidated a live handle";
}

/**
 * @brief A hint left pointing at a departed thread must not wedge the rotation shut.
 *
 * try_advance remembers whoever refused it last and asks that thread first, which is what keeps
 * the common case off the full registry walk. Reading a payload directly bypasses the
 * is_active() filter ThreadRegistry::all_of applies, and detached nodes keep their payload -- so
 * if a node could be left reading "pinned at an older stage" after its thread was gone, the hint
 * would refuse every rotation forever and a pooled proxy would refuse every enqueue with the
 * pool sitting idle.
 *
 * What rules that out is that ~guard stores 0 unconditionally, so a departed thread always reads
 * unpinned and the hint falls through to the scan. That is an invariant of two components at
 * once, which is why it is tested rather than argued.
 */
TEST_F(Fixture, AHintNamingADepartedThreadDoesNotWedgeTheRotation) {
    std::atomic<bool> pinned{false}, release{false}, gone{false};
    std::thread straggler{[&] {
        {
            auto s = pool.join();
            ASSERT_TRUE(s);
            auto g = pool.pin();
            pinned.store(true, std::memory_order_release);
            while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
        }   // guard first, then session: the thread is unpinned and detached from here
        gone.store(true, std::memory_order_release);
    }};
    while (!pinned.load(std::memory_order_acquire)) std::this_thread::yield();

    ASSERT_TRUE(pool.try_advance_epoch());   // both current; leaves the straggler behind
    // Refused, and in refusing the straggler is recorded as the hint.
    ASSERT_FALSE(pool.try_advance_epoch());

    release.store(true, std::memory_order_release);
    while (!gone.load(std::memory_order_acquire)) std::this_thread::yield();
    straggler.join();

    // The hint still names that thread's node. If it were trusted blindly the rotation would
    // now be shut for good.
    EXPECT_TRUE(pool.try_advance_epoch())
        << "the rotation stayed blocked by a thread that has already gone";
}

/// The pool only accepts segments that can actually be reset; see segment_traits.
TEST(PoolConstraints, EverySegmentOfferedToThePoolDeclaresItselfRecyclable) {
    static_assert(core::segment_traits<Seg>::recyclable);
    // FAAArray used to be the counter-example here: its cells are write-once, so it
    // declared recyclable == false and mem::source::Pool refused it outright. Its reopen()
    // now flips a generation flag instead of sweeping the array, so it is pooled like
    // anything else -- which leaves the tree with no non-recyclable segment to point the
    // guard at. The static_assert in Pool is still there; it simply cannot be exercised
    // positively without a negative-compilation harness.
    static_assert(core::segment_traits<seg::FAAArray<Item>>::recyclable);
    SUCCEED();
}

/// Both sources still satisfy core::SegmentSource, which is where `renew() -> bool` is spelled.
/// Hazard's is a constant false -- per-handle protection has no epoch to move -- and this is
/// what keeps that from silently drifting back to void.
TEST(PoolConstraints, BothSourcesSatisfyTheRenewContract) {
    using HazSeg = seg::Vyukov<Item>;
    using Haz = mem::source::Hazard<HazSeg, mem::source::NoPayload>;
    static_assert(core::SegmentSource<Haz, HazSeg>);
    static_assert(core::SegmentSource<TestPool, Seg>);
    SUCCEED();
}

} // namespace
