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
#include <algo/SCQ.hpp>
#include <algo/Spin.hpp>
#include <algo/VyukovNoABA.hpp>
#include <algo/VyukovDCAS.hpp>
#include <algo/Mutex.hpp>
#include <algo/Vyukov.hpp>
#include <mem/source/Hazard.hpp>
#include <mem/source/Pool.hpp>
#include <meta/OptionsPack.hpp>
#include <proxy/Aliases.hpp>

#include <cstddef>
#include <type_traits>
#include <chrono>
#include <thread>
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
    static_assert(seg::HQ<Item>::patience == 1024);
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
/**
 * @brief `no_pow2` really takes the size as asked, and the queue still works without a mask.
 *
 * The default rounds, so the exact-size paths are otherwise never compiled or run. Three of
 * these are newly written fallbacks -- PSCQ and VyukovDCAS swap a mask for a modulo, and
 * VyukovNoABA additionally swaps the `>> shift_` that derives its *lap number* for a division.
 * That last one is the one to distrust: get it wrong and cells appear permanently occupied or
 * permanently empty, which a round-trip past the wrap point catches and a single push/pop does
 * not. Hence 100 items through a 3-slot ring below.
 */
/// @param honours_capacity false for PSCQ only -- see the call site.
template <typename Q>
void exact_size_round_trips(bool honours_capacity = true) {
    constexpr std::size_t kOdd = 100;
    Q* q = mem::SingleBlock<Q>::create(kOdd);
    EXPECT_EQ(q->capacity(), kOdd) << "no_pow2 did not take the size as asked";

    std::vector<Data> store(kOdd);
    for (std::size_t i = 0; i < kOdd; ++i) store[i] = {i + 1};
    for (auto& d : store) ASSERT_TRUE(q->try_enqueue(&d));
    if (honours_capacity) {
        EXPECT_FALSE(q->try_enqueue(&store[0])) << "accepted past its stated capacity";
    }

    for (std::size_t i = 0; i < kOdd; ++i) {
        Item out = nullptr;
        ASSERT_TRUE(q->try_dequeue(out)) << "ran dry after " << i << " of " << kOdd;
        EXPECT_EQ(out->seq, i + 1) << "FIFO order broken at " << i;
    }
    mem::SingleBlock<Q>::destroy(q);
}

/// Drive a small ring well past its wrap point, which is where an index or lap that is computed
/// wrongly on the modulo path actually shows up.
template <typename Q>
void exact_size_wraps() {
    Q* q = mem::SingleBlock<Q>::create(3);
    ASSERT_EQ(q->capacity(), 3u);
    Data d{7};
    for (int lap = 0; lap < 100; ++lap) {
        ASSERT_TRUE(q->try_enqueue(&d)) << "full at lap " << lap << " with nothing queued";
        Item out = nullptr;
        ASSERT_TRUE(q->try_dequeue(out)) << "empty at lap " << lap << " right after an enqueue";
        EXPECT_EQ(out->seq, 7u);
    }
    mem::SingleBlock<Q>::destroy(q);
}

