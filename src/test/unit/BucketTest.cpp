/**
 * @file BucketTest.cpp
 * @brief The two index buckets an epoch reclaimer is built from.
 *
 * Both exist because a general MPMC ring is more machinery than an epoch rotation needs, and
 * both buy that by taking on a precondition instead:
 *
 *  - `algo::PhasedBucket` assumes a bucket is never filled and drained at the same time, which
 *    is what lets both ends be a single `fetch_add` with no per-cell sequence word. What has
 *    to hold is that a fill phase followed by a drain phase yields every value exactly once,
 *    and that the phase flip works whether the reset is lazy or explicit.
 *  - `algo::CacheRing` folds the lap into every cell so a single-word CAS is ABA-safe. What has
 *    to hold is that cells stay distinguishable across many laps -- a ring that reused a word
 *    would hand the same slot to two callers, and it would take thousands of wraps to show.
 *
 * Deliberately deterministic: the phase invariant is a *caller* obligation, so a test that
 * interleaved producers and consumers would be testing a contract violation. The concurrent
 * behaviour is exercised separately.
 */
#include <gtest/gtest.h>

#include <algo/CacheRing.hpp>
#include <algo/PhasedBucket.hpp>

#include <set>
#include <vector>

namespace {

// ===== PhasedBucket ==========================================================

using Bucket = algo::PhasedBucket<8>;
using ManualBucket =
    algo::PhasedBucket<8, meta::OptionsPack<algo::PhasedBucketOpt::no_implicit_reset>>;

TEST(PhasedBucket, StartsEmpty) {
    Bucket b;
    std::size_t v = 0;
    EXPECT_TRUE(b.empty());
    EXPECT_EQ(b.size(), 0u);
    EXPECT_FALSE(b.dequeue(v));
}

TEST(PhasedBucket, AFillPhaseFollowedByADrainYieldsEveryValueOnce) {
    Bucket b;
    for (std::size_t i = 0; i < 8; ++i) b.enqueue(i);
    EXPECT_EQ(b.size(), 8u);

    std::set<std::size_t> got;
    std::size_t v = 0;
    while (b.dequeue(v)) got.insert(v);
    EXPECT_EQ(got.size(), 8u) << "a value was lost or handed out twice";
    EXPECT_TRUE(b.empty());
}

TEST(PhasedBucket, TheImplicitResetSurvivesRepeatedPhaseFlips) {
    // The lazy reset is the part that is easy to get wrong: each phase leaves one half of the
    // packed state dirty, and the first operation of the next phase has to notice.
    Bucket b;
    for (int cycle = 0; cycle < 32; ++cycle) {
        for (std::size_t i = 0; i < 8; ++i) b.enqueue(i);
        ASSERT_EQ(b.size(), 8u) << "cycle " << cycle;

        std::size_t v = 0;
        int drained = 0;
        while (b.dequeue(v)) ++drained;
        ASSERT_EQ(drained, 8) << "cycle " << cycle << ": the bucket did not refill cleanly";
    }
}

TEST(PhasedBucket, WithoutTheImplicitResetTheOwnerFlipsThePhase) {
    ManualBucket b;
    for (std::size_t i = 0; i < 8; ++i) b.enqueue(i);

    std::size_t v = 0;
    int drained = 0;
    while (b.dequeue(v)) ++drained;
    ASSERT_EQ(drained, 8);

    b.reset(); // the owner's job now -- an epoch advance has exactly one winner to do it
    for (std::size_t i = 0; i < 8; ++i) b.enqueue(i);
    drained = 0;
    while (b.dequeue(v)) ++drained;
    EXPECT_EQ(drained, 8) << "an explicit reset did not start a fresh fill phase";
}

TEST(PhasedBucket, DrainingPastTheEndKeepsReportingEmpty) {
    // dequeue() advances the head unconditionally, so an over-eager consumer runs past the
    // array; the bounds check is what stops that being a read out of range.
    Bucket b;
    b.enqueue(3);
    std::size_t v = 0;
    ASSERT_TRUE(b.dequeue(v));
    EXPECT_EQ(v, 3u);
    for (int i = 0; i < 64; ++i) EXPECT_FALSE(b.dequeue(v));
}

// ===== CacheRing =============================================================

using Ring = algo::CacheRing<8>;

TEST(CacheRing, StartsEmptyAndFillsToCapacity) {
    Ring r;
    std::size_t v = 0;
    EXPECT_TRUE(r.empty());
    EXPECT_FALSE(r.dequeue(v));

    for (std::size_t i = 0; i < 8; ++i) EXPECT_TRUE(r.enqueue(i));
    EXPECT_EQ(r.size(), 8u);
    EXPECT_FALSE(r.enqueue(0)) << "accepted a value past capacity";
}

TEST(CacheRing, IsFirstInFirstOut) {
    Ring r;
    for (std::size_t i = 0; i < 8; ++i) ASSERT_TRUE(r.enqueue(i));
    for (std::size_t i = 0; i < 8; ++i) {
        std::size_t v = 0;
        ASSERT_TRUE(r.dequeue(v));
        EXPECT_EQ(v, i);
    }
}

TEST(CacheRing, CellsStayDistinguishableAcrossManyLaps) {
    // The whole reason the lap is folded into the cell: without it a single-word CAS cannot
    // tell "still the empty I read" from "empty again, one lap later", and the ring hands the
    // same slot to two callers. It takes a few thousand wraps for that to surface.
    Ring r;
    for (int lap = 0; lap < 4096; ++lap) {
        for (std::size_t i = 0; i < 8; ++i) ASSERT_TRUE(r.enqueue(i)) << "lap " << lap;
        for (std::size_t i = 0; i < 8; ++i) {
            std::size_t v = 0;
            ASSERT_TRUE(r.dequeue(v)) << "lap " << lap;
            ASSERT_EQ(v, i) << "lap " << lap;
        }
    }
}

TEST(CacheRing, ANonPowerOfTwoCapacityRoundsUp) {
    // The wrap is a mask and the lap a shift, so the allocation is rounded; the *contract* is
    // still that values stay below the requested capacity.
    algo::CacheRing<5> r;
    EXPECT_EQ(r.capacity(), 8u);
    for (std::size_t i = 0; i < 5; ++i) ASSERT_TRUE(r.enqueue(i));
    for (std::size_t i = 0; i < 5; ++i) {
        std::size_t v = 0;
        ASSERT_TRUE(r.dequeue(v));
        EXPECT_EQ(v, i);
    }
}

TEST(CacheRing, PaddingIsOptional) {
    using Packed = algo::CacheRing<8, meta::OptionsPack<algo::CacheRingOpt::no_cell_padding>>;
    EXPECT_LT(sizeof(Packed), sizeof(Ring)) << "no_cell_padding did not shrink the ring";

    Packed r;
    for (std::size_t i = 0; i < 8; ++i) ASSERT_TRUE(r.enqueue(i));
    std::size_t v = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        ASSERT_TRUE(r.dequeue(v));
        EXPECT_EQ(v, i);
    }
}

} // namespace
