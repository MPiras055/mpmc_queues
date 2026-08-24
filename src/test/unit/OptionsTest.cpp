/**
 * @file OptionsTest.cpp
 * @brief The option machinery, and the guarantee that turning constants into options changed
 *        none of them.
 *
 * `meta::OptionsPack` carries two kinds of entry. A **flag** is a plain tag, matched by type.
 * A **value** is a template -- `patience<2>` and `patience<8>` are different types -- read back
 * with `get<K, Default>` and named in an accepts-list as `meta::ValueOption<K>`. The value half
 * shipped unused for a long time; this is its first coverage.
 *
 * Most of what follows is static_asserts, deliberately. Every property here is a compile-time
 * one, and a runtime EXPECT would only confirm that the compiler agreed with itself. The
 * gtest bodies exist so a failure is reported against a named case rather than as a wall of
 * template diagnostics.
 *
 * @note The **defaults** section is the load-bearing one. Turning a hard-coded constant into an
 *       option is only a refactor if the default is the old literal, and a silent retune would
 *       otherwise be invisible -- it changes no interface and breaks no test.
 */
#include <gtest/gtest.h>

#include <algo/FAAArray.hpp>
#include <algo/HQ.hpp>
#include <algo/LFring.hpp>
#include <algo/PRQ.hpp>
#include <algo/PSCQ.hpp>
#include <algo/Vyukov.hpp>
#include <mem/source/Hazard.hpp>
#include <mem/source/Pool.hpp>
#include <meta/OptionsPack.hpp>
#include <proxy/Aliases.hpp>

#include <cstddef>
#include <type_traits>
#include <vector>