TEST(OptionDefaults, TheExactSizeOptOutWorksEverywhereItIsOffered) {
    using L = linkage::None;
    exact_size_round_trips<algo::Vyukov<Item, meta::OptionsPack<algo::VyukovOpt::no_pow2>, L>>();
    exact_size_round_trips<
        algo::VyukovDCAS<Item, meta::OptionsPack<algo::VyukovDCASOpt::no_pow2>, L>>();
    exact_size_round_trips<
        algo::VyukovNoABA<Item, meta::OptionsPack<algo::VyukovNoABAOpt::no_pow2>, L>>();
    // PSCQ is passed false because it does not honour its own capacity() -- a *pre-existing*
    // defect, unrelated to the size rounding and equally present with the default: its fullness
    // check in enqueue() tests against `size_`, the physical ring, while capacity() reports
    // `size_ >> 1`, the usable figure its own header documents ("the physical ring is twice the
    // usable capacity"). So it reports 128 and accepts 256. Left alone here rather than fixed in
    // passing, because halving pscq's effective capacity changes what every recorded benchmark
    // for it measured. The rest of this helper -- exact sizing, FIFO order, the modulo indexing
    // -- is still checked for it.
    exact_size_round_trips<algo::PSCQ<Item, meta::OptionsPack<algo::PSCQOpt::no_pow2>, L>>(false);
    exact_size_round_trips<algo::Mutex<Item, meta::OptionsPack<algo::MutexOpt::no_pow2>, L>>();

    exact_size_wraps<algo::Vyukov<Item, meta::OptionsPack<algo::VyukovOpt::no_pow2>, L>>();
    exact_size_wraps<algo::VyukovDCAS<Item, meta::OptionsPack<algo::VyukovDCASOpt::no_pow2>, L>>();
    exact_size_wraps<
        algo::VyukovNoABA<Item, meta::OptionsPack<algo::VyukovNoABAOpt::no_pow2>, L>>();
    exact_size_wraps<algo::PSCQ<Item, meta::OptionsPack<algo::PSCQOpt::no_pow2>, L>>();
    exact_size_wraps<algo::Mutex<Item, meta::OptionsPack<algo::MutexOpt::no_pow2>, L>>();

    // PRQ only exists as a segment -- it requires a linked linkage -- but the ring underneath
    // is the same, so it gets the same treatment with the linkage it insists on.
    using PRQx = algo::PRQ<Item, meta::OptionsPack<algo::PRQOpt::no_pow2>,
                           linkage::Node<mem::PtrHandle>>;
    exact_size_round_trips<PRQx>();
    exact_size_wraps<PRQx>();
}

/// FAAArray and HQ take the option too, but for them it is a pure sizing choice -- they walk
/// their cells linearly and close, so there is no wrap to get wrong. Segments rather than
/// standalone queues, hence the separate case.
TEST(OptionDefaults, TheExactSizeOptOutSizesWriteOnceSegments) {
    using FAAx = algo::FAAArray<Item, meta::OptionsPack<algo::FAAArrayOpt::no_pow2>,
                                linkage::Node<mem::PtrHandle>>;
    using HQx = algo::HQ<Item, meta::OptionsPack<algo::HQOpt::no_pow2>,
                         linkage::Node<mem::PtrHandle>>;
    static_assert(FAAx::capacity_for(100) == 100);
    static_assert(HQx::capacity_for(100) == 100);
    // ...and the default still rounds.
    static_assert(seg::FAAArray<Item>::capacity_for(100) == 128);
    static_assert(seg::HQ<Item>::capacity_for(100) == 128);

    FAAx* f = mem::SingleBlock<FAAx>::create(100);
    HQx* h = mem::SingleBlock<HQx>::create(100);
    Data d{1};
    EXPECT_TRUE(f->enqueue(&d));
    EXPECT_TRUE(h->enqueue(&d));
    Item out = nullptr;
    EXPECT_TRUE(f->try_dequeue(out));
    EXPECT_TRUE(h->try_dequeue(out));
    mem::SingleBlock<FAAx>::destroy(f);
    mem::SingleBlock<HQx>::destroy(h);
}

/// LFring and SCQ deliberately offer no opt-out: LFring keeps its size as an *order* and SCQ is
/// built on two of them, so a non-power-of-two is not expressible rather than merely slower.
/// Handing either a `no_pow2` tag is a compile error through meta::AcceptsOnly; assert the
/// positive property here, since a negative-compilation harness is not available.
/**
 * @brief The condition-variable wait is on for a standalone queue and off for a linked segment.
 *
 * Asserted as a constant rather than by timing, because it is a correctness property, not a
 * performance one: a *linked* segment that parked when full would stall the proxy outright. The
 * proxy reads "full" as "link a successor", so waiting there is waiting for room that this
 * segment is never going to have.
 */
