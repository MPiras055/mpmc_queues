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
#include <mem/source/Pool.hpp>

#include <optional>
#include <set>
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
    if (b->index() == a->index())
        EXPECT_EQ(pool.deref(*b), first) << "the same index resolved to a different segment";
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

} // namespace
