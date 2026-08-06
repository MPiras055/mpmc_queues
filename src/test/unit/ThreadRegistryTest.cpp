/**
 * @file ThreadRegistryTest.cpp
 * @brief The lock-free thread registry that both reclamation sources are built on.
 *
 * util::threading::ThreadRegistry replaces DynamicThreadTicket, and the two things it adds
 * over a bitset of tickets are the ones worth pinning down: the payload lives in the node,
 * and a detached thread stops being visited. Everything below either ports a case from
 * DynamicThreadTicketTest -- attach/detach, exhaustion, reuse, TLS stability, instance
 * independence, churn -- or covers what is new.
 *
 * The property the reclamation sources depend on, stated once:
 *
 *     for_each_active visits every thread that stays attached for the whole walk,
 *     and a detached thread's node is returned for reuse exactly once.
 *
 * Miss the first and a hazard scan frees an object somebody is reading; miss the second
 * and the registry quietly shrinks until no thread can attach. Both are checked here,
 * the first by ScanningConcurrentlyWithChurnNeverMissesAStableThread and the second by
 * NoNodeIsLostOverManyAttachDetachCycles -- which is the test that caught the physical
 * splice this registry used to do, and no longer does.
 */
#include <gtest/gtest.h>

#include <util/threading/ThreadRegistry.hpp>

#include <atomic>
#include <set>
#include <thread>
#include <vector>

using util::threading::ThreadRegistry;
using util::threading::ThreadRegistryOpt;

namespace {

/// A payload with both shapes the real sources use: a published word and an owned list.
struct Payload {
    std::atomic<int> published{0};
    std::vector<int> owned;
};

using Registry = ThreadRegistry<Payload>;

constexpr std::size_t kMaxThreads = 16;

struct Fixture : ::testing::Test {
    Registry reg{kMaxThreads};

