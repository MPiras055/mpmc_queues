/**
 * @file ConcurrencyTest.cpp
 * @brief Multi-producer / multi-consumer correctness over every registered implementation.
 *
 * Checks three things per run: no item is lost, no item is duplicated, and each
 * producer's items are observed in the order that producer sent them (per-producer FIFO
 * is the strongest ordering a concurrent queue can be held to).
 *
 * Several thread shapes are exercised, not just the symmetric one. Contention profile is
 * exactly what these algorithms differ on: a single-producer ring never contends on the
 * tail, and a single consumer never contends on the head, so a symmetric 4P/4C run alone
 * hides whole classes of defect. The duplicate-item bug in pooled SCQ, for instance,
 * needed segments to be recycled often enough to be handed back out mid-flight.
 *
 * @note The bugs this catches are intermittent -- a lost-item defect in PRQ reproduced in
 *       3 runs out of 8, and a livelock in 4 of 12 -- so the run count matters more than
 *       the item count. Raise MPMC_REPEATS when investigating.
 *
 * @note Producers give up after a bounded number of refused enqueues and report a stall
 *       rather than spinning forever, so a livelock fails the test instead of hanging it.
 */
#include <gtest/gtest.h>
#include <algo/HQ.hpp>
#include <registry/Registry.hpp>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <thread>
#include <vector>

