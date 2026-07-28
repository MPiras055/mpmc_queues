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

#include <set>
#include <vector>

namespace {

struct Data {
    uint64_t seq;
};
using Item = Data*;

using Seg = seg::Vyukov<Item, meta::EmptyOptions, mem::IndexHandle>;
constexpr std::size_t kPool = 4;
using TestPool = mem::source::Pool<Seg, kPool>;

/// A pool with the calling thread registered, as every user of it must be.
struct Fixture : ::testing::Test {
    TestPool pool{/*max_threads=*/4, /*segment_capacity=*/16};
    void SetUp() override { ASSERT_TRUE(pool.register_thread()); }
    void TearDown() override { pool.unregister_thread(); }

    /// Drain the free list, returning the handles.
    std::vector<mem::VersionedIndex> take_all() {
        std::vector<mem::VersionedIndex> out;
        while (auto h = pool.acquire()) out.push_back(*h);
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
    EXPECT_FALSE(pool.acquire().has_value());
}

TEST_F(Fixture, DiscardReturnsASlotImmediately) {
    auto h = pool.acquire();
    ASSERT_TRUE(h);
    const auto before = pool.free_count();
    pool.discard(*h); // never published, so nothing can be observing it
    EXPECT_EQ(pool.free_count(), before + 1);
}

TEST_F(Fixture, RetireDoesNotReturnASlotImmediately) {
    auto h = pool.acquire();
    ASSERT_TRUE(h);
    const auto before = pool.free_count();
    pool.retire(*h);
    // It went to the staging bucket, not the free one: a reader pinned before the retire
    // may still be looking at it.
    EXPECT_EQ(pool.free_count(), before) << "a retired slot became available at once";
}

TEST_F(Fixture, RetiredSlotBecomesAvailableAfterEnoughAdvances) {
    auto h = pool.acquire();
    ASSERT_TRUE(h);
    const auto drained = take_all(); // exhaust the rest
    ASSERT_EQ(pool.free_count(), 0u);

    pool.retire(*h);
    // free_count() is used throughout rather than acquire(): acquire() advances the epoch
    // itself when it finds nothing free, which would move the very state under test.
    EXPECT_EQ(pool.free_count(), 0u) << "a retirement was reusable immediately";

    // One advance moves it from the current bucket to grace -- still not reusable.
    ASSERT_TRUE(pool.try_advance_epoch());
    EXPECT_EQ(pool.free_count(), 0u)
        << "released after a single advance; a reader one epoch behind could still hold it";

    // The second advance drains it into the free list.
    ASSERT_TRUE(pool.try_advance_epoch());
    ASSERT_EQ(pool.free_count(), 1u) << "never became reusable";

    auto again = pool.acquire();
    ASSERT_TRUE(again);
    EXPECT_EQ(again->index(), h->index());
}

TEST_F(Fixture, EpochCannotAdvancePastALivePin) {
    // This is the property everything else rests on.
    const auto e0 = pool.epoch();
    {
        auto g = pool.pin();
        // The pin published e0, so the first advance is legal: everyone active is current.
        ASSERT_TRUE(pool.try_advance_epoch());
        EXPECT_EQ(pool.epoch(), e0 + 1);
        // A second would strand this pin one epoch behind, so it must be refused.
        EXPECT_FALSE(pool.try_advance_epoch())
            << "advanced past a thread still pinned at an older epoch";
        EXPECT_EQ(pool.epoch(), e0 + 1);
    }
    // Once the pin is released nothing is holding the epoch back.
    EXPECT_TRUE(pool.try_advance_epoch());
    EXPECT_EQ(pool.epoch(), e0 + 2);
}

TEST_F(Fixture, SlotRetiredUnderAPinIsNotReusedWhileThatPinLives) {
    auto h = pool.acquire();
    ASSERT_TRUE(h);
    const auto rest = take_all();
    ASSERT_FALSE(pool.acquire().has_value());

    {
        auto g = pool.pin();
        pool.retire(*h);
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

TEST_F(Fixture, VersionChangesOnEveryReuseOfASlot) {
    // The version half of the handle is what stops a stale `next` aliasing a live
    // segment after the slot is recycled.
    auto first = pool.acquire();
    ASSERT_TRUE(first);
    const uint32_t idx = first->index();

    std::set<uint32_t> versions{first->version()};
    for (int cycle = 0; cycle < 8; ++cycle) {
        pool.discard(*first); // straight back to free, so we get the same slot again
        auto again = pool.acquire();
        ASSERT_TRUE(again);
        if (again->index() == idx) versions.insert(again->version());
        first = again;
    }
    EXPECT_GT(versions.size(), 1u) << "the handle version never changed across reuses";
    EXPECT_NE(mem::VersionedIndex{}, *first) << "a live handle must not equal nil";
}

TEST_F(Fixture, NoSlotIsLostOverManyCycles) {
    // A leak here shows up as a pool that quietly shrinks until the proxy can never
    // link another segment.
    for (int cycle = 0; cycle < 50; ++cycle) {
        auto held = take_all();
        ASSERT_EQ(held.size(), kPool) << "pool shrank by cycle " << cycle;
        for (auto h : held) pool.retire(h);
        // Two advances with nothing pinned returns the whole generation.
        ASSERT_TRUE(pool.try_advance_epoch());
        ASSERT_TRUE(pool.try_advance_epoch());
    }
    EXPECT_EQ(take_all().size(), kPool);
}

TEST_F(Fixture, DerefIsStableForAGivenIndex) {
    auto a = pool.acquire();
    ASSERT_TRUE(a);
    Seg* first = pool.deref(*a);
    ASSERT_NE(first, nullptr);
    pool.discard(*a);

    auto b = pool.acquire();
    ASSERT_TRUE(b);
    if (b->index() == a->index())
        EXPECT_EQ(pool.deref(*b), first) << "the same index resolved to a different segment";
}

/// The pool only accepts segments that can actually be reset; see segment_traits.
TEST(PoolConstraints, RejectsNonRecyclableSegmentsAtCompileTime) {
    static_assert(!core::segment_traits<seg::FAAArray<Item>>::recyclable);
    static_assert(core::segment_traits<Seg>::recyclable);
    // mem::source::Pool<seg::FAAArray<Item>, 4> is a static_assert failure by construction;
    // asserting the trait it keys off is the runnable half of that guarantee.
    SUCCEED();
}

} // namespace