namespace {

struct Data {
    uint64_t seq;
};
using Item = Data*;

// ---------------------------------------------------------------------------
// The pack itself
// ---------------------------------------------------------------------------

struct FooOpt {
    struct flag_a {};
    struct flag_b {};
    template <auto N> struct patience {};
    template <auto N> struct period {};
};
/// A different owner's option that happens to share a name -- the case `accepts` exists for.
struct BarOpt {
    template <auto N> struct patience {};
};

using Empty = meta::EmptyOptions;
using Flags = meta::OptionsPack<FooOpt::flag_a>;
using Mixed = meta::OptionsPack<FooOpt::flag_a, FooOpt::patience<3>, FooOpt::period<512>>;

template <typename O>
concept FooAccepts =
    meta::AcceptsOnly<O, typename FooOpt::flag_a, typename FooOpt::flag_b,
                      meta::ValueOption<FooOpt::patience>, meta::ValueOption<FooOpt::period>>;

TEST(OptionsPack, ValueLookupFallsBackToTheDefault) {
    static_assert(Empty::get<FooOpt::patience, std::size_t{2}> == 2);
    static_assert(Flags::get<FooOpt::patience, std::size_t{2}> == 2,
                  "a pack holding only flags must still fall back");
    SUCCEED();
}

TEST(OptionsPack, ValueLookupFindsTheOptionWhenPresent) {
    static_assert(Mixed::get<FooOpt::patience, std::size_t{2}> == 3);
    static_assert(Mixed::get<FooOpt::period, std::size_t{256}> == 512);
    SUCCEED();
}

TEST(OptionsPack, FlagsAndValuesCoexist) {
    static_assert(Mixed::has<FooOpt::flag_a>);
    static_assert(!Mixed::has<FooOpt::flag_b>);
    SUCCEED();
}

TEST(OptionsPack, AcceptsAdmitsValueOptionsAndRejectsForeignOnes) {
    static_assert(FooAccepts<Empty>);
    static_assert(FooAccepts<Flags>);
    static_assert(FooAccepts<Mixed>);
    // The negative cases are the point: `has<>` answers false for anything it does not
    // recognise, so without `accepts` a misspelled or foreign option reads as "not requested"
    // and silently does nothing.
    static_assert(!FooAccepts<meta::OptionsPack<BarOpt::patience<8>>>,
                  "a value option belonging to another component must be rejected");
    static_assert(!FooAccepts<meta::OptionsPack<int>>);
    static_assert(!FooAccepts<meta::OptionsPack<FooOpt::flag_a, BarOpt::patience<1>>>);
    SUCCEED();
}

/**
 * The result type follows whichever side supplied the value, so a caller writing
 * `patience<3>` gets `int` and one writing `patience<3ull>` gets `unsigned long long`. Every
 * use site in the tree therefore casts. Pinned here because the trap is silent: the value is
 * right and only the type differs, so it surfaces later as a sign-extension or a narrowing.
 */
TEST(OptionsPack, ValueTypeFollowsTheOptionNotTheDefault) {
    static_assert(std::is_same_v<decltype(Empty::get<FooOpt::patience, std::size_t{2}>),
                                 const std::size_t>);
    static_assert(std::is_same_v<decltype(Mixed::get<FooOpt::patience, std::size_t{2}>),
                                 const int>,
                  "if this ever becomes size_t the casts at the use sites can go");
    SUCCEED();
}

// ---------------------------------------------------------------------------
// Defaults: the refactor must not have retuned anything
// ---------------------------------------------------------------------------

/**
 * Each of these literals is what the constant was before it became an option. A silent retune
 * would change no interface and break no other test, so this is the only thing standing
 * between "made configurable" and "made configurable and quietly different".
 */
TEST(OptionDefaults, AlgorithmsKeepTheirPreviousConstants) {
    static_assert(seg::PRQ<Item>::max_dequeue_retries == 4 * 1024);
    static_assert(seg::PRQ<Item>::tail_reload_period == 1u << 8);

    static_assert(queue::PSCQ<Item>::max_dequeue_retries == 4 * 1024);
    static_assert(queue::PSCQ<Item>::tail_reload_period == 1ull << 8);

    static_assert(queue::IndexRing<>::max_dequeue_retries == 1024 * 10);
    static_assert(queue::IndexRing<>::tail_reload_period == 1ull << 8);

    static_assert(seg::FAAArray<Item>::patience == 1024);
    static_assert(seg::HQ<Item>::patience == 2);
    SUCCEED();
}

TEST(OptionDefaults, SourcesKeepTheirPreviousConstants) {
    using H = proxy::HazardSource<seg::Vyukov<Item>>;
    static_assert(H::retire_threshold == 64, "was the HAZARD_RETIRE_THRESHOLD macro's value");

    constexpr std::size_t N = 8;
    using P = mem::source::Pool<seg::Vyukov<Item, meta::EmptyOptions, mem::IndexHandle<N>>, N,
                                proxy::ThreadMeta<mem::VersionedIndex<N>>>;
    static_assert(P::max_acquire_spins == 64);
    static_assert(P::max_advance_attempts == 4, "one rotation window");

    // The proxy's own retry budget. Small on purpose: under a pooled source an empty pool is
    // also how the memory bound is reported, so this is bounded to keep a genuinely full queue
    // refusing rather than spinning.
    using Q = proxy::MemBounded<Item, seg::Vyukov<Item, meta::EmptyOptions, mem::IndexHandle<N>>, N>;
    static_assert(Q::acquire_retries == 2);
    SUCCEED();
}

/// The other half: an option must actually reach the member it names.
TEST(OptionDefaults, OptionsOverrideTheDefaults) {
    using PRQt = algo::PRQ<Item,
                           meta::OptionsPack<algo::PRQOpt::max_dequeue_retries<64>,
                                             algo::PRQOpt::tail_reload_period<16>>,
                           linkage::Node<mem::PtrHandle>>;
    static_assert(PRQt::max_dequeue_retries == 64);
    static_assert(PRQt::tail_reload_period == 16);

    using Ht = algo::HQ<Item, meta::OptionsPack<algo::HQOpt::patience<8>>,
                        linkage::Node<mem::PtrHandle>>;
    static_assert(Ht::patience == 8);

    using Hz = mem::source::Hazard<seg::Vyukov<Item>, proxy::ThreadMeta<seg::Vyukov<Item>*>,
                                   meta::OptionsPack<mem::source::HazardOpt::retire_threshold<4>>>;
    static_assert(Hz::retire_threshold == 4);
    SUCCEED();
}

/// Naming a tuned segment is one thing; instantiating and driving it is another. A constraint
/// that rejected the value option, or a `get<>` that failed to bind, shows up here rather than
/// in a static_assert that was never instantiated.
TEST(OptionDefaults, TunedWriteOnceSegmentsStillWork) {
    using FAAd = seg::FAAArray<Item>;
    using FAAt = algo::FAAArray<Item, meta::OptionsPack<algo::FAAArrayOpt::patience<8>>,
                                linkage::Node<mem::PtrHandle>>;
    using HQd = seg::HQ<Item>;
    using HQt = algo::HQ<Item, meta::OptionsPack<algo::HQOpt::patience<8>>,
                         linkage::Node<mem::PtrHandle>>;
    static_assert(!std::is_same_v<FAAd, FAAt>);
    static_assert(!std::is_same_v<HQd, HQt>);

    // Instantiate them: a constraint that rejected the value option would fail here.
    FAAt* f = mem::SingleBlock<FAAt>::create(8);
    HQt* h = mem::SingleBlock<HQt>::create(8);
    Data d{1};
    EXPECT_TRUE(f->enqueue(&d));
    EXPECT_TRUE(h->enqueue(&d));
    Item out = nullptr;
    EXPECT_TRUE(f->dequeue(out));
    EXPECT_TRUE(h->dequeue(out));
    mem::SingleBlock<FAAt>::destroy(f);
    mem::SingleBlock<HQt>::destroy(h);
}

TEST(OptionDefaults, SourcesAcceptTheirTuning) {
    // Hazard's threshold was a macro; a tuned source must still be a usable source.
    using Tuned = mem::source::Hazard<seg::Vyukov<Item>, proxy::ThreadMeta<seg::Vyukov<Item>*>,
                                      meta::OptionsPack<mem::source::HazardOpt::retire_threshold<4>>>;
    static_assert(core::SegmentSource<Tuned, seg::Vyukov<Item>>);

    constexpr std::size_t N = 8;
    using Seg = seg::Vyukov<Item, meta::EmptyOptions, mem::IndexHandle<N>>;
    using TunedPool =
        mem::source::Pool<Seg, N, proxy::ThreadMeta<mem::VersionedIndex<N>>,
                          meta::OptionsPack<mem::source::PoolOpt::max_acquire_spins<8>,
                                            mem::source::PoolOpt::max_advance_attempts<2>>>;
    static_assert(core::SegmentSource<TunedPool, Seg>);
    SUCCEED();
}

/// End to end: a tuned source under a real proxy still moves items.
/// The proxy's retry budget is reachable through the same value-option channel as the rest.
TEST(OptionDefaults, TheProxyRetryBudgetIsTunable) {
    constexpr std::size_t N = 8;
    using Seg = seg::Vyukov<Item, meta::EmptyOptions, mem::IndexHandle<N>>;
    using Tuned = proxy::MemBounded<Item, Seg, N, meta::EmptyOptions,
                                    meta::OptionsPack<proxy::ProxyOpt::acquire_retries<7>>>;
    static_assert(Tuned::acquire_retries == 7);

    // Zero is meaningful rather than degenerate: it turns the retry off and restores the
    // straight "empty means the bound" behaviour.
    using Off = proxy::MemBounded<Item, Seg, N, meta::EmptyOptions,
                                  meta::OptionsPack<proxy::ProxyOpt::acquire_retries<0>>>;
    static_assert(Off::acquire_retries == 0);
    SUCCEED();
}

TEST(OptionDefaults, ATunedProxyStillWorks) {
    constexpr std::size_t N = 8;
    using Seg = seg::HQ<Item, meta::OptionsPack<algo::HQOpt::patience<8>>, mem::IndexHandle<N>>;
    using Q = proxy::MemBounded<Item, Seg, N,
                                meta::OptionsPack<mem::source::PoolOpt::max_acquire_spins<8>>>;
    Q q{8};
    auto s = q.join();
    ASSERT_TRUE(s);

    std::vector<Data> store(16);
    std::size_t placed = 0;
    for (std::size_t i = 0; i < store.size(); ++i) {
        store[i] = {i + 1};
        if (q.enqueue(&store[i])) ++placed;
    }
    ASSERT_GT(placed, 0u);
    Item out = nullptr;
    std::size_t taken = 0;
    while (q.dequeue(out)) ++taken;
    EXPECT_EQ(taken, placed);
}

// ---------------------------------------------------------------------------
// Segment stats
// ---------------------------------------------------------------------------

constexpr std::size_t kPool = 8;
using StatSeg = seg::Vyukov<Item, meta::EmptyOptions, mem::IndexHandle<kPool>>;
using Plain = proxy::MemBounded<Item, StatSeg, kPool>;
using Counted = proxy::MemBounded<Item, StatSeg, kPool, meta::EmptyOptions,
                                  meta::OptionsPack<proxy::ProxyOpt::segment_stats>>;

/// The whole justification for making the counters optional: off, they must occupy nothing.
TEST(SegmentStats, DisabledCountersAreFree) {
    EXPECT_LT(sizeof(Plain), sizeof(Counted))
        << "enabling stats added nothing, so the counters are not actually being carried";
    // Three counters' worth at minimum. Deliberately not asserted against a padded layout:
    // the counters are packed rather than cache-line separated, trading the false sharing
    // between a linking producer and a retiring consumer for a smaller proxy -- they are
    // written once per segment, so the sharing costs little and the footprint is the thing
    // worth keeping small.
    EXPECT_GE(sizeof(Counted) - sizeof(Plain), 3 * sizeof(uint64_t));
}

TEST(SegmentStats, SentinelCountsAsASegmentInService) {
    Counted q{8};
    auto s = q.join();
    ASSERT_TRUE(s);
    EXPECT_EQ(q.segments_linked(), 1u) << "the sentinel is a segment in service";
    EXPECT_EQ(q.segments_retired(), 0u);
    EXPECT_EQ(q.segments_discarded(), 0u);
}

TEST(SegmentStats, LinksAndRetiresBalanceOverAFillAndDrain) {
    constexpr std::size_t kSegment = 4;
    Counted q{kSegment};
    auto s = q.join();
    ASSERT_TRUE(s);

    std::vector<Data> store(kSegment * 3);
    std::size_t placed = 0;
    for (std::size_t i = 0; i < store.size(); ++i) {
        store[i] = {i + 1};
        if (q.enqueue(&store[i])) ++placed;
    }
    ASSERT_EQ(placed, store.size()) << "the pool should hold this comfortably";

    // Crossing a segment boundary must have linked something beyond the sentinel.
    const uint64_t linked = q.segments_linked();
    EXPECT_GT(linked, 1u) << placed << " items over " << kSegment << "-slot segments linked none";
    // Single-threaded, so nobody can lose a link race.
    EXPECT_EQ(q.segments_discarded(), 0u);

    Item out = nullptr;
    std::size_t taken = 0;
    while (q.dequeue(out)) ++taken;
    EXPECT_EQ(taken, placed);

    // A segment is retired only once a successor exists, so the last one stays in service.
    EXPECT_EQ(q.segments_retired(), linked - 1)
        << "every segment but the current tail should have been retired";
    EXPECT_EQ(q.segments_linked(), linked) << "draining must not link anything";
}

/// The number the counters exist for.
TEST(SegmentStats, ReuseFactorExceedsThePoolAcrossManyCycles) {
    constexpr std::size_t kSegment = 2;
    Counted q{kSegment};
    auto s = q.join();
    ASSERT_TRUE(s);

    std::vector<Data> store(4);
    for (std::size_t i = 0; i < store.size(); ++i) store[i] = {i + 1};

    // Fill and drain repeatedly: the pool is fixed, so segments must be recycled to keep up.
    //
    // A refusal is legitimate rather than a failure -- under a pooled source that *is* the
    // memory bound, and reclamation is deferred by two epoch advances, which a single thread
    // only drives from inside acquire(). So this does what a real caller does: drain and try
    // again, and only give up if the queue refuses an item it has just made room for.
    Item out = nullptr;
    std::size_t enqueued = 0, dequeued = 0;
    for (int cycle = 0; cycle < 50; ++cycle) {
        for (auto& d : store) {
            // A refusal here is expected rather than a failure, and draining alone does not
            // clear it. Reclamation is deferred by two epoch advances, and a *single* thread
            // only ever drives an advance from inside acquire() -- which it reaches only by
            // attempting an enqueue. One attempt therefore moves the rotation one stage, so a
            // segment retired a moment ago takes a couple of attempts to come back round.
            // A real caller loops on enqueue and never notices; this spells it out.
            bool ok = false;
            for (int attempt = 0; attempt < 8 && !ok; ++attempt) {
                ok = q.enqueue(&d);
                if (!ok)
                    while (q.dequeue(out)) ++dequeued;
            }
            ASSERT_TRUE(ok) << "still refusing after eight attempts on a drained pool, cycle "
                            << cycle;
            ++enqueued;
        }
        while (q.dequeue(out)) ++dequeued;
    }
    EXPECT_EQ(enqueued, dequeued) << "items lost across the recycling cycles";

    // The point: far more segments passed through service than the pool physically holds.
    EXPECT_GT(q.segments_linked(), kPool)
        << "no slot was ever reused, so nothing here measured recycling";
    const double reuse = double(q.segments_linked()) / double(kPool);
    EXPECT_GT(reuse, 1.0) << "reuse factor " << reuse;
    // Single-threaded, so no producer can ever lose a link race.
    EXPECT_EQ(q.segments_discarded(), 0u);
}

} // namespace