namespace {

struct Data {
    uint64_t seq;
    uint64_t tid;
};
using Item = Data*;

#ifndef MPMC_REPEATS
#define MPMC_REPEATS 3
#endif

constexpr uint64_t kItems = 2'000'000;
constexpr size_t kProducers = 4;
constexpr size_t kConsumers = 4;
/// Items used by the extra thread shapes, kept smaller so the matrix stays runnable.
constexpr uint64_t kShapeItems = 200'000;
/// Failed enqueues between checks that the queue as a whole is still moving.
///
/// The check is deliberately on *global* progress, not on this producer's own. These
/// queues are lock-free, not wait-free: the guarantee is that some thread advances, not
/// that every thread does. Counting one producer's consecutive failures declares a stall
/// whenever a thread is merely starved -- which happens routinely at 8P/1C, where eight
/// producers contend for a queue one consumer is draining, and cost a false failure in
/// roughly one run in three.
constexpr uint64_t kProgressWindow = 5'000'000ULL;

struct Outcome {
    uint64_t produced = 0;
    uint64_t consumed = 0;
    int order_violations = 0;
    bool stalled = false;
};

/**
 * @brief Run @p producers x @p consumers over @p total items and report what happened.
 *
 * Each producer stamps its items with a strictly increasing sequence number, so a
 * consumer can detect reordering within a producer's stream without needing a global
 * order the queue never promised.
 */
template <typename Q>
Outcome run_shape(uint64_t total, size_t producers, size_t consumers, size_t capacity = 1024) {
    registry::Instance<Q> inst{capacity};
    Q& q = inst.get();

    std::vector<Data> items(total);
    std::atomic<uint64_t> produced{0}, consumed{0};
    std::atomic<bool> producers_done{false}, stalled{false};
    std::atomic<int> order_violations{0};
    std::barrier sync(producers + consumers);
    std::vector<std::thread> threads;

    for (size_t p = 0; p < producers; ++p) {
        threads.emplace_back([&, p] {
            // Held for the whole lambda: the early return below used to need its own
            // `leave()`, which is precisely the pairing this replaces.
            [[maybe_unused]] auto joined = registry::Instance<Q>::session(q);
            sync.arrive_and_wait();
            uint64_t mine = 0;
            for (uint64_t i = p; i < total; i += producers) {
                items[i] = {++mine, p};
                uint64_t spins = 0;
                uint64_t progress_mark = produced.load(std::memory_order_relaxed);
                while (!q.enqueue(&items[i])) {
                    if (++spins % kProgressWindow == 0) {
                        const uint64_t now = produced.load(std::memory_order_relaxed);
                        if (now == progress_mark) { // nobody advanced: a real livelock
                            stalled.store(true, std::memory_order_relaxed);
                            return;
                        }
                        progress_mark = now;
                    }
                }
                produced.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (size_t c = 0; c < consumers; ++c) {
        threads.emplace_back([&] {
            std::vector<uint64_t> last(producers, 0);
            [[maybe_unused]] auto joined = registry::Instance<Q>::session(q);
            sync.arrive_and_wait();
            Item out = nullptr;
            const auto take = [&] {
                if (out->seq <= last[out->tid])
                    order_violations.fetch_add(1, std::memory_order_relaxed);
                last[out->tid] = out->seq;
                consumed.fetch_add(1, std::memory_order_relaxed);
            };
            while (!producers_done.load(std::memory_order_relaxed) &&
                   !stalled.load(std::memory_order_relaxed))
                if (q.dequeue(out)) take();
            // Producers are finished: whatever remains must still come out.
            while (q.dequeue(out)) take();
        });
    }

    for (size_t p = 0; p < producers; ++p) threads[p].join();
    producers_done.store(true, std::memory_order_release);
    for (size_t c = producers; c < threads.size(); ++c) threads[c].join();

    return {produced.load(), consumed.load(), order_violations.load(), stalled.load()};
}

template <typename Q>
void expect_clean(const Outcome& o, uint64_t expected, const char* what) {
    ASSERT_FALSE(o.stalled) << what << ": producers made no progress";
    EXPECT_EQ(o.produced, expected) << what;
    EXPECT_EQ(o.consumed, o.produced) << what << ": items lost or duplicated";
    EXPECT_EQ(o.order_violations, 0) << what << ": per-producer FIFO violated";
}

template <typename Q>
class Mpmc : public ::testing::Test {};

using AllTypes = registry::AsTypes<registry::All<Item>>::apply<::testing::Types>;
TYPED_TEST_SUITE(Mpmc, AllTypes, registry::TestNames<registry::All<Item>>);

TYPED_TEST(Mpmc, NoLossNoDuplicationPerProducerFifo) {
    for (int attempt = 0; attempt < MPMC_REPEATS; ++attempt) {
        const auto o = run_shape<TypeParam>(kItems, kProducers, kConsumers);
        expect_clean<TypeParam>(o, kItems, "4P/4C");
        if (::testing::Test::HasFailure()) return; // no value in repeating a known failure
    }
}

/// One producer, one consumer: no contention on either index.
TYPED_TEST(Mpmc, SingleProducerSingleConsumer) {
    expect_clean<TypeParam>(run_shape<TypeParam>(kShapeItems, 1, 1), kShapeItems, "1P/1C");
}

/// Many producers, one consumer: the tail is contended, the head is not.
TYPED_TEST(Mpmc, MultiProducerSingleConsumer) {
    expect_clean<TypeParam>(run_shape<TypeParam>(kShapeItems, 8, 1), kShapeItems, "8P/1C");
}

/// One producer, many consumers: the head is contended, and consumers routinely find the
/// queue empty -- which is the path where an over-eager "empty" answer loses items.
TYPED_TEST(Mpmc, SingleProducerMultiConsumer) {
    expect_clean<TypeParam>(run_shape<TypeParam>(kShapeItems, 1, 8), kShapeItems, "1P/8C");
}

/// More threads than a segment holds, so segments are linked and retired constantly.
/// This is the shape that exercises reuse hardest under a pooled source.
TYPED_TEST(Mpmc, SmallSegmentsForceConstantTurnover) {
    expect_clean<TypeParam>(run_shape<TypeParam>(kShapeItems, 4, 4, /*capacity=*/16),
                            kShapeItems, "4P/4C, 16-slot segments");
}

/**
 * @brief HQ's slow path must not destroy capacity when nothing is queued behind the head.
 *
 * Not typed: this is a property of one segment's dequeue strategy, not of every registered
 * queue. HQ splits dequeue on whether a successor exists, and the slow path -- the one
 * taken while this is the only segment -- is the reason HQ exists at all. Both paths may
 * overwrite a cell a producer has claimed but not yet written, which permanently costs a
 * slot in a write-once array; the slow path is supposed to avoid that.
 *
 * It did not. A consumer that reached an empty cell waited two loads and then burned it
 * anyway, even when `tail == head + 1`, i.e. when nothing was published behind the stalled
 * index and so there was no head-of-line blocking to break. Measured before the fix, one
 * producer against one hard-polling consumer: 989 of 1024 slots survived, worst trial.
 *
 * The segment is used unlinked and directly, not through a proxy, because a proxy would
 * link a successor the moment the segment refused an item -- which is exactly the
 * condition that switches HQ to the fast path and hides this.
 */
TEST(HQSlowPath, DoesNotBurnCapacityWithNothingQueuedBehind) {
    using Seg = seg::HQ<Item>;
    constexpr std::size_t kCapacity = 1024;
    constexpr int kTrials = 10;

    std::size_t placed_total = 0, taken_total = 0, worst = kCapacity;

    for (int trial = 0; trial < kTrials; ++trial) {
        Seg* s = mem::SingleBlock<Seg>::create(kCapacity);
        std::vector<Data> store(kCapacity);
        for (std::size_t i = 0; i < kCapacity; ++i) store[i] = {i + 1, 0};

        std::atomic<std::size_t> placed{0};
        std::atomic<bool> done{false};

        // The gap is what makes this test bite, and it has to be a real delay -- a
        // compiler barrier is not one. It keeps the queue near-empty so the consumer
        // polls many times per item and is therefore usually sitting on the head cell
        // when the producer fetch-adds, which is the window the burn happens in.
        std::thread producer{[&] {
            for (std::size_t i = 0; i < kCapacity; ++i) {
                if (!s->enqueue(&store[i])) break; // refused: capacity was destroyed
                placed.fetch_add(1, std::memory_order_relaxed);
                for (int k = 0; k < 200; ++k) SPIN_HINT();
            }
            done.store(true, std::memory_order_release);
        }};

        std::size_t taken = 0;
        std::thread consumer{[&] {
            Item out = nullptr;
            while (!done.load(std::memory_order_acquire))
                if (s->dequeue(out)) ++taken;
            while (s->dequeue(out)) ++taken;
        }};

        producer.join();
        consumer.join();

        placed_total += placed.load();
        taken_total += taken;
        worst = std::min(worst, placed.load());
        mem::SingleBlock<Seg>::destroy(s);
    }

    EXPECT_EQ(placed_total, kCapacity * kTrials)
        << "the consumer burned slots a producer had claimed with nothing queued behind "
           "them; worst trial placed " << worst << " of " << kCapacity;
    EXPECT_EQ(taken_total, placed_total) << "items were lost";
}

} // namespace
