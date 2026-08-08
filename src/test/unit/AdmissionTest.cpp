/**
 * @file AdmissionTest.cpp
 * @brief Admission policies, and the bounds they promise, under concurrency.
 *
 * The three policies are what remains of the difference between UnboundedProxy,
 * BoundedCounterProxy and BoundedChunkProxy. A bound that only holds single-threaded is
 * not a bound, so the interesting cases are the concurrent ones.
 */
#include <gtest/gtest.h>

#include <algo/PRQ.hpp>
#include <algo/Vyukov.hpp>
#include <proxy/Aliases.hpp>

#include <atomic>
#include <barrier>
#include <thread>
#include <vector>

namespace {

struct Data {
    uint64_t seq;
};
using Item = Data*;

constexpr std::size_t kSegment = 64;
constexpr std::size_t kChunks = 4;

// ---------------------------------------------------------------------------
// The policies in isolation
// ---------------------------------------------------------------------------

TEST(AdmitNone, AdmitsEverythingAndCostsNothing) {
    proxy::admit::None a{proxy::admit::None::config(0, 0)};
    EXPECT_TRUE(a.try_admit());
    EXPECT_EQ(a.bound(), 0u);
    EXPECT_FALSE(proxy::admit::None::bounded);
    // Empty, so [[no_unique_address]] can erase it from the proxy entirely.
    EXPECT_TRUE(std::is_empty_v<proxy::admit::None>);
}

TEST(AdmitItemCount, StopsAdmittingAtTheBound) {
    proxy::admit::ItemCount a{proxy::admit::ItemCount::Config{4}};
    EXPECT_EQ(a.bound(), 4u);
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(a.try_admit()) << "refused at occupancy " << i;
        a.on_enqueue();
    }
    EXPECT_FALSE(a.try_admit()) << "admitted beyond the bound";
    a.on_dequeue();
    EXPECT_TRUE(a.try_admit()) << "did not recover after a dequeue";
}

TEST(AdmitSegmentCount, CountsSegmentsNotItems) {
    proxy::admit::SegmentCount a{proxy::admit::SegmentCount::config(kSegment, kChunks)};
    EXPECT_EQ(a.bound(), kChunks);
    // Item traffic must not move a segment-based bound.
    for (int i = 0; i < 1000; ++i) a.on_enqueue();
    EXPECT_TRUE(a.try_admit());
    for (std::size_t i = 0; i + 1 < kChunks; ++i) a.on_segment_linked();
    EXPECT_FALSE(a.try_admit());
    a.on_segment_retired();
    EXPECT_TRUE(a.try_admit());
}

// ---------------------------------------------------------------------------
// The bounds as observed through a proxy
// ---------------------------------------------------------------------------

/// Peak occupancy reached while producers push and nobody consumes.
template <typename Q>
std::size_t fill_to_refusal(Q& q, std::vector<Data>& store) {
    std::size_t placed = 0;
    for (auto& d : store)
        if (q.enqueue(&d)) ++placed;
        else break;
    return placed;
}

TEST(BoundedProxies, ItemBoundedNeverExceedsItsCapacity) {
    using Q = proxy::ItemBounded<Item, seg::Vyukov<Item>>;
    Q q{kSegment, kChunks};
    auto joined = q.join();
    ASSERT_TRUE(joined);

    std::vector<Data> store(kSegment * kChunks * 4);
    const std::size_t placed = fill_to_refusal(q, store);

    EXPECT_LE(placed, q.capacity()) << "admitted more items than the stated capacity";
    EXPECT_GT(placed, 0u);
}

TEST(BoundedProxies, UnboundedKeepsGoingWellPastOneSegment) {
    using Q = proxy::Unbounded<Item, seg::Vyukov<Item>>;
    Q q{kSegment};
    auto joined = q.join();
    ASSERT_TRUE(joined);

    std::vector<Data> store(kSegment * 10);
    EXPECT_EQ(fill_to_refusal(q, store), store.size());
}

TEST(BoundedProxies, ItemBoundIsRespectedConcurrently) {
    using Q = proxy::ItemBounded<Item, seg::PRQ<Item>>;
    constexpr std::size_t kProducers = 4;
    Q q{kSegment, kChunks};

    std::vector<Data> store(20000);
    std::atomic<int64_t> live{0};
    std::atomic<int64_t> peak{0};
    std::atomic<std::size_t> next{0};
    std::barrier sync(kProducers);
    std::vector<std::thread> ts;

    // Producers only: occupancy climbs monotonically to the ceiling and must stop there.
    for (std::size_t p = 0; p < kProducers; ++p)
        ts.emplace_back([&] {
            auto joined = q.join();
    ASSERT_TRUE(joined);
            sync.arrive_and_wait();
            for (;;) {
                const std::size_t i = next.fetch_add(1);
                if (i >= store.size()) break;
                if (!q.enqueue(&store[i])) continue;
                const int64_t now = live.fetch_add(1) + 1;
                int64_t seen = peak.load();
                while (now > seen && !peak.compare_exchange_weak(seen, now)) {}
            }
        });
    for (auto& t : ts) t.join();

    EXPECT_LE(static_cast<std::size_t>(peak.load()), q.capacity())
        << "peak occupancy " << peak.load() << " exceeded capacity " << q.capacity();
}

TEST(BoundedProxies, ChunkBoundedRefusesRatherThanGrowingForever) {
    using Q = proxy::ChunkBounded<Item, seg::Vyukov<Item>>;
    Q q{kSegment, kChunks};
    auto joined = q.join();
    ASSERT_TRUE(joined);

    std::vector<Data> store(kSegment * kChunks * 8);
    const std::size_t placed = fill_to_refusal(q, store);

    EXPECT_LT(placed, store.size()) << "a segment-bounded proxy accepted an unbounded stream";
    EXPECT_LE(placed, kSegment * kChunks)
        << "accepted more than bound() segments' worth of items";
}

TEST(BoundedProxies, RefusalIsRecoverableAfterDraining) {
    using Q = proxy::ItemBounded<Item, seg::Vyukov<Item>>;
    Q q{kSegment, kChunks};
    auto joined = q.join();
    ASSERT_TRUE(joined);

    std::vector<Data> store(kSegment * kChunks * 4);
    const std::size_t placed = fill_to_refusal(q, store);
    ASSERT_GT(placed, 0u);

    Item out = nullptr;
    std::size_t drained = 0;
    while (q.dequeue(out)) ++drained;
    EXPECT_EQ(drained, placed);

    // Having drained, the proxy must admit again -- a bound that latches is a deadlock.
    EXPECT_TRUE(q.enqueue(&store[0]));
}

} // namespace