TEST(LockBasedControls, OnlyAStandaloneMutexParks) {
    static_assert(queue::Mutex<Item>::parks_when_blocked,
                  "the standalone control should exercise the condition variables");
    static_assert(!seg::Mutex<Item>::parks_when_blocked,
                  "a linked segment must refuse instantly so the proxy links a successor");

    // The wake policy is the tunable, and notify_one is the default: one enqueue creates work
    // for exactly one consumer, so waking the rest is the herd the two variables avoid.
    static_assert(!queue::Mutex<Item>::notify_all_policy);
    using WakeAll = queue::Mutex<Item, meta::OptionsPack<algo::MutexOpt::notify_all>>;
    static_assert(WakeAll::notify_all_policy);
}

/// try_dequeue never waits, which is what keeps every generic drain in the tree terminating.
/// The blocking dequeue() on an empty queue is covered below, where a close() releases it.
TEST(LockBasedControls, TryDequeueNeverWaits) {
    using Q = queue::Mutex<Item>;
    Q* q = mem::SingleBlock<Q>::create(4);
    Item out = nullptr;
    EXPECT_FALSE(q->try_dequeue(out)) << "an empty queue reported an item";
    Data d{1};
    EXPECT_TRUE(q->try_enqueue(&d));
    EXPECT_TRUE(q->try_dequeue(out));
    EXPECT_FALSE(q->try_dequeue(out));
    mem::SingleBlock<Q>::destroy(q);
}

/**
 * @brief A consumer parked in the blocking dequeue() is released by close(), not by a timeout.
 *
 * This is the one situation the unbounded wait exists for, and the reason close() notifies
 * *both* variables: without it a consumer on an empty queue parks for ever. The test would hang
 * rather than fail if that broke, so it carries its own deadline and reports.
 */
TEST(LockBasedControls, CloseReleasesAParkedConsumer) {
    using Q = queue::Mutex<Item>;
    Q* q = mem::SingleBlock<Q>::create(4);

    std::atomic<bool> returned{false};
    std::atomic<bool> took{true};
    std::thread consumer{[&] {
        Item out = nullptr;
        took.store(q->dequeue(out));      // blocks: the queue is empty and open
        returned.store(true, std::memory_order_release);
    }};

    q->close();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!returned.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();

    EXPECT_TRUE(returned.load()) << "close() did not release a consumer parked in dequeue()";
    EXPECT_FALSE(took.load()) << "an empty closed queue handed back an item";
    consumer.join();
    mem::SingleBlock<Q>::destroy(q);
}

/// The producer side of the same contract: parked on a full queue, released by close().
TEST(LockBasedControls, CloseReleasesAParkedProducer) {
    using Q = queue::Mutex<Item>;
    Q* q = mem::SingleBlock<Q>::create(2);
    std::vector<Data> store(8);
    for (std::size_t i = 0; i < store.size(); ++i) store[i] = {i + 1};
    while (q->try_enqueue(&store[0])) {}          // fill it

    std::atomic<bool> returned{false};
    std::atomic<bool> placed{true};
    std::thread producer{[&] {
        placed.store(q->enqueue(&store[1]));      // blocks: the queue is full and open
        returned.store(true, std::memory_order_release);
    }};

    q->close();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!returned.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();

    EXPECT_TRUE(returned.load()) << "close() did not release a producer parked in enqueue()";
    EXPECT_FALSE(placed.load()) << "a closed queue accepted an item";
    producer.join();
    mem::SingleBlock<Q>::destroy(q);
}