    /// Slots of every currently attached thread.
    std::set<uint32_t> active_slots() {
        std::set<uint32_t> out;
        reg.for_each_active([&](Payload& p) { out.insert(static_cast<uint32_t>(p.published.load())); });
        return out;
    }
};

// ===== attach / detach basics ================================================

TEST_F(Fixture, AttachIsIdempotentAndDetachIsIdempotent) {
    EXPECT_EQ(reg.self(), nullptr) << "a thread starts unattached";

    ASSERT_TRUE(reg.attach());
    auto* first = reg.self();
    ASSERT_NE(first, nullptr);

    ASSERT_TRUE(reg.attach());
    EXPECT_EQ(reg.self(), first) << "a second attach handed out a different node";

    reg.detach();
    EXPECT_EQ(reg.self(), nullptr);
    reg.detach(); // must not fault or corrupt the list
    EXPECT_EQ(reg.active_count(), 0u);
}

TEST_F(Fixture, SlotIsStableWhileAttached) {
    ASSERT_TRUE(reg.attach());
    const uint32_t slot = reg.self()->slot;
    for (int i = 0; i < 100; ++i) ASSERT_EQ(reg.self()->slot, slot);
    EXPECT_LT(slot, kMaxThreads) << "the slot must index a proxy's per-thread array";
    reg.detach();
}

TEST_F(Fixture, DetachedNodeIsHandedOutAgain) {
    // A registry that does not recycle looks fine until it runs out; this is the check
    // that the splice actually returns the node rather than merely hiding it.
    ASSERT_TRUE(reg.attach());
    const uint32_t slot = reg.self()->slot;
    reg.detach();

    ASSERT_TRUE(reg.attach());
    EXPECT_EQ(reg.self()->slot, slot);
    reg.detach();
}

// ===== the traversal =========================================================

TEST_F(Fixture, ForEachActiveVisitsExactlyTheAttachedThreads) {
    EXPECT_EQ(reg.active_count(), 0u);

    ASSERT_TRUE(reg.attach());
    reg.self()->data.published.store(7);
    EXPECT_EQ(reg.active_count(), 1u);
    EXPECT_EQ(active_slots(), (std::set<uint32_t>{7}));

    std::atomic<bool> ready{false}, done{false};
    std::thread other{[&] {
        ASSERT_TRUE(reg.attach());
        reg.self()->data.published.store(9);
        ready = true;
        while (!done.load()) {}
        reg.detach();
    }};
    while (!ready.load()) {}

    EXPECT_EQ(reg.active_count(), 2u);
    EXPECT_EQ(active_slots(), (std::set<uint32_t>{7, 9}));

    done = true;
    other.join();

    EXPECT_EQ(reg.active_count(), 1u) << "a detached thread was still visited";
    EXPECT_EQ(active_slots(), (std::set<uint32_t>{7}));
    reg.detach();
}

TEST_F(Fixture, AFunctorReturningFalseStopsTheWalk) {
    // Hazard::is_protected depends on this: it stops at the first thread protecting the
    // pointer rather than walking the rest of the list.
    std::atomic<bool> ready{false}, done{false};
    std::vector<std::thread> ts;
    for (int i = 0; i < 4; ++i)
        ts.emplace_back([&] {
            ASSERT_TRUE(reg.attach());
            while (!done.load()) {}
            reg.detach();
        });
    while (reg.active_count() < 4) {}
    (void)ready;

    int visited = 0;
    reg.for_each_active([&](Payload&) {
        ++visited;
        return false;
    });
    EXPECT_EQ(visited, 1);

    done = true;
    for (auto& t : ts) t.join();
}

TEST_F(Fixture, EveryNodeReturnsToCirculationAfterAFullRound) {
    // Detach marks the node inactive and pushes it to the free list; it keeps its place in
    // the active list. Exhaustion is the sharpest test that it really came back.
    std::vector<std::thread> ts;
    for (std::size_t i = 0; i < kMaxThreads; ++i)
        ts.emplace_back([&] {
            ASSERT_TRUE(reg.attach());
            reg.detach();
        });
    for (auto& t : ts) t.join();

    EXPECT_EQ(reg.active_count(), 0u);

    // Every node must be available again, which it is not if any splice leaked one.
    std::vector<std::thread> holders;
    std::atomic<int> attached{0};
    std::atomic<bool> release{false};
    for (std::size_t i = 0; i < kMaxThreads; ++i)
        holders.emplace_back([&] {
            if (reg.attach()) {
                ++attached;
                while (!release.load()) {}
                reg.detach();
            }
        });
    while (attached.load() < static_cast<int>(kMaxThreads)) {}
    EXPECT_EQ(attached.load(), static_cast<int>(kMaxThreads));
    release = true;
    for (auto& t : holders) t.join();
}

// ===== capacity ==============================================================

TEST_F(Fixture, ExhaustionIsReportedAndRecovers) {
    std::vector<std::thread> ts;
    std::atomic<int> ok{0};
    std::atomic<bool> release{false};
    for (std::size_t i = 0; i < kMaxThreads; ++i)
        ts.emplace_back([&] {
            if (reg.attach()) {
                ++ok;
                while (!release.load()) {}
                reg.detach();
            }
        });
    while (ok.load() < static_cast<int>(kMaxThreads)) {}

    EXPECT_EQ(ok.load(), static_cast<int>(kMaxThreads));
    EXPECT_FALSE(reg.attach()) << "attached past max_threads";

    release = true;
    for (auto& t : ts) t.join();

    EXPECT_TRUE(reg.attach()) << "never recovered after every thread detached";
    reg.detach();
}

TEST(ThreadRegistryStandalone, ThreadCountIsRuntimeNotCompileTime) {
    // The point of the replacement: DTT_MAX_BITS capped this at 1024 at compile time.
    Registry big{4096};
    EXPECT_EQ(big.max_threads(), 4096u);
    ASSERT_TRUE(big.attach());
    EXPECT_LT(big.self()->slot, 4096u);
    big.detach();
}

// ===== payload ===============================================================

TEST_F(Fixture, PayloadSurvivesRecyclingOfItsNode) {
    // Hazard keeps its retire list here. If recycling cleared the payload, a thread that
    // detached with retirements pending would drop them, leaking every segment in it.
    ASSERT_TRUE(reg.attach());
    const uint32_t slot = reg.self()->slot;
    reg.self()->data.owned = {1, 2, 3};
    reg.detach();

    ASSERT_TRUE(reg.attach());
    ASSERT_EQ(reg.self()->slot, slot) << "did not get the same node back";
    EXPECT_EQ(reg.self()->data.owned, (std::vector<int>{1, 2, 3}))
        << "the inherited payload was discarded";
    reg.detach();
}

TEST_F(Fixture, ForEachNodeReachesDetachedNodesToo) {
    // Hazard's destructor uses this to drain retire lists left behind by threads that
    // detached; walking only the active list would miss them.
    ASSERT_TRUE(reg.attach());
    reg.self()->data.owned.push_back(5);
    reg.detach();

    int found = 0;
    reg.for_each_node([&](Payload& p) { found += static_cast<int>(p.owned.size()); });
    EXPECT_EQ(found, 1);
}

// ===== instance and thread isolation =========================================

TEST(ThreadRegistryStandalone, InstancesOfTheSameTypeDoNotShareTls) {
    // DynamicThreadTicket needed a compile-time DTT_MAX_INSTANCES for this; the
    // association is keyed by a monotonic id instead, so there is no cap.
    Registry a{8};
    Registry b{8};

    ASSERT_TRUE(a.attach());
    ASSERT_TRUE(b.attach());
    EXPECT_NE(a.self(), b.self());
    EXPECT_EQ(a.active_count(), 1u);
    EXPECT_EQ(b.active_count(), 1u);

    a.detach();
    EXPECT_EQ(a.self(), nullptr);
    EXPECT_NE(b.self(), nullptr) << "detaching from one registry detached from the other";
    b.detach();
}

TEST(ThreadRegistryStandalone, ManyInstancesCoexist) {
    std::vector<std::unique_ptr<Registry>> regs;
    for (int i = 0; i < 64; ++i) { // DTT_MAX_INSTANCES was 16, and threw past it
        regs.push_back(std::make_unique<Registry>(4));
        ASSERT_TRUE(regs.back()->attach());
    }
    for (auto& r : regs) {
        EXPECT_NE(r->self(), nullptr);
        r->detach();
    }
}

TEST(ThreadRegistryStandalone, ARebuiltRegistryDoesNotInheritStaleAssociations) {
    // Keying the per-thread association by address rather than by a monotonic id would
    // fail here: the second registry can land in the storage the first just released.
    auto first = std::make_unique<Registry>(4);
    ASSERT_TRUE(first->attach());
    first.reset();

    auto second = std::make_unique<Registry>(4);
    EXPECT_EQ(second->self(), nullptr) << "a stale association matched a new registry";
    ASSERT_TRUE(second->attach());
    second->detach();
}

TEST(ThreadRegistryStandalone, EachThreadGetsADistinctSlot) {
    Registry reg{kMaxThreads};
    std::mutex m;
    std::set<uint32_t> slots;
    std::atomic<int> attached{0};
    std::atomic<bool> release{false};

    std::vector<std::thread> ts;
    for (std::size_t i = 0; i < kMaxThreads; ++i)
        ts.emplace_back([&] {
            ASSERT_TRUE(reg.attach());
            {
                std::lock_guard lk{m};
                slots.insert(reg.self()->slot);
            }
            ++attached;
            while (!release.load()) {}
            reg.detach();
        });
    while (attached.load() < static_cast<int>(kMaxThreads)) {}
    release = true;
    for (auto& t : ts) t.join();

    EXPECT_EQ(slots.size(), kMaxThreads) << "two threads were given the same slot";
}

// ===== churn =================================================================

TEST(ThreadRegistryStandalone, NoNodeIsLostOverManyAttachDetachCycles) {
    // A leaked node shows up as a registry that quietly shrinks until nothing can attach,
    // which in a queue looks like threads silently failing to register.
    constexpr std::size_t kThreads = 8;
    constexpr int kIters = 2000;
    Registry reg{kThreads};

    std::atomic<int> completed{0};
    std::vector<std::thread> ts;
    for (std::size_t i = 0; i < kThreads; ++i)
        ts.emplace_back([&] {
            for (int k = 0; k < kIters; ++k) {
                ASSERT_TRUE(reg.attach()) << "registry shrank at iteration " << k;
                ASSERT_LT(reg.self()->slot, kThreads);
                reg.detach();
                ++completed;
            }
        });
    for (auto& t : ts) t.join();

    EXPECT_EQ(completed.load(), static_cast<int>(kThreads) * kIters);
    EXPECT_EQ(reg.active_count(), 0u);

    // Full capacity must still be reachable.
    std::atomic<int> attached{0};
    std::atomic<bool> release{false};
    std::vector<std::thread> holders;
    for (std::size_t i = 0; i < kThreads; ++i)
        holders.emplace_back([&] {
            if (reg.attach()) {
                ++attached;
                while (!release.load()) {}
                reg.detach();
            }
        });
    while (attached.load() < static_cast<int>(kThreads)) {}
    EXPECT_EQ(attached.load(), static_cast<int>(kThreads));
    release = true;
    for (auto& t : holders) t.join();
}

TEST(ThreadRegistryStandalone, ScanningConcurrentlyWithChurnNeverMissesAStableThread) {
    // The safety property both sources rest on. One thread stays attached throughout while
    // others churn; every scan must see it.
    constexpr std::size_t kThreads = 8;
    Registry reg{kThreads};

    ASSERT_TRUE(reg.attach());
    reg.self()->data.published.store(0xABCD);

    std::atomic<bool> stop{false};
    std::vector<std::thread> ts;
    for (std::size_t i = 1; i < kThreads; ++i)
        ts.emplace_back([&] {
            while (!stop.load()) {
                if (reg.attach()) reg.detach();
            }
        });

    for (int i = 0; i < 20000; ++i) {
        bool seen = false;
        reg.for_each_active([&](Payload& p) {
            if (p.published.load() == 0xABCD) seen = true;
        });
        ASSERT_TRUE(seen) << "a continuously attached thread was missed on scan " << i;
    }

    stop = true;
    for (auto& t : ts) t.join();
    reg.detach();
}

// ===== the optional attach-retry =============================================

TEST(ThreadRegistryStandalone, RetryOnAttachIsAvailableAndBehavesIdentically) {
    using Opt = meta::OptionsPack<ThreadRegistryOpt::retry_scan_on_attach>;
    ThreadRegistry<Payload, Opt> reg{4};

    ASSERT_TRUE(reg.attach());
    reg.self()->data.published.store(3);
    EXPECT_EQ(reg.active_count(), 1u);

    std::atomic<bool> ready{false}, done{false};
    std::thread other{[&] {
        ASSERT_TRUE(reg.attach());
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
