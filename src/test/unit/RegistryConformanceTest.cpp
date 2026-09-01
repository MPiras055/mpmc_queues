/**
 * @file RegistryConformanceTest.cpp
 * @brief Every registered implementation satisfies its contract, and behaves sequentially.
 *
 * Replaces the hand-maintained ::testing::Types lists, which had drifted far enough that
 * six entries in BoundedProxyTest named a namespace that no longer held those types and
 * one type that no longer existed at all. The list now comes from the registry, so a new
 * implementation is covered the moment it is registered.
 */
#include <gtest/gtest.h>
#include <registry/Registry.hpp>

#include <core/Proxy.hpp>
#include <core/Queue.hpp>
#include <util/bit.hpp>
#include <vector>

namespace {

struct Data {
    uint64_t seq;
    uint64_t tid;
};
using Item = Data*;

// -----------------------------------------------------------------------------
// Contract conformance, entirely at compile time.
// -----------------------------------------------------------------------------
template <typename... Es>
constexpr bool all_model_queue(meta::TypeList<Es...>) {
    return (core::Queue<typename Es::type, Item> && ...);
}
template <typename... Es>
constexpr bool all_model_proxy(meta::TypeList<Es...>) {
    return (core::Proxy<typename Es::type, Item> && ...);
}

static_assert(all_model_queue(registry::Standalone<Item>{}),
              "a standalone registry entry does not model core::Queue");
static_assert(all_model_proxy(registry::Linked<Item>{}),
              "a linked registry entry does not model core::Proxy");

// A pooled source may only hold segments that can be reopened; verify the trait that
// enforces it is actually set the way each algorithm behaves.
static_assert(core::segment_traits<seg::FAAArray<Item>>::recyclable);
static_assert(core::segment_traits<seg::HQ<Item>>::recyclable);
static_assert(core::segment_traits<seg::Vyukov<Item>>::recyclable);
static_assert(core::segment_traits<seg::PRQ<Item>>::needs_close_hint,
              "PRQ needs the close hint; without it the bounded proxies livelock");
static_assert(core::segment_traits<seg::SCQ<Item>>::needs_dequeue_prepare);

// -----------------------------------------------------------------------------
// Sequential behaviour, over every registered implementation.
// -----------------------------------------------------------------------------
template <typename Q>
class QueueBehaviour : public ::testing::Test {
protected:
    static constexpr size_t kCapacity = 64;
    static constexpr size_t kThreads = 8;
    registry::Instance<Q> inst{kCapacity};
    Q& q() { return inst.get(); }
    /// Declared after `inst` so it is released before the queue is destroyed.
    decltype(registry::Instance<Q>::session(std::declval<Q&>())) joined{};

    void SetUp() override { joined = registry::Instance<Q>::session(q()); }
};

using AllTypes = registry::AsTypes<registry::All<Item>>::apply<::testing::Types>;
TYPED_TEST_SUITE(QueueBehaviour, AllTypes, registry::TestNames<registry::All<Item>>);

/**
 * @brief Every registered queue answers a capacity request the same way: rounded up to a power
 *        of two, and never below what was asked.
 *
 * The algorithms used to disagree three ways -- LFring, SCQ, PSCQ and the two Vyukov comparators
 * always rounded; Vyukov and PRQ rounded only when asked; FAAArray, HQ and Mutex never did. So
 * "capacity 1000" meant 1024 cells in one implementation and 1000 in another, and a benchmark
 * across them was comparing geometries rather than algorithms.
 *
 * The sizes here are deliberately *not* powers of two. Every other test in the tree uses 64 or
 * 8, which satisfies this property without exercising any rounding at all -- so with pow2 inputs
 * this test would pass against the old, inconsistent code.
 */
TYPED_TEST(QueueBehaviour, CapacityRoundsUpToAPowerOfTwo) {
    for (std::size_t requested : {std::size_t{3}, std::size_t{100}, std::size_t{1000}}) {
        registry::Instance<TypeParam> inst{requested};
        [[maybe_unused]] auto joined = registry::Instance<TypeParam>::session(inst.get());
        const std::size_t cap = inst.get().capacity();

        EXPECT_TRUE(bit::is_pow2(cap))
            << "requested " << requested << ", got " << cap << " which is not a power of two";
        EXPECT_GE(cap, requested) << "rounded a request *down*";
    }
}

TYPED_TEST(QueueBehaviour, EmptyOnCreation) {
    Item out = nullptr;
    EXPECT_FALSE(this->q().try_dequeue(out));
    EXPECT_EQ(out, nullptr) << "a failed dequeue must not write to the out parameter";
    EXPECT_EQ(this->q().size(), 0u);
}

TYPED_TEST(QueueBehaviour, FifoOrderSingleThreaded) {
    std::vector<Data> data(32);
    for (size_t i = 0; i < data.size(); ++i) data[i] = {i + 1, 0};

    size_t pushed = 0;
    for (auto& d : data)
        if (this->q().try_enqueue(&d)) ++pushed;
    ASSERT_GT(pushed, 0u);

    Item out = nullptr;
    for (size_t i = 0; i < pushed; ++i) {
        ASSERT_TRUE(this->q().try_dequeue(out)) << "lost item " << i << " of " << pushed;
        EXPECT_EQ(out->seq, i + 1) << "out of order at " << i;
    }
    EXPECT_FALSE(this->q().try_dequeue(out));
}

TYPED_TEST(QueueBehaviour, RefusesBeyondCapacityThenDrainsExactly) {
    std::vector<Data> data(4096);
    for (size_t i = 0; i < data.size(); ++i) data[i] = {i + 1, 0};

    size_t pushed = 0;
    for (auto& d : data) {
        if (!this->q().try_enqueue(&d)) break; // bounded: refuses. unbounded: never trips.
        ++pushed;
    }
    ASSERT_GT(pushed, 0u);

    Item out = nullptr;
    size_t popped = 0;
    while (this->q().try_dequeue(out)) ++popped;
    EXPECT_EQ(popped, pushed) << "drained a different number of items than were accepted";
}

TYPED_TEST(QueueBehaviour, SizeTracksOccupancy) {
    std::vector<Data> data(16);
    for (size_t i = 0; i < data.size(); ++i) data[i] = {i + 1, 0};

    size_t pushed = 0;
    for (auto& d : data)
        if (this->q().try_enqueue(&d)) ++pushed;
    EXPECT_EQ(this->q().size(), pushed);

    Item out = nullptr;
    while (this->q().try_dequeue(out)) {}
    EXPECT_EQ(this->q().size(), 0u);
}

} // namespace
