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
    proxy::admit::None a{proxy::admit::None::config(0)};
    EXPECT_TRUE(a.try_admit());
    EXPECT_EQ(a.bound(), 0u);
    EXPECT_FALSE(proxy::admit::None::bounded);
    // Empty, so [[no_unique_address]] can erase it from the proxy entirely.
    EXPECT_TRUE(std::is_empty_v<proxy::admit::None>);

    // 0 means "no opinion", not "holds nothing": this policy imposes no ceiling, so the real
    // one is structural and LinkedProxy::capacity() falls back on the segment count. Answering
    // with one segment's worth, as this used to, was wrong for a pooled source.
    EXPECT_EQ(a.capacity(64), 0u);
}

/// The emptiness above has to be worth something, and this is where it shows: with the same
/// segment and the same source, the only difference between these two proxies is the policy
/// member. It is the property that keeps a pooled or unbounded queue from paying for a hook it
/// never calls -- and the reason the per-segment capacity lives in the source rather than here.
TEST(AdmitNone, CostsTheProxyNothingComparedToACountingPolicy) {
    using S = seg::Vyukov<Item>;
    EXPECT_LT(sizeof(proxy::Unbounded<Item, S>), sizeof(proxy::ItemBounded<Item, S, kChunks>))
        << "admit::None is no longer being elided by [[no_unique_address]]";
}