/// A closed queue still drains, and closing twice is a no-op. Both matter to the termination
/// story: the owner closes once production ends and then collects what is left.
TEST(LockBasedControls, AClosedQueueStillDrainsAndCloseIsIdempotent) {
    using Q = queue::Mutex<Item>;
    Q* q = mem::SingleBlock<Q>::create(4);
    std::vector<Data> store(4);
    for (std::size_t i = 0; i < store.size(); ++i) store[i] = {i + 1};
    for (auto& d : store) ASSERT_TRUE(q->try_enqueue(&d));

    q->close();
    EXPECT_TRUE(q->is_closed());
    q->close();                                    // idempotent
    EXPECT_TRUE(q->is_closed());

    // dequeue() past a close behaves as try_dequeue(): it hands back what is there, then false.
    for (std::size_t i = 0; i < store.size(); ++i) {
        Item out = nullptr;
        ASSERT_TRUE(q->dequeue(out)) << "a closed queue dropped item " << i;
        EXPECT_EQ(out->seq, i + 1) << "FIFO order broken across the close";
    }
    Item out = nullptr;
    EXPECT_FALSE(q->dequeue(out)) << "a drained closed queue reported an item";
    EXPECT_FALSE(q->enqueue(&store[0])) << "a closed queue accepted an item";
    mem::SingleBlock<Q>::destroy(q);
}

/// Spin is the other lock-based control: same ring, same critical sections, a spinlock instead
/// of a mutex, and no waiting on the queue's state at all. That last part is what makes the
/// pair a measurement of lock acquisition rather than of blocking policy.
TEST(LockBasedControls, SpinRoundTripsAndIsTunable) {
    using Q = queue::Spin<Item>;
    static_assert(Q::spins_before_park == 64);
    using Futex = queue::Spin<Item, meta::OptionsPack<algo::SpinOpt::spins_before_park<0>>>;
    static_assert(Futex::spins_before_park == 0, "zero should park immediately");

    Q* q = mem::SingleBlock<Q>::create(4);
    std::vector<Data> store(4);
    for (std::size_t i = 0; i < store.size(); ++i) store[i] = {i + 1};
    for (auto& d : store) ASSERT_TRUE(q->try_enqueue(&d));
    EXPECT_FALSE(q->try_enqueue(&store[0])) << "accepted past capacity";
    for (std::size_t i = 0; i < store.size(); ++i) {
        Item out = nullptr;
        ASSERT_TRUE(q->try_dequeue(out));
        EXPECT_EQ(out->seq, i + 1) << "FIFO order broken under the spinlock";
    }
    Item drained = nullptr;
    EXPECT_FALSE(q->try_dequeue(drained)) << "reported an item when empty";
    mem::SingleBlock<Q>::destroy(q);
}

TEST(OptionDefaults, TheTwoOrderBasedAlgorithmsAlwaysRound) {
    static_assert(seg::SCQ<Item>::capacity_for(100) == 128);
    static_assert(seg::SCQ<Item>::capacity_for(1000) == 1024);
    SUCCEED();
}

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
    EXPECT_TRUE(f->try_dequeue(out));
    EXPECT_TRUE(h->try_dequeue(out));
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
        if (q.try_enqueue(&store[i])) ++placed;
    }
    ASSERT_GT(placed, 0u);
    Item out = nullptr;
    std::size_t taken = 0;
    while (q.try_dequeue(out)) ++taken;
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
        if (q.try_enqueue(&store[i])) ++placed;
    }
    ASSERT_EQ(placed, store.size()) << "the pool should hold this comfortably";

    // Crossing a segment boundary must have linked something beyond the sentinel.
    const uint64_t linked = q.segments_linked();
    EXPECT_GT(linked, 1u) << placed << " items over " << kSegment << "-slot segments linked none";
    // Single-threaded, so nobody can lose a link race.
    EXPECT_EQ(q.segments_discarded(), 0u);

    Item out = nullptr;
    std::size_t taken = 0;
    while (q.try_dequeue(out)) ++taken;
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
                ok = q.try_enqueue(&d);
                if (!ok)
                    while (q.try_dequeue(out)) ++dequeued;
            }
            ASSERT_TRUE(ok) << "still refusing after eight attempts on a drained pool, cycle "
                            << cycle;
            ++enqueued;
        }
        while (q.try_dequeue(out)) ++dequeued;
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
