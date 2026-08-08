/**
 * @file ThreadRegistryTest.cpp
 * @brief The lock-free thread registry both reclamation sources are built on.
 *
 * util::threading::ThreadRegistry has no cap and hands out no index: a thread gets a
 * reference to its own `ThreadData`, stored in a node the registry owns. What has to hold:
 *
 *   1. a scan visits every thread that stays attached for the whole walk;
 *   2. a detached node comes back for reuse exactly once, so the registry does not grow;
 *   3. a thread's payload survives its node being handed to somebody else.
 *
 * Miss (1) and a hazard scan frees an object somebody is reading. Miss (2) and the node count
 * climbs without bound -- which is exactly how the first free list here failed, popping by
 * `exchange` and allocating whenever a concurrent attach landed in the window. Miss (3) and a
 * detaching thread's pending retirements are dropped.
 *
 * The failures this file has caught were intermittent at roughly one run in two, so the churn
 * cases are deliberately long enough to be worth repeating.
 */
#include <gtest/gtest.h>

#include <util/threading/ThreadRegistry.hpp>

#include <atomic>
#include <iostream>
#include <memory>
#include <set>
#include <thread>
#include <vector>

using util::threading::ThreadRegistry;
using util::threading::ThreadRegistryOpt;

namespace {

/// Both shapes the real sources use: a word other threads read, and a list this thread owns.
/// Aligned because the registry deliberately does not over-align its nodes.
struct alignas(CACHE_LINE) Payload {
    std::atomic<int> published{0};
    std::vector<int> owned;
};

using Registry = ThreadRegistry<Payload>;

/// Spawn @p n threads, wait until all have attached, run @p body, then release them.
template <typename Body>
void with_attached(Registry& reg, std::size_t n, Body body) {
    std::atomic<std::size_t> up{0};
    std::atomic<bool> release{false};
    std::vector<std::thread> ts;
    for (std::size_t i = 0; i < n; ++i)
        ts.emplace_back([&, i] {
            reg.attach();
            reg.self()->data.published.store(static_cast<int>(i) + 1);
            ++up;
            while (!release.load()) {}
            reg.detach();
        });
    while (up.load() < n) {}
    body();
    release = true;
    for (auto& t : ts) t.join();
}

struct Fixture : ::testing::Test {
    /// A hint, not a cap. The free queue also holds a permanent dummy, so the registry
    /// starts with kHint + 1 nodes.
    static constexpr std::size_t kHint = 4;
    Registry reg{kHint};
};

TEST(ThreadRegistryStandalone, TheFreeQueueIsLockFreeOnThisTarget) {
    // Reported rather than asserted: a target without a double-width CAS still behaves
    // correctly, it just takes a lock inside std::atomic. But a silent fallback where the
    // instruction *should* be available -- -mcx16 not reaching the translation unit -- would
    // quietly undo the point of the counted pointer, so it is worth seeing.
    std::cout << "[          ] free_list_is_lock_free = " << std::boolalpha
              << Registry::free_list_is_lock_free << ", sizeof(TaggedPtr) = "
              << sizeof(Registry::TaggedPtr) << "\n";
    SUCCEED();
}

// ===== attach / detach =======================================================

TEST_F(Fixture, AttachIsIdempotentAndSoIsDetach) {
    EXPECT_EQ(reg.self(), nullptr) << "a thread starts unattached";

    reg.attach();
    auto* first = reg.self();
    ASSERT_NE(first, nullptr);

    reg.attach();
    EXPECT_EQ(reg.self(), first) << "a second attach handed out a different node";

    reg.detach();
    EXPECT_EQ(reg.self(), nullptr);
    reg.detach(); // must not fault or corrupt either list
    EXPECT_EQ(reg.active_count(), 0u);
}

TEST_F(Fixture, ThePayloadReferenceIsStableWhileAttached) {
    // The whole point of dropping the index: a thread holds a reference, not a subscript.
    reg.attach();
    Payload* p = &reg.self()->data;
    for (int i = 0; i < 100; ++i) ASSERT_EQ(&reg.self()->data, p);
    reg.detach();
}

TEST_F(Fixture, ADetachedNodeIsHandedOutAgain) {
    reg.attach();
    const std::size_t nodes = reg.node_count();
    reg.detach();

    reg.attach();
    ASSERT_NE(reg.self(), nullptr);
    // Reuse is asserted by the node count holding still, not by pointer identity: the free
    // list is a FIFO queue, so a detached node goes to the back and the *oldest* free node
    // comes out. Which node a thread gets is not part of the contract; that it did not have
    // to allocate one is.
    EXPECT_EQ(reg.node_count(), nodes) << "a node was allocated when one was already free";
    reg.detach();
}

TEST_F(Fixture, ReuseIsFirstInFirstOut) {
    // Not load-bearing, but it pins the queue's ordering so a silent regression to a stack
    // shows up here rather than as a surprise somewhere downstream.
    reg.attach();
    Payload* a = &reg.self()->data;
    reg.detach();
    reg.attach();
    Payload* b = &reg.self()->data;
    reg.detach();

    EXPECT_NE(a, b) << "the node just released came straight back: this is a stack, not a queue";
}

// ===== unbounded =============================================================

TEST(ThreadRegistryStandalone, ThereIsNoCapAndTheHintIsOnlyAHint) {
    // DTT_MAX_BITS capped this at 1024 at compile time; the array-backed version capped it
    // at the constructor argument. Both are gone.
    Registry reg{2};
    EXPECT_EQ(reg.node_count(), 3u) << "hint of 2, plus the free queue's dummy";

    constexpr std::size_t kThreads = 32;
    with_attached(reg, kThreads, [&] {
        EXPECT_EQ(reg.active_count(), kThreads) << "attaching past the hint failed";
        EXPECT_GE(reg.node_count(), kThreads);
    });

    EXPECT_EQ(reg.active_count(), 0u);
    EXPECT_EQ(reg.node_count(), kThreads + 1) << "grew beyond what was actually needed";
}

TEST(ThreadRegistryStandalone, EveryAttachedThreadGetsADistinctPayload) {
    Registry reg{0};
    constexpr std::size_t kThreads = 16;

    std::mutex m;
    std::set<const Payload*> seen;
    std::atomic<std::size_t> up{0};
    std::atomic<bool> release{false};
    std::vector<std::thread> ts;
    for (std::size_t i = 0; i < kThreads; ++i)
        ts.emplace_back([&] {
            reg.attach();
            {
                std::lock_guard lk{m};
                seen.insert(&reg.self()->data);
            }
            ++up;
            while (!release.load()) {}
            reg.detach();
        });
    while (up.load() < kThreads) {}
    release = true;
    for (auto& t : ts) t.join();

    EXPECT_EQ(seen.size(), kThreads) << "two threads were handed the same payload";
}

// ===== the two attach forms ==================================================

TEST(ThreadRegistryStandalone, TryAttachNeverAllocates) {
    Registry reg{1};
    ASSERT_TRUE(reg.try_attach());
    EXPECT_EQ(reg.node_count(), 2u); // the one node, plus the dummy

    // A second thread finds the queue down to its dummy. try_attach must refuse, not grow.
    std::thread other{[&] {
        EXPECT_FALSE(reg.try_attach());
        EXPECT_EQ(reg.node_count(), 2u);
        EXPECT_EQ(reg.self(), nullptr);
    }};
    other.join();
    reg.detach();
}

TEST(ThreadRegistryStandalone, AttachWithACallerNodeAdoptsItAndNeverAllocates) {
    // The lock-free way in: the caller pays for the allocation off the critical path.
    Registry reg{0};
    auto* n = new Registry::Node();
    ASSERT_TRUE(reg.attach(n));
    EXPECT_EQ(reg.self(), n);
    EXPECT_EQ(reg.node_count(), 2u); // the adopted node, plus the dummy

    // Already attached: the node is refused and stays the caller's to free, not leaked.
    auto* spare = new Registry::Node();
    EXPECT_FALSE(reg.attach(spare));
    delete spare;

    reg.detach();
}

// ===== the session guard =====================================================

TEST_F(Fixture, ASessionDetachesWhenItLeavesScope) {
    EXPECT_EQ(reg.active_count(), 0u);
    {
        auto s = reg.join();
        EXPECT_TRUE(s);
        EXPECT_NE(reg.self(), nullptr);
        EXPECT_EQ(reg.active_count(), 1u);
    }
    EXPECT_EQ(reg.self(), nullptr) << "the session did not detach on scope exit";
    EXPECT_EQ(reg.active_count(), 0u);
}

TEST_F(Fixture, ANestedJoinDoesNotDetachEarly) {
    // The inner session must report that the thread is attached while owing nothing, or the
    // outer scope loses its registration the moment an inner helper returns.
    auto outer = reg.join();
    ASSERT_TRUE(outer);
    {
        auto inner = reg.join();
        EXPECT_TRUE(inner) << "a nested join must still report the thread as attached";
    }
    EXPECT_NE(reg.self(), nullptr) << "the inner session detached the outer one";
    EXPECT_EQ(reg.active_count(), 1u);
}

TEST_F(Fixture, AMovedFromSessionDoesNotDetachTwice) {
    {
        auto a = reg.join();
        ASSERT_TRUE(a);
        auto b = std::move(a);
        EXPECT_TRUE(b);
        EXPECT_FALSE(a) << "the moved-from session still claims to be attached";
        EXPECT_NE(reg.self(), nullptr) << "moving detached the thread";
    }
    EXPECT_EQ(reg.active_count(), 0u);
    // A second detach from the moved-from session would enqueue the node twice, which shows
    // up as the free queue handing one node to two threads.
    EXPECT_EQ(reg.node_count(), kHint + 1);
}

TEST_F(Fixture, ResetDetachesEarlyAndIsIdempotent) {
    auto s = reg.join();
    ASSERT_TRUE(s);
    s.reset();
    EXPECT_FALSE(s);
    EXPECT_EQ(reg.self(), nullptr);
    s.reset(); // must be harmless
    EXPECT_EQ(reg.active_count(), 0u);
}

TEST(ThreadRegistryStandalone, TryJoinReportsFailureWithoutAllocating) {
    Registry reg{1};
    auto mine = reg.try_join();
    ASSERT_TRUE(mine);
    EXPECT_EQ(reg.node_count(), 2u); // the one node, plus the dummy

    std::thread other{[&] {
        auto s = reg.try_join();
        EXPECT_FALSE(s) << "try_join must not allocate when the queue is down to its dummy";
        EXPECT_EQ(reg.node_count(), 2u);
        EXPECT_EQ(reg.self(), nullptr);
    }};
    other.join();
}

// ===== traversal =============================================================

TEST_F(Fixture, ReduceVisitsExactlyTheAttachedThreads) {
    reg.attach();
    reg.self()->data.published.store(100);
    EXPECT_EQ(reg.active_count(), 1u);

    with_attached(reg, 3, [&] {
        EXPECT_EQ(reg.active_count(), 4u);
        // published values are 100 and 1..3
        const int sum = reg.reduce(0, [](int acc, const Payload& p) {
            return acc + p.published.load();
        });
        EXPECT_EQ(sum, 100 + 1 + 2 + 3);
    });

    EXPECT_EQ(reg.active_count(), 1u) << "a detached thread was still visited";
    reg.detach();
}

TEST_F(Fixture, AnyOfStopsAtTheFirstMatch) {
    // Hazard's collect depends on the scan being able to give up early.
    with_attached(reg, 4, [&] {
        int calls = 0;
        EXPECT_TRUE(reg.any_of([&](const Payload&) {
            ++calls;
            return true;
        }));
        EXPECT_EQ(calls, 1) << "any_of kept walking after it had its answer";

        calls = 0;
        EXPECT_FALSE(reg.any_of([&](const Payload&) {
            ++calls;
            return false;
        }));
        EXPECT_EQ(calls, 4) << "any_of stopped before exhausting the list";
    });
}

TEST_F(Fixture, AllOfStopsAtTheFirstFailure) {
    // Pool's epoch advance depends on this: one thread behind is decisive.
    with_attached(reg, 4, [&] {
        int calls = 0;
        EXPECT_FALSE(reg.all_of([&](const Payload&) {
            ++calls;
            return false;
        }));
        EXPECT_EQ(calls, 1) << "all_of kept walking after it had its answer";

        calls = 0;
        EXPECT_TRUE(reg.all_of([&](const Payload&) {
            ++calls;
            return true;
        }));
        EXPECT_EQ(calls, 4);
    });
}

TEST_F(Fixture, ForEachActiveCanMutateAndForEachNodeReachesDetachedOnes) {
    reg.attach();
    reg.self()->data.owned.push_back(7);
    reg.detach(); // detached, but the payload is still there

    int via_active = 0;
    reg.for_each_active([&](Payload& p) { via_active += static_cast<int>(p.owned.size()); });
    EXPECT_EQ(via_active, 0) << "a detached thread was visited as active";

    // Hazard's destructor uses for_each_node to drain retire lists a detached thread left.
    int via_all = 0;
    reg.for_each_node([&](Payload& p) {
        via_all += static_cast<int>(p.owned.size());
        p.owned.clear(); // the mutable form is what makes teardown possible
    });
    EXPECT_EQ(via_all, 1);

    via_all = 0;
    reg.for_each_node([&](Payload& p) { via_all += static_cast<int>(p.owned.size()); });
    EXPECT_EQ(via_all, 0) << "for_each_node did not hand out a mutable reference";
}

// ===== payload lifetime ======================================================

TEST_F(Fixture, PayloadSurvivesTheNodeBeingRecycled) {
    // Hazard keeps its retire list here. If recycling cleared the payload, a thread that
    // detached with retirements pending would drop them, leaking every segment in the list.
    //
    // The guarantee is that no payload is *lost*, not that a particular thread gets a
    // particular node back -- with a FIFO free list it usually will not. So the check is
    // over every node, which is exactly how ~Hazard drains the retire lists.
    reg.attach();
    reg.self()->data.owned = {1, 2, 3};
    reg.detach();

    int total = 0;
    reg.for_each_node([&](Payload& p) { total += static_cast<int>(p.owned.size()); });
    EXPECT_EQ(total, 3) << "the payload was discarded when the node was recycled";
}

// ===== instance isolation ====================================================

TEST(ThreadRegistryStandalone, InstancesOfTheSameTypeDoNotShareThreadLocalState) {
    // The per-thread association is an intrusive chain keyed by the registry, so holding a
    // node in one instance says nothing about the other.
    Registry a{1};
    Registry b{1};

    a.attach();
    b.attach();
    EXPECT_NE(a.self(), b.self());
    EXPECT_EQ(a.active_count(), 1u);
    EXPECT_EQ(b.active_count(), 1u);

    a.detach();
    EXPECT_EQ(a.self(), nullptr);
    EXPECT_NE(b.self(), nullptr) << "detaching from one registry detached from the other";
    b.detach();
}

TEST(ThreadRegistryStandalone, ManyInstancesCoexist) {
    // DTT_MAX_INSTANCES was 16 and threw past it; the chain has no such limit.
    std::vector<std::unique_ptr<Registry>> regs;
    for (int i = 0; i < 64; ++i) {
        regs.push_back(std::make_unique<Registry>(1));
        regs.back()->attach();
    }
    for (auto& r : regs) {
        EXPECT_NE(r->self(), nullptr);
        r->detach();
    }
}

TEST(ThreadRegistryStandalone, ARebuiltRegistryDoesNotInheritAStaleAssociation) {
    // The second registry can land in the storage the first just released.
    auto first = std::make_unique<Registry>(1);
    first->attach();
    first->detach(); // the precondition: detach before the registry dies
    first.reset();

    auto second = std::make_unique<Registry>(1);
    EXPECT_EQ(second->self(), nullptr) << "a stale association matched a new registry";
    second->attach();
    second->detach();
}

// ===== churn =================================================================

TEST(ThreadRegistryStandalone, SustainedChurnDoesNotGrowTheRegistry) {
    // This is the case that caught the exchange-based free-list pop: it emptied the list
    // while pushing the remainder back, so every attach landing in that window allocated,
    // and more nodes made the window longer. A registry hinted at 2 reached 309 nodes in
    // under a second and was still climbing.
    constexpr std::size_t kThreads = 8;
    constexpr int kIters = 4000;
    Registry reg{kThreads};

    std::atomic<int> completed{0};
    std::vector<std::thread> ts;
    for (std::size_t i = 0; i < kThreads; ++i)
        ts.emplace_back([&] {
            for (int k = 0; k < kIters; ++k) {
                reg.attach();
                reg.detach();
                ++completed;
            }
        });
    for (auto& t : ts) t.join();

    EXPECT_EQ(completed.load(), static_cast<int>(kThreads) * kIters);
    EXPECT_EQ(reg.active_count(), 0u);

    // Bounded, not exact. Threads churning past each other can briefly all be between
    // detach and attach, so a few extra nodes get allocated before the population settles;
    // measured, it plateaus a little above the hint and stays there. What must not happen is
    // monotonic growth -- the exchange-based pop this replaced climbed from a hint of 2 to
    // 309 nodes in under a second with no plateau at any thread count.
    EXPECT_GE(reg.node_count(), kThreads + 1);
    EXPECT_LE(reg.node_count(), 4 * kThreads)
        << "the registry keeps growing under churn: nodes are not being recycled";
}

TEST(ThreadRegistryStandalone, ScanningConcurrentlyWithChurnNeverMissesAStableThread) {
    // The safety property both sources rest on. One thread stays attached throughout while
    // the others churn; every scan must see it.
    constexpr std::size_t kThreads = 8;
    Registry reg{kThreads};

    reg.attach();
    reg.self()->data.published.store(0xABCD);

    std::atomic<bool> stop{false};
    std::vector<std::thread> ts;
    for (std::size_t i = 1; i < kThreads; ++i)
        ts.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                reg.attach();
                reg.detach();
            }
        });

    for (int i = 0; i < 20000; ++i) {
        const bool seen = reg.any_of([](const Payload& p) { return p.published.load() == 0xABCD; });
        ASSERT_TRUE(seen) << "a continuously attached thread was missed on scan " << i;
    }

    stop = true;
    for (auto& t : ts) t.join();
    reg.detach();
}

// ===== the optional attach-retry =============================================

TEST(ThreadRegistryStandalone, RetryOnAttachIsAvailableAndBehavesIdentically) {
    using Opt = meta::OptionsPack<ThreadRegistryOpt::retry_scan_on_attach>;
    ThreadRegistry<Payload, Opt> reg{2};

    reg.attach();
    reg.self()->data.published.store(3);
    EXPECT_EQ(reg.active_count(), 1u);

    std::atomic<bool> ready{false}, done{false};
    std::thread other{[&] {
        reg.attach();
        ready = true;
        while (!done.load()) {}
        reg.detach();
    }};
    while (!ready.load()) {}
    EXPECT_EQ(reg.active_count(), 2u);
    done = true;
    other.join();

    EXPECT_EQ(reg.active_count(), 1u);
    reg.detach();
}

} // namespace