TEST(AdmitItemCount, StopsAdmittingAtTheBound) {
    proxy::admit::ItemCount<> a{proxy::admit::ItemCount<>::Config{4}};
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
    using Policy = proxy::admit::SegmentCount<kChunks>;
    Policy a{Policy::config(kSegment)};
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

/**
 * @brief Run @p f once per segment count, each as a compile-time argument.
 *
 * The counts moved from a constructor argument into the type, so a runtime `for` over them is
 * no longer possible -- which is the whole point of the change: a chunk count and a pool size
 * are now spelled the same way and cannot drift apart.
 */
template <std::size_t... Counts, typename F>
void for_each_count(F f) {
    (f.template operator()<Counts>(), ...);
}

/// Peak occupancy reached while producers push and nobody consumes.
template <typename Q>
std::size_t fill_to_refusal(Q& q, std::vector<Data>& store) {
    std::size_t placed = 0;
    for (auto& d : store)
        if (q.try_enqueue(&d)) ++placed;
        else break;
    return placed;
}

TEST(BoundedProxies, ItemBoundedNeverExceedsItsCapacity) {
    using Q = proxy::ItemBounded<Item, seg::Vyukov<Item>, kChunks>;
    Q q{kSegment};
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
    using Q = proxy::ItemBounded<Item, seg::PRQ<Item>, kChunks>;
    constexpr std::size_t kProducers = 4;
    Q q{kSegment};

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
                if (!q.try_enqueue(&store[i])) continue;
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
    using Q = proxy::ChunkBounded<Item, seg::Vyukov<Item>, kChunks>;
    Q q{kSegment};
    auto joined = q.join();
    ASSERT_TRUE(joined);

    std::vector<Data> store(kSegment * kChunks * 8);
    const std::size_t placed = fill_to_refusal(q, store);

    EXPECT_LT(placed, store.size()) << "a segment-bounded proxy accepted an unbounded stream";
    EXPECT_LE(placed, kSegment * kChunks)
        << "accepted more than bound() segments' worth of items";
}

/**
 * @brief A bounded proxy must *reach* the capacity it advertises, not merely stay under it.
 *
 * Every other test here asserts `placed <= capacity()`, which a bound that admits nothing
 * satisfies perfectly. That is the hole this closes, and it was hiding a real defect: because
 * `admit::SegmentCount` was asked at the top of every enqueue rather than when a segment was
 * about to be linked, a chunk-bounded queue refused while its tail still had free slots --
 * and at `chunks == 1` it held zero items while reporting a capacity of `kSegment`.
 *
 * `chunks == 1` is therefore the case that matters most and is checked explicitly.
 */
TEST(BoundedProxies, ChunkBoundedReachesItsStatedCapacity) {
    for_each_count<1, 2, 4, 8>([]<std::size_t Chunks>() {
        using Q = proxy::ChunkBounded<Item, seg::Vyukov<Item>, Chunks>;
        Q q{kSegment};
        auto joined = q.join();
        ASSERT_TRUE(joined);

        std::vector<Data> store(kSegment * Chunks * 4);
        const std::size_t placed = fill_to_refusal(q, store);

        EXPECT_EQ(placed, q.capacity())
            << "chunks=" << Chunks << ": advertised " << q.capacity() << " but held " << placed;
    });
}

/// The same property for the item-counting policy, which has always had it -- present so a
/// change to one policy that breaks the other is caught here rather than in a benchmark.
TEST(BoundedProxies, ItemBoundedReachesItsStatedCapacity) {
    for_each_count<1, 2, 4>([]<std::size_t Chunks>() {
        using Q = proxy::ItemBounded<Item, seg::Vyukov<Item>, Chunks>;
        Q q{kSegment};
        auto joined = q.join();
        ASSERT_TRUE(joined);

        std::vector<Data> store(kSegment * Chunks * 4);
        EXPECT_EQ(fill_to_refusal(q, store), q.capacity()) << "chunks=" << Chunks;
    });
}

/**
 * @brief The capacity argument is a *total*, divided among the segments that will exist.
 *
 * Two things are checked at once, because they can fail independently: the queue reaches the
 * capacity it advertises, and it got there by actually splitting rather than by building one
 * big segment. The second needs `segment_stats` -- without it a proxy that ignored the divisor
 * entirely would still pass the first.
 *
 * `capacity()` is read from the queue rather than assumed. Segments round their own size up,
 * and the split is floored at two slots because a one-slot ring never reports itself full, so
 * the achievable total is frequently larger than the request.
 */
TEST(BoundedProxies, CapacityIsSplitAcrossSegments) {
    for_each_count<2, 4, 8>([]<std::size_t Chunks>() {
        using Counted = proxy::ChunkBounded<Item, seg::Vyukov<Item>, Chunks, meta::EmptyOptions,
                                            meta::OptionsPack<proxy::ProxyOpt::segment_stats>>;
        Counted q{kSegment * Chunks};              // ask for a total, not a segment size
        auto joined = q.join();
        ASSERT_TRUE(joined);

        std::vector<Data> store(kSegment * Chunks * 4);
        const std::size_t placed = fill_to_refusal(q, store);

        EXPECT_EQ(placed, q.capacity()) << "chunks=" << Chunks;
        // Sentinel plus one per link: the whole point is that the total was spread out.
        EXPECT_EQ(q.segments_linked(), Chunks)
            << "chunks=" << Chunks << ": capacity was not split -- linked "
            << q.segments_linked() << " segments";
    });
}

/// The same for a pooled source, where the divisor comes from the pool rather than the policy.
TEST(BoundedProxies, PooledCapacityIsSplitAcrossThePool) {
    constexpr std::size_t N = 8;
    using Seg = seg::Vyukov<Item, meta::EmptyOptions, mem::IndexHandle<N>>;
    using Q = proxy::MemBounded<Item, Seg, N>;

    Q q{N * kSegment};                              // a total across the eight pool slots
    auto joined = q.join();
    ASSERT_TRUE(joined);

    // admit::None cannot answer here -- the ceiling is the pool running dry, so the proxy asks
    // the source. Before that it reported one segment's worth as the entire capacity.
    EXPECT_EQ(q.capacity(), N * kSegment);

    std::vector<Data> store(N * kSegment * 4);
    EXPECT_EQ(fill_to_refusal(q, store), q.capacity());
}

/**
 * @brief A chunk-bounded queue and a pooled one, given the same segment count, must be the
 *        same shape.
 *
 * This is the guard for the change that made the count a template parameter. It used to be the
 * second constructor argument of LinkedProxy, defaulted to 4 and passed by nobody -- so every
 * `chunk-*` registry entry ran with 4 segments while every `mem-*` entry ran with `kPoolSize`,
 * and a benchmark comparing the two families was comparing different geometries without saying
 * so. Now both take it as a template argument, and this asserts they land in the same place.
 *
 * Deliberately checks the *observable* consequences rather than the constant: same advertised
 * capacity, same number of segments actually linked to reach it, same items admitted.
 */
TEST(BoundedProxies, ChunkAndPoolAgreeAtTheSameSegmentCount) {
    for_each_count<2, 4, 8>([]<std::size_t Segments>() {
        using Stats = meta::OptionsPack<proxy::ProxyOpt::segment_stats>;
        using PooledSeg = seg::Vyukov<Item, meta::EmptyOptions, mem::IndexHandle<Segments>>;
        using Chunked =
            proxy::ChunkBounded<Item, seg::Vyukov<Item>, Segments, meta::EmptyOptions, Stats>;
        using Pooled = proxy::MemBounded<Item, PooledSeg, Segments, meta::EmptyOptions, Stats>;

        static_assert(Chunked::live_segments == Pooled::live_segments,
                      "the two families disagree about how many segments they may have live");

        constexpr std::size_t total = kSegment * Segments;
        Chunked chunked{total};
        Pooled pooled{total};
        auto jc = chunked.join();
        auto jp = pooled.join();
        ASSERT_TRUE(jc);
        ASSERT_TRUE(jp);

        EXPECT_EQ(chunked.capacity(), pooled.capacity())
            << "segments=" << Segments << ": chunk-bounded advertises " << chunked.capacity()
            << " but pooled advertises " << pooled.capacity();

        std::vector<Data> store(total * 4);
        const std::size_t in_chunked = fill_to_refusal(chunked, store);
        const std::size_t in_pooled = fill_to_refusal(pooled, store);

        EXPECT_EQ(in_chunked, in_pooled) << "segments=" << Segments << ": same stated capacity, "
                                         << "different number of items actually admitted";
        EXPECT_EQ(chunked.segments_linked(), pooled.segments_linked())
            << "segments=" << Segments << ": the capacity was split differently";
    });
}

TEST(BoundedProxies, RefusalIsRecoverableAfterDraining) {
    using Q = proxy::ItemBounded<Item, seg::Vyukov<Item>, kChunks>;
    Q q{kSegment};
    auto joined = q.join();
    ASSERT_TRUE(joined);

    std::vector<Data> store(kSegment * kChunks * 4);
    const std::size_t placed = fill_to_refusal(q, store);
    ASSERT_GT(placed, 0u);

    Item out = nullptr;
    std::size_t drained = 0;
    while (q.try_dequeue(out)) ++drained;
    EXPECT_EQ(drained, placed);

    // Having drained, the proxy must admit again -- a bound that latches is a deadlock.
    EXPECT_TRUE(q.try_enqueue(&store[0]));
}

} // namespace
