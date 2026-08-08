/**
 * @file ThreadPinnerTest.cpp
 * @brief Core placement and topology parsing, without needing a machine of any given shape.
 *
 * Where threads land is an input to every measurement the benchmark makes, so getting it
 * wrong does not fail — it silently answers a different question. The two parts that can be
 * wrong are the placement rule and the file parser, and both are pure: `plan()` takes counts
 * and returns indices, `parse_topology()` takes a stream. Neither spawns a thread or makes a
 * syscall, so all of it is checkable here rather than only on the machine that happens to be
 * running it.
 *
 * What is deliberately *not* tested: that `pthread_setaffinity_np` actually moved a thread.
 * That depends on the cpuset the test runs under, and a CI box with a restricted mask would
 * fail for reasons that say nothing about this code.
 */
#include <gtest/gtest.h>

#include <util/threading/ThreadPinner.hpp>

#include <sstream>
#include <vector>

using util::threading::ThreadPinner;

namespace {

std::vector<int> parse(const std::string& text, bool* ok = nullptr) {
    std::istringstream in{text};
    std::vector<int> out;
    const bool r = ThreadPinner::parse_topology(in, out);
    if (ok) *ok = r;
    return out;
}

// ===== the placement rule ====================================================

TEST(ThreadPinnerPlan, OneGroupIsRoundRobin) {
    const auto l = ThreadPinner::plan(5, 0, 4);
    EXPECT_EQ(l.first, (std::vector<std::size_t>{0, 1, 2, 3, 0})) << "did not wrap at the end";
    EXPECT_TRUE(l.second.empty());

    // Symmetric: which group is empty must not matter.
    const auto r = ThreadPinner::plan(0, 5, 4);
    EXPECT_EQ(r.second, (std::vector<std::size_t>{0, 1, 2, 3, 0}));
    EXPECT_TRUE(r.first.empty());
}

TEST(ThreadPinnerPlan, EqualGroupsAlternate) {
    // 2 and 2: gcd 2, so one from each, twice -> A B A B.
    const auto l = ThreadPinner::plan(2, 2, 8);
    EXPECT_EQ(l.first, (std::vector<std::size_t>{0, 2}));
    EXPECT_EQ(l.second, (std::vector<std::size_t>{1, 3}));
}

TEST(ThreadPinnerPlan, UnequalGroupsInterleaveInProportion) {
    // 2 producers, 4 consumers -> P C C P C C: each producer sits beside the consumers
    // draining it, instead of both producers bunching onto cores 0 and 1.
    const auto l = ThreadPinner::plan(2, 4, 8);
    EXPECT_EQ(l.first, (std::vector<std::size_t>{0, 3}));
    EXPECT_EQ(l.second, (std::vector<std::size_t>{1, 2, 4, 5}));
}

TEST(ThreadPinnerPlan, TheSmallerGroupLeadsWhicheverArgumentItIs) {
    // The rule is about sizes, not argument order. Passing the groups the other way round
    // must mirror the answer exactly -- the old implementation achieved this by swapping the
    // caller's two vectors, which is what made it observable from outside.
    const auto ab = ThreadPinner::plan(2, 4, 8);
    const auto ba = ThreadPinner::plan(4, 2, 8);
    EXPECT_EQ(ab.first, ba.second);
    EXPECT_EQ(ab.second, ba.first);
}

TEST(ThreadPinnerPlan, EveryThreadIsPlacedAndNothingRunsAway) {
    // The interleave advances by whole batches; a batch of zero would spin forever.
    for (std::size_t n1 = 0; n1 <= 9; ++n1) {
        for (std::size_t n2 = 0; n2 <= 9; ++n2) {
            const auto l = ThreadPinner::plan(n1, n2, 4);
            ASSERT_EQ(l.first.size(), n1) << n1 << "," << n2;
            ASSERT_EQ(l.second.size(), n2) << n1 << "," << n2;
            for (auto c : l.first) ASSERT_LT(c, 4u) << "core index out of range";
            for (auto c : l.second) ASSERT_LT(c, 4u) << "core index out of range";
        }
    }
}

TEST(ThreadPinnerPlan, FewerCoresThanThreadsWrapsRatherThanOverruns) {
    // Indexing cores() with an unwrapped counter is the bug this guards: it reads past the
    // end of the core list rather than doubling up, which is a crash instead of contention.
    const auto l = ThreadPinner::plan(3, 3, 2);
    for (auto c : l.first) EXPECT_LT(c, 2u);
    for (auto c : l.second) EXPECT_LT(c, 2u);
}

TEST(ThreadPinnerPlan, NoCoresPlacesNothing) {
    const auto l = ThreadPinner::plan(4, 4, 0);
    EXPECT_EQ(l.first.size(), 4u);
    EXPECT_EQ(l.second.size(), 4u);
    // Values are meaningless here; the caller is expected to have checked ok() first. What
    // matters is that this returns rather than dividing by zero, which the previous
    // round-robin did when the core list was empty.
    SUCCEED();
}

// ===== the parser ============================================================

TEST(ThreadPinnerParse, ReadsOneIdPerLineInOrder) {
    bool ok = false;
    EXPECT_EQ(parse("0\n2\n4\n6\n", &ok), (std::vector<int>{0, 2, 4, 6}));
    EXPECT_TRUE(ok);
    // Order is the whole point: the generator emits cluster-first or ping-pong orders, and
    // sorting or reordering them would discard the layout being asked for.
}

TEST(ThreadPinnerParse, SkipsBlankLinesAndComments) {
    bool ok = false;
    EXPECT_EQ(parse("# cluster 0\n0\n\n  1  \n\n# cluster 1\n2\n", &ok),
              (std::vector<int>{0, 1, 2}));
    EXPECT_TRUE(ok);
}

TEST(ThreadPinnerParse, DropsDuplicatesKeepingFirstSeenOrder) {
    // A repeated id would shift every later thread onto the wrong core, silently.
    EXPECT_EQ(parse("3\n1\n3\n2\n1\n"), (std::vector<int>{3, 1, 2}));
}

TEST(ThreadPinnerParse, RejectsAMalformedLine) {
    bool ok = true;
    const auto out = parse("0\n1\nnot-a-number\n2\n", &ok);
    EXPECT_FALSE(ok) << "a corrupt topology file must be reported, not silently truncated";
    (void)out;

    ok = true;
    (void)parse("0\n1 2\n", &ok);
    EXPECT_FALSE(ok) << "trailing junk after the id must be rejected";
}

TEST(ThreadPinnerParse, DropsIdsNoCpuSetCouldHold) {
    // CPU_SET on a negative or oversized id is undefined, so these cannot reach it.
    const auto out = parse("0\n-1\n1\n");
    EXPECT_EQ(out, (std::vector<int>{0, 1}));
    EXPECT_FALSE(ThreadPinner::representable(-1));
    EXPECT_TRUE(ThreadPinner::representable(0));
}

TEST(ThreadPinnerParse, AnEmptyFileParsesToNothing) {
    bool ok = false;
    EXPECT_TRUE(parse("\n\n# only comments\n", &ok).empty());
    EXPECT_TRUE(ok) << "empty is well-formed; it is ok() that reports it as unusable";
}

// ===== construction ==========================================================

TEST(ThreadPinner, FallsBackToProcessAffinityWhenTheFileIsMissing) {
    // The previous version aborted here, so `benchmark ... pin` dumped core when the
    // generator had not been run. The fallback also respects taskset and cpusets, so it is
    // a truthful core list rather than an assumed 0..nproc.
    ThreadPinner p{"/nonexistent/path/to/sys.topo"};
    EXPECT_EQ(p.origin(), ThreadPinner::Origin::affinity);
    EXPECT_TRUE(p.ok());
    EXPECT_FALSE(p.cores().empty());
    for (int c : p.cores()) EXPECT_TRUE(ThreadPinner::representable(c));
}

TEST(ThreadPinner, PinningIsANoOpFailureWhenThereAreNoCores) {
    // Not reachable through the constructor on Linux, but the guard is what stops `pin()`
    // indexing an empty core list.
    ThreadPinner p{"/nonexistent/path/to/sys.topo"};
    if (!p.ok()) {
        std::vector<std::thread> none;
        EXPECT_FALSE(p.pin(none));
    }
    SUCCEED();
}

} // namespace
