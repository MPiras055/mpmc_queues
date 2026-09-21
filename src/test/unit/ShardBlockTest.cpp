/**
 * @file ShardBlockTest.cpp
 * @brief The SPSC buffers, the all-to-all block built from them, and its two strategies.
 *
 * Single-threaded cases pin the protocol, the arena layout and the refusal contract; the
 * `Threaded*` suites check loss, duplication and per-pair FIFO under real concurrency. Per the
 * standing convention in this repository the threaded suites are built and left to be run by
 * hand:
 *
 * ```
 * ./ShardBlockTest --gtest_filter='-Threaded*'   # everything single-threaded
 * ./ShardBlockTest --gtest_filter='Threaded*'    # the concurrent half
 * ```
 */
#include <gtest/gtest.h>

#include <block/AllToAll.hpp>
#include <core/Construction.hpp>
#include <core/Queue.hpp>
#include <registry/Registry.hpp>
#include <spsc/Buffer.hpp>
#include <spsc/Unbounded.hpp>

#include <array>
#include <atomic>
#include <barrier>
#include <bit>
#include <cstdint>
#include <memory>
#include <set>
#include <thread>
#include <vector>

namespace {

using Item = std::uint64_t*;

/// Item @p i as a non-null, even pointer-shaped value -- the shape the benchmark sends.
Item item(std::uint64_t i) { return std::bit_cast<Item>((i + 1) * 2); }
std::uint64_t seq(Item v) { return std::bit_cast<std::uint64_t>(v) / 2 - 1; }

/// A standalone buffer over heap cells, for testing the buffers on their own.
template <typename Buf>
struct Owned {
    explicit Owned(std::size_t n)
        : cells{new typename Buf::cell_type[Buf::capacity_for(n)]},
          buf{Buf::capacity_for(n), cells.get()} {}
    std::unique_ptr<typename Buf::cell_type[]> cells;
    Buf buf;
};

// ---------------------------------------------------------------------------------------------
// Padding is what was asked for
// ---------------------------------------------------------------------------------------------

TEST(Padding, CircularCellsOccupyAWholeLine) {
    using C = spsc::Ring<Item>::cell_type;
    static_assert(alignof(C) == CACHE_LINE);
    static_assert(sizeof(C) == CACHE_LINE);
    SUCCEED();
}

TEST(Padding, LinearCellsArePacked) {
    using C = spsc::Linear<Item>::cell_type;
    static_assert(sizeof(C) == sizeof(Item));
    static_assert(alignof(C) < CACHE_LINE);
    SUCCEED();
}

TEST(Padding, IndicesSitOnSeparateLines) {
    // cells/mask, write index, read index: three lines, so the producer's and the consumer's
    // only writes to the header never share one.
    static_assert(sizeof(spsc::Ring<Item>) == 3 * CACHE_LINE);
    static_assert(sizeof(spsc::Linear<Item>) == 3 * CACHE_LINE);
    SUCCEED();
}

// ---------------------------------------------------------------------------------------------
// The buffers, single-threaded
// ---------------------------------------------------------------------------------------------

TEST(Ring, CapacityRoundsUpToAPowerOfTwo) {
    EXPECT_EQ(spsc::Ring<Item>::capacity_for(0), 1u);
    EXPECT_EQ(spsc::Ring<Item>::capacity_for(3), 4u);
    EXPECT_EQ(spsc::Ring<Item>::capacity_for(8), 8u);
    EXPECT_EQ(spsc::Ring<Item>::capacity_for(100), 128u);
}

TEST(Ring, FillsToCapacityThenRefusesAndDrainsInOrder) {
    Owned<spsc::Ring<Item>> o{8};
    auto& r = o.buf;
    for (std::uint64_t i = 0; i < 8; ++i) ASSERT_TRUE(r.enqueue(item(i))) << i;
    EXPECT_FALSE(r.enqueue(item(99))) << "a full ring must refuse";
    EXPECT_EQ(r.size(), 8u);
    Item out = nullptr;
    for (std::uint64_t i = 0; i < 8; ++i) {
        ASSERT_TRUE(r.dequeue(out));
        EXPECT_EQ(seq(out), i);
    }
    EXPECT_EQ(r.size(), 0u);
}

TEST(Ring, WrapsAroundManyLaps) {
    Owned<spsc::Ring<Item>> o{4};
    auto& r = o.buf;
    std::uint64_t in = 0, next = 0;
    Item out = nullptr;
    for (int lap = 0; lap < 100; ++lap) {
        while (r.enqueue(item(in))) ++in;
        for (int k = 0; k < 3; ++k) {
            ASSERT_TRUE(r.dequeue(out));
            ASSERT_EQ(seq(out), next++);
        }
    }
    while (r.dequeue(out)) ASSERT_EQ(seq(out), next++);
    EXPECT_EQ(next, in);
}

TEST(Ring, FailedDequeueLeavesOutUntouched) {
    Owned<spsc::Ring<Item>> o{4};
    Item out = nullptr;
    EXPECT_FALSE(o.buf.dequeue(out));
    EXPECT_EQ(out, nullptr) << "a failed dequeue must not write to the out parameter";
}

TEST(Linear, FillsOnceAndStaysRefusedAfterADrain) {
    Owned<spsc::Linear<Item>> o{5};
    auto& l = o.buf;
    EXPECT_EQ(l.capacity(), 5u) << "a linear buffer is not rounded";
    for (std::uint64_t i = 0; i < 5; ++i) ASSERT_TRUE(l.enqueue(item(i)));
    EXPECT_FALSE(l.enqueue(item(99)));
    Item out = nullptr;
    for (std::uint64_t i = 0; i < 5; ++i) {
        ASSERT_TRUE(l.dequeue(out));
        EXPECT_EQ(seq(out), i);
    }
    EXPECT_TRUE(l.drained());
    EXPECT_FALSE(l.enqueue(item(100))) << "a linear buffer never wraps: drained is still full";
    Item untouched = nullptr;
    EXPECT_FALSE(l.dequeue(untouched));
    EXPECT_EQ(untouched, nullptr);
}

TEST(Linear, ResetMakesItReusable) {
    Owned<spsc::Linear<Item>> o{3};
    auto& l = o.buf;
    Item out = nullptr;
    for (std::uint64_t i = 0; i < 3; ++i) ASSERT_TRUE(l.enqueue(item(i)));
    while (l.dequeue(out)) {}
    l.reset();
    EXPECT_FALSE(l.drained());
    EXPECT_FALSE(l.dequeue(out)) << "a reset buffer is empty";
    EXPECT_TRUE(l.enqueue(item(7)));
    ASSERT_TRUE(l.dequeue(out));
    EXPECT_EQ(seq(out), 7u);
}

TEST(Linear, EmptyBeforeTheFrontierIsNotDrained) {
    Owned<spsc::Linear<Item>> o{4};
    Item out = nullptr;
    ASSERT_TRUE(o.buf.enqueue(item(0)));
    ASSERT_TRUE(o.buf.dequeue(out));
    EXPECT_FALSE(o.buf.dequeue(out));
    EXPECT_FALSE(o.buf.drained()) << "empty now is not drained for good";
}

TEST(Unbounded, GrowsPastAnyBoundInOrder) {
    spsc::Unbounded<Item> q{/*chunk=*/4, /*recycled=*/2};
    constexpr std::uint64_t kN = 1000;
    for (std::uint64_t i = 0; i < kN; ++i) ASSERT_TRUE(q.enqueue(item(i)));
    EXPECT_GE(q.chunks_linked(), kN / 4 - 1) << "it had to grow to hold them";
    Item out = nullptr;
    for (std::uint64_t i = 0; i < kN; ++i) {
        ASSERT_TRUE(q.dequeue(out)) << i;
        ASSERT_EQ(seq(out), i);
    }
    Item untouched = nullptr;
    EXPECT_FALSE(q.dequeue(untouched));
    EXPECT_EQ(untouched, nullptr);
}

TEST(Unbounded, AFullChunkWithNoSuccessorIsNotFreed) {
    // The one-chunk case: drained to capacity, `next` still null -> report empty, keep the chunk,
    // and pick up where the producer continues.
    spsc::Unbounded<Item> q{2, 2};
    Item out = nullptr;
    ASSERT_TRUE(q.enqueue(item(0)));
    ASSERT_TRUE(q.enqueue(item(1)));
    ASSERT_TRUE(q.dequeue(out));
    ASSERT_TRUE(q.dequeue(out));
    EXPECT_FALSE(q.dequeue(out));
    EXPECT_EQ(q.chunks_retired(), 0u);
    ASSERT_TRUE(q.enqueue(item(2)));
    ASSERT_TRUE(q.dequeue(out));
    EXPECT_EQ(seq(out), 2u);
    EXPECT_EQ(q.chunks_retired(), 1u);
}

TEST(Unbounded, RecyclesDrainedChunksInsteadOfAllocating) {
    spsc::Unbounded<Item> q{2, 4};
    Item out = nullptr;
    std::uint64_t in = 0, next = 0;
    // Interleaved, so there are never more than two chunks live and recycling suffices.
    for (int round = 0; round < 200; ++round) {
        for (int k = 0; k < 3; ++k) ASSERT_TRUE(q.enqueue(item(in++)));
        for (int k = 0; k < 3; ++k) {
            ASSERT_TRUE(q.dequeue(out));
            ASSERT_EQ(seq(out), next++);
        }
    }
    EXPECT_GT(q.chunks_retired(), 100u);
}

// ---------------------------------------------------------------------------------------------
// The arena
// ---------------------------------------------------------------------------------------------

template <typename B>
void CheckArena(std::size_t capacity, std::size_t p, std::size_t c) {
    SCOPED_TRACE(testing::Message() << "capacity=" << capacity << " P=" << p << " C=" << c);
    // The MemoryLayoutTest guards, applied to the arena.
    const auto pl = B::plan(capacity, p, c);
    EXPECT_TRUE(pl.valid(sizeof(B)));
    std::size_t prev_end = sizeof(B);
    for (const auto& r : pl.regions) {
        EXPECT_GE(r.offset, prev_end) << "region overlaps header/previous";
        EXPECT_LE(r.end(), pl.total) << "region runs past the block";
        prev_end = r.end();
    }
    EXPECT_EQ(pl.total % pl.block_align, 0u);

    // Every buffer's cell run starts on a line and no two runs overlap.
    B* b = B::create(capacity, p, c);
    const auto* base = reinterpret_cast<const std::byte*>(b);
    const std::size_t run = b->per_buffer() * sizeof(typename B::cell_type);
    EXPECT_GE(b->stride(), run);
    EXPECT_EQ(b->stride() % CACHE_LINE, 0u);
    const std::byte* prev = nullptr;
    for (std::size_t i = 0; i < p; ++i)
        for (std::size_t j = 0; j < c; ++j) {
            const auto& buf = b->buffer(i, j);
            const auto* cells = reinterpret_cast<const std::byte*>(buf.cells());
            EXPECT_EQ(reinterpret_cast<std::uintptr_t>(cells) % CACHE_LINE, 0u);
            EXPECT_GE(cells, base + pl.regions[1].offset);
            EXPECT_LE(cells + run, base + pl.regions[1].end());
            if (prev) EXPECT_GE(cells, prev + run) << "two buffers' cells overlap";
            prev = cells;
            const auto* hdr = reinterpret_cast<const std::byte*>(&buf);
            EXPECT_GE(hdr, base + pl.regions[0].offset);
            EXPECT_LE(hdr + sizeof(buf), base + pl.regions[0].end());
        }
    EXPECT_EQ(b->capacity(), p * c * b->per_buffer());
    EXPECT_GE(b->capacity(), capacity);
    B::destroy(b);
}

TEST(Arena, RingLayoutIsWellFormed) {
    using B = block::AllToAll<Item>;
    for (auto [cap, p, c] : std::vector<std::array<std::size_t, 3>>{
             {1, 1, 1}, {4096, 4, 4}, {100, 3, 5}, {7, 8, 1}, {1000, 1, 7}})
        CheckArena<B>(cap, p, c);
}

TEST(Arena, LinearLayoutIsWellFormed) {
    // Packed cells: the stride is the only padding, and it must still keep runs apart.
    using B = block::AllToAll<Item, block::RoundRobin, spsc::Linear>;
    for (auto [cap, p, c] : std::vector<std::array<std::size_t, 3>>{
             {1, 1, 1}, {4096, 4, 4}, {100, 3, 5}, {7, 8, 1}, {1000, 1, 7}})
        CheckArena<B>(cap, p, c);
}

TEST(Arena, PerBufferIsTheTotalSplitFlooredAndRounded) {
    using B = block::AllToAll<Item>;
    EXPECT_EQ(B::per_buffer_for(4096, 4, 4), 256u);
    EXPECT_EQ(B::per_buffer_for(100, 3, 5), 8u);   // ceil(100/15) = 7 -> 8
    EXPECT_EQ(B::per_buffer_for(1, 4, 4), 2u);     // floored at 2
}

TEST(Arena, ZeroDegreeIsRefused) {
    EXPECT_THROW((void)block::AllToAll<Item>::create(64, 0, 4), std::invalid_argument);
    EXPECT_THROW((void)block::AllToAll<Item>::create(64, 4, 0), std::invalid_argument);
}

// ---------------------------------------------------------------------------------------------
// Degrees and strategies, single-threaded
// ---------------------------------------------------------------------------------------------

template <typename S>
struct Strategies : testing::Test {};
using StrategyTypes = testing::Types<block::RoundRobin, block::Sticky>;
struct StrategyNames {
    template <typename S>
    static std::string GetName(int) {
        return std::is_same_v<S, block::Sticky> ? "Sticky" : "RoundRobin";
    }
};
TYPED_TEST_SUITE(Strategies, StrategyTypes, StrategyNames);

TYPED_TEST(Strategies, OneByOneIsASingleFifoBuffer) {
    using B = block::AllToAll<Item, TypeParam>;
    auto* b = B::create(8, 1, 1);
    auto p = b->producer(0);
    auto c = b->consumer(0);
    std::uint64_t in = 0, next = 0;
    Item out = nullptr;
    for (int lap = 0; lap < 20; ++lap) {
        while (p.enqueue(item(in))) ++in;
        EXPECT_EQ(b->size(), b->capacity());
        while (c.dequeue(out)) ASSERT_EQ(seq(out), next++) << "1x1 must be globally FIFO";
    }
    EXPECT_EQ(next, in);
    B::destroy(b);
}

/// Fill every row to refusal, then drain every column to refusal; every item comes back once.
template <typename B>
void FillAndDrain(std::size_t cap, std::size_t np, std::size_t nc) {
    SCOPED_TRACE(testing::Message() << "P=" << np << " C=" << nc);
    auto* b = B::create(cap, np, nc);
    std::vector<typename B::Producer> ps;
    std::vector<typename B::Consumer> cs;
    for (std::size_t i = 0; i < np; ++i) ps.push_back(b->producer(i));
    for (std::size_t j = 0; j < nc; ++j) cs.push_back(b->consumer(j));

    std::uint64_t in = 0;
    for (std::size_t i = 0; i < np; ++i)
        while (ps[i].enqueue(item(in))) ++in;
    EXPECT_EQ(in, b->capacity()) << "a row refuses only once every buffer in it is full";
    EXPECT_EQ(b->size(), b->capacity());

    std::set<std::uint64_t> seen;
    Item out = nullptr;
    for (std::size_t j = 0; j < nc; ++j)
        while (cs[j].dequeue(out)) EXPECT_TRUE(seen.insert(seq(out)).second) << "duplicate";
    EXPECT_EQ(seen.size(), in) << "a column reports empty only once every buffer in it is";
    EXPECT_EQ(b->size(), 0u);
    B::destroy(b);
}

TYPED_TEST(Strategies, AllDegreesFillAndDrainCompletely) {
    using B = block::AllToAll<Item, TypeParam>;
    FillAndDrain<B>(64, 4, 4);
    FillAndDrain<B>(64, 1, 4); // emitter
    FillAndDrain<B>(64, 4, 1); // collector
    FillAndDrain<B>(100, 3, 5);
    FillAndDrain<B>(8, 1, 1);
}

TYPED_TEST(Strategies, FarmAliasesPinOneDegree) {
    auto* e = block::FarmEmitter<Item, TypeParam>::create(64, 4);
    EXPECT_EQ(e->producers(), 1u);
    EXPECT_EQ(e->consumers(), 4u);
    block::FarmEmitter<Item, TypeParam>::type::destroy(e);
    auto* c = block::FarmCollector<Item, TypeParam>::create(64, 4);
    EXPECT_EQ(c->producers(), 4u);
    EXPECT_EQ(c->consumers(), 1u);
    block::FarmCollector<Item, TypeParam>::type::destroy(c);
}

TYPED_TEST(Strategies, AColumnHoldingOneItemInItsLastBufferIsStillDequeued) {
    // The refusal contract, directly: the cursor sits on buffer 0, which is empty, and the only
    // item is in the last buffer of the column. Reporting empty here is a lost item.
    using B = block::AllToAll<Item, TypeParam>;
    constexpr std::size_t kP = 4;
    auto* b = B::create(64, kP, 1);
    auto last = b->producer(kP - 1);
    ASSERT_TRUE(last.enqueue(item(42)));
    auto c = b->consumer(0);
    Item out = nullptr;
    ASSERT_TRUE(c.dequeue(out)) << "the column is not empty; its last buffer holds an item";
    EXPECT_EQ(seq(out), 42u);
    Item untouched = nullptr;
    EXPECT_FALSE(c.dequeue(untouched));
    EXPECT_EQ(untouched, nullptr) << "a failed dequeue must not write to the out parameter";
    B::destroy(b);
}

TYPED_TEST(Strategies, ARowWithOneFreeSlotInItsLastBufferStillAccepts) {
    using B = block::AllToAll<Item, TypeParam>;
    constexpr std::size_t kC = 4;
    auto* b = B::create(2 * kC, 1, kC); // 2 per buffer
    auto p = b->producer(0);
    std::uint64_t in = 0;
    while (p.enqueue(item(in))) ++in;
    ASSERT_EQ(in, b->capacity());
    // Free exactly one slot, in the last column.
    auto c = b->consumer(kC - 1);
    Item out = nullptr;
    ASSERT_TRUE(c.dequeue(out));
    EXPECT_TRUE(p.enqueue(item(in))) << "one buffer in the row has room";
    EXPECT_FALSE(p.enqueue(item(in + 1)));
    B::destroy(b);
}

TEST(Strategy, RoundRobinSpreadsStickyConcentrates) {
    // The two policies differ in *where* items go, and that difference is the whole point of
    // having both: round-robin fans one item to each consumer in turn, sticky keeps feeding one.
    auto* rr = block::AllToAll<Item, block::RoundRobin>::create(64, 1, 4);
    auto* st = block::AllToAll<Item, block::Sticky>::create(64, 1, 4);
    auto prr = rr->producer(0);
    auto pst = st->producer(0);
    for (std::uint64_t i = 0; i < 4; ++i) {
        ASSERT_TRUE(prr.enqueue(item(i)));
        ASSERT_TRUE(pst.enqueue(item(i)));
    }
    for (std::size_t j = 0; j < 4; ++j) EXPECT_EQ(rr->buffer(0, j).size(), 1u) << j;
    EXPECT_EQ(st->buffer(0, 0).size(), 4u);
    block::AllToAll<Item, block::RoundRobin>::destroy(rr);
    block::AllToAll<Item, block::Sticky>::destroy(st);
}

TEST(Handles, ARetakenHandleResumesFromThePublishedIndices) {
    using B = block::AllToAll<Item>;
    auto* b = B::create(16, 1, 1);
    {
        auto p = b->producer(0);
        ASSERT_TRUE(p.enqueue(item(0)));
        ASSERT_TRUE(p.enqueue(item(1)));
    }
    {
        auto c = b->consumer(0);
        Item out = nullptr;
        ASSERT_TRUE(c.dequeue(out));
        EXPECT_EQ(seq(out), 0u);
    }
    auto p = b->producer(0);
    ASSERT_TRUE(p.enqueue(item(2)));
    auto c = b->consumer(0);
    Item out = nullptr;
    ASSERT_TRUE(c.dequeue(out));
    EXPECT_EQ(seq(out), 1u);
    ASSERT_TRUE(c.dequeue(out));
    EXPECT_EQ(seq(out), 2u);
    B::destroy(b);
}

// ---------------------------------------------------------------------------------------------
// The harness bridge
// ---------------------------------------------------------------------------------------------

using ShardRR = queue::ShardQueue<block::AllToAll<Item, block::RoundRobin>>;
using ShardSticky = queue::ShardQueue<block::AllToAll<Item, block::Sticky>>;

static_assert(core::Queue<ShardRR, Item>);
static_assert(core::ShapeConstructed<ShardRR>);
static_assert(!core::BlockAllocated<ShardRR>, "a shaped create must not answer create(n)");
static_assert(!core::DirectConstructed<ShardRR>);
static_assert(core::Constructible<ShardRR>);
static_assert(!core::BlockAllocated<block::AllToAll<Item>>,
              "the block itself is not mistaken for a plain block-allocated queue");
// The reverse mistake, which the first version of the concept made: SingleBlock's variadic
// create() answers any arity, so a standalone queue looked shaped as well.
static_assert(!core::ShapeConstructed<queue::Vyukov<Item>>);
static_assert(core::Constructible<queue::Vyukov<Item>>);

TEST(ShardQueue, OneThreadHoldingARowAndAColumn) {
    registry::Instance<ShardRR> inst{64, 1, 1};
    auto& q = inst.get();
    EXPECT_EQ(q.capacity(), 64u);
    Item out = nullptr;
    EXPECT_FALSE(q.try_dequeue(out));
    EXPECT_EQ(out, nullptr);
    for (std::uint64_t i = 0; i < 64; ++i) ASSERT_TRUE(q.try_enqueue(item(i)));
    EXPECT_FALSE(q.try_enqueue(item(99)));
    EXPECT_EQ(q.size(), 64u);
    for (std::uint64_t i = 0; i < 64; ++i) {
        ASSERT_TRUE(q.try_dequeue(out));
        EXPECT_EQ(seq(out), i);
    }
}

TEST(ShardQueue, ANewInstanceDoesNotInheritAThreadsCachedHandle) {
    // Two instances in a row on one thread, possibly at the same address: the second must hand
    // out a fresh ticket against its own matrix, not reuse the first one's handle.
    for (int round = 0; round < 3; ++round) {
        registry::Instance<ShardSticky> inst{8, 1, 1};
        auto& q = inst.get();
        ASSERT_TRUE(q.try_enqueue(item(round)));
        Item out = nullptr;
        ASSERT_TRUE(q.try_dequeue(out));
        EXPECT_EQ(seq(out), static_cast<std::uint64_t>(round));
    }
}

TEST(ShardQueue, IsRegisteredOutsideAll) {
    EXPECT_EQ((registry::name_of<registry::Shard<Item>, ShardRR>), "a2a");
    EXPECT_EQ((registry::name_of<registry::Shard<Item>, ShardSticky>), "a2a-sticky");
    EXPECT_TRUE((registry::name_of<registry::All<Item>, ShardRR>).empty());
}

// ---------------------------------------------------------------------------------------------
// Threaded: built, not run by the automated checks. See the file comment.
// ---------------------------------------------------------------------------------------------

struct Shape {
    std::size_t p, c, capacity;
};

constexpr std::uint64_t kPerProducer = 200'000;

/// Items encode (producer, sequence) so per-pair FIFO can be checked at the consumer.
Item encode(std::size_t producer, std::uint64_t s) {
    return std::bit_cast<Item>(((s << 8) | producer) * 2 + 2);
}
void decode(Item v, std::size_t& producer, std::uint64_t& s) {
    const std::uint64_t raw = (std::bit_cast<std::uint64_t>(v) - 2) / 2;
    producer = raw & 0xff;
    s = raw >> 8;
}

template <typename B>
void RunThreaded(Shape sh) {
    SCOPED_TRACE(testing::Message() << "P=" << sh.p << " C=" << sh.c << " cap=" << sh.capacity);
    auto* b = B::create(sh.capacity, sh.p, sh.c);
    std::atomic<std::size_t> producers_left{sh.p};
    std::barrier start(static_cast<std::ptrdiff_t>(sh.p + sh.c));
    // Per consumer: every item it took, in order.
    std::vector<std::vector<Item>> taken(sh.c);

    std::vector<std::thread> ts;
    for (std::size_t i = 0; i < sh.p; ++i)
        ts.emplace_back([&, i] {
            auto p = b->producer(i);
            start.arrive_and_wait();
            for (std::uint64_t s = 0; s < kPerProducer; ++s)
                while (!p.enqueue(encode(i, s))) std::this_thread::yield();
            producers_left.fetch_sub(1, std::memory_order_release);
        });
    for (std::size_t j = 0; j < sh.c; ++j)
        ts.emplace_back([&, j] {
            auto c = b->consumer(j);
            auto& mine = taken[j];
            mine.reserve(kPerProducer * sh.p / sh.c + 1024);
            start.arrive_and_wait();
            Item out = nullptr;
            while (producers_left.load(std::memory_order_acquire) != 0)
                if (c.dequeue(out)) mine.push_back(out);
            while (c.dequeue(out)) mine.push_back(out); // the drain the benchmark relies on
        });
    for (auto& t : ts) t.join();

    std::vector<std::vector<bool>> seen(sh.p, std::vector<bool>(kPerProducer, false));
    std::uint64_t total = 0;
    for (std::size_t j = 0; j < sh.c; ++j) {
        // Per (producer, consumer) pair the order is the buffer's, so it is FIFO.
        std::vector<std::int64_t> last(sh.p, -1);
        for (Item v : taken[j]) {
            std::size_t pr = 0;
            std::uint64_t s = 0;
            decode(v, pr, s);
            ASSERT_LT(pr, sh.p);
            ASSERT_LT(s, kPerProducer);
            ASSERT_FALSE(seen[pr][s]) << "duplicate: producer " << pr << " seq " << s;
            seen[pr][s] = true;
            ASSERT_GT(static_cast<std::int64_t>(s), last[pr])
                << "per-pair FIFO violated: producer " << pr << " -> consumer " << j;
            last[pr] = static_cast<std::int64_t>(s);
            ++total;
        }
    }
    EXPECT_EQ(total, kPerProducer * sh.p) << "lost items";
    EXPECT_EQ(b->size(), 0u);
    B::destroy(b);
}

const std::vector<Shape> kShapes{
    {1, 1, 64}, {4, 4, 256}, {1, 4, 64}, {4, 1, 64}, {3, 5, 100}, {8, 8, 4096}, {2, 2, 4},
};

template <typename S>
struct ThreadedStrategies : testing::Test {};
TYPED_TEST_SUITE(ThreadedStrategies, StrategyTypes, StrategyNames);

TYPED_TEST(ThreadedStrategies, NoLossNoDuplicationPerPairFifo) {
    for (const Shape& sh : kShapes) RunThreaded<block::AllToAll<Item, TypeParam>>(sh);
}

TEST(ThreadedUnbounded, NoLossInOrder) {
    spsc::Unbounded<Item> q{16, 4};
    constexpr std::uint64_t kN = 2'000'000;
    std::thread prod{[&] {
        for (std::uint64_t i = 0; i < kN; ++i) q.enqueue(item(i));
    }};
    std::uint64_t next = 0;
    Item out = nullptr;
    while (next < kN)
        if (q.dequeue(out)) ASSERT_EQ(seq(out), next++);
    prod.join();
    EXPECT_FALSE(q.dequeue(out));
}

TEST(ThreadedShardQueue, TicketsHandOutEveryRowAndColumn) {
    constexpr std::size_t kP = 4, kC = 4;
    registry::Instance<ShardRR> inst{256, kP, kC};
    auto& q = inst.get();
    std::atomic<std::size_t> producers_left{kP};
    std::atomic<std::uint64_t> consumed{0};
    std::vector<std::thread> ts;
    for (std::size_t i = 0; i < kP; ++i)
        ts.emplace_back([&, i] {
            for (std::uint64_t s = 0; s < kPerProducer; ++s)
                while (!q.try_enqueue(encode(i, s))) std::this_thread::yield();
            producers_left.fetch_sub(1, std::memory_order_release);
        });
    for (std::size_t j = 0; j < kC; ++j)
        ts.emplace_back([&] {
            Item out = nullptr;
            std::uint64_t n = 0;
            while (producers_left.load(std::memory_order_acquire) != 0)
                if (q.try_dequeue(out)) ++n;
            while (q.try_dequeue(out)) ++n;
            consumed.fetch_add(n);
        });
    for (auto& t : ts) t.join();
    EXPECT_EQ(consumed.load(), kP * kPerProducer);
}

} // namespace
