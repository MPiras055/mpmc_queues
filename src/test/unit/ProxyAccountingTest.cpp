/**
 * @file ProxyAccountingTest.cpp
 * @brief What the proxy still knows after a thread has gone.
 *
 * `LinkedProxy::size()` is a fold over per-thread counters living in the source's registry
 * nodes, and a scan visits only *attached* threads. So the interesting question is what
 * happens at the seam: a worker enqueues, finishes, detaches — and its items are still in the
 * queue, but the thread that counted them is not.
 *
 * Every other suite holds one session for the whole test and asks the thread that pushed, so
 * none of them can see this. These deliberately do not: each spawns a worker, joins it, and
 * only then asks. That makes them ordinary deterministic tests rather than concurrency
 * stress — the worker is finished before anything is asserted.
 *
 * The second half is the same hole from the other side. A detached thread's node is recycled,
 * and whoever inherits it inherits its payload: leftover `ops` would double-count, and a
 * leftover `last_seen` would make the close hint fire against a tail this thread never saw
 * closed.
 */
#include <gtest/gtest.h>

#include <algo/Vyukov.hpp>
#include <proxy/Aliases.hpp>

#include <thread>
#include <vector>

namespace {

struct Data {
    uint64_t seq;
};
using Item = Data*;

using Seg = seg::Vyukov<Item>;
using Q = proxy::Unbounded<Item, Seg>;

constexpr std::size_t kSegment = 8;

/// Run @p body on a fresh thread that joins the queue, and wait for it to finish and detach.
template <typename Body>
void in_a_worker(Q& q, Body body) {
    std::thread t{[&] {
        auto joined = q.join();
        ASSERT_TRUE(joined);
        body();
    }};
    t.join();
}

TEST(ProxyAccounting, ItemsSurviveTheThreadThatEnqueuedThem) {
    Q q{kSegment};
    std::vector<Data> store(40);
    for (std::size_t i = 0; i < store.size(); ++i) store[i] = {i + 1};

    in_a_worker(q, [&] {
        for (auto& d : store) ASSERT_TRUE(q.enqueue(&d));
    });

    // The producer is gone; its items are not. A size() that folds only over attached
    // threads reports 0 here, and stays wrong for the life of the queue.
    EXPECT_EQ(q.size(), store.size())
        << "the producer's count left with the producer";
}

TEST(ProxyAccounting, ADetachedProducerAndAttachedConsumerAgree) {
    Q q{kSegment};
    std::vector<Data> store(24);
    for (std::size_t i = 0; i < store.size(); ++i) store[i] = {i + 1};

    in_a_worker(q, [&] {
        for (auto& d : store) ASSERT_TRUE(q.enqueue(&d));
    });
    ASSERT_EQ(q.size(), store.size());

    // Drain half from a different thread, which was never the producer.
    constexpr std::size_t kTaken = 10;
    in_a_worker(q, [&] {
        Item out = nullptr;
        for (std::size_t i = 0; i < kTaken; ++i) ASSERT_TRUE(q.dequeue(out));
    });

    EXPECT_EQ(q.size(), store.size() - kTaken)
        << "the balance is wrong once producer and consumer are different, departed threads";
}

TEST(ProxyAccounting, CountsDoNotAccumulateAcrossManyShortLivedThreads) {
    // A worker pool that hands the queue between short-lived threads is the shape that
    // exposes both halves at once: each departure has to be folded in exactly once, and each
    // arrival has to start from zero rather than inheriting the last owner's tally.
    Q q{kSegment};
    std::vector<Data> store(60);
    for (std::size_t i = 0; i < store.size(); ++i) store[i] = {i + 1};

    std::size_t pushed = 0;
    for (int round = 0; round < 6; ++round) {
        in_a_worker(q, [&] {
            for (int k = 0; k < 10; ++k) ASSERT_TRUE(q.enqueue(&store[pushed + k]));
        });
        pushed += 10;
        ASSERT_EQ(q.size(), pushed) << "after round " << round;
    }

    // And back down again, one departing consumer per round.
    for (int round = 0; round < 6; ++round) {
        in_a_worker(q, [&] {
            Item out = nullptr;
            for (int k = 0; k < 10; ++k) ASSERT_TRUE(q.dequeue(out));
        });
        pushed -= 10;
        ASSERT_EQ(q.size(), pushed) << "after drain round " << round;
    }
    EXPECT_EQ(q.size(), 0u);
}

TEST(ProxyAccounting, AnInheritedNodeDoesNotCarryTheLastOwnersCloseHint) {
    // A segment-bounded proxy over a small segment forces the close hint into play: a
    // producer that finds the tail full records it in `last_seen`, and the next enqueue on
    // that same tail is told to skip the segment's own loop. If that field survives into the
    // next thread to inherit the node, it will skip a segment it has never seen closed.
    using Bounded = proxy::ChunkBounded<Item, Seg>;
    Bounded q{/*segment_capacity=*/2, /*chunks=*/8};

    // Eight items over 2-slot segments crosses four segment boundaries, so the first worker
    // certainly records a full tail -- while staying well inside the 8-segment bound, so the
    // second worker's refusal could only come from the inherited hint, not from admission.
    std::vector<Data> store(8);
    for (std::size_t i = 0; i < store.size(); ++i) store[i] = {i + 1};

    std::size_t placed = 0;
    {
        std::thread t{[&] {
            auto joined = q.join();
            ASSERT_TRUE(joined);
            for (auto& d : store)
                if (q.enqueue(&d)) ++placed;
        }};
        t.join();
    }
    ASSERT_GT(placed, 2u) << "the first worker never crossed a segment boundary";

    // A second worker inherits the recycled node. It must be able to enqueue immediately.
    bool second_ok = false;
    std::vector<Data> more(4);
    for (std::size_t i = 0; i < more.size(); ++i) more[i] = {100 + i};
    {
        std::thread t{[&] {
            auto joined = q.join();
            ASSERT_TRUE(joined);
            second_ok = q.enqueue(&more[0]);
        }};
        t.join();
    }
    EXPECT_TRUE(second_ok) << "the inherited close hint refused a tail this thread never saw";
    EXPECT_EQ(q.size(), placed + 1);
}

} // namespace
