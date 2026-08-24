/**
 * @file SegmentLifecycleTest.cpp
 * @brief close / is_closed / reopen / next, exercised directly on each segment type.
 *
 * Restores the coverage lost with the old LinkedSegmentTest, and adds what that suite
 * did not check: that `reopen()` agrees with `segment_traits<>::recyclable`, and that a
 * reopened segment is genuinely empty.
 *
 * That last property is not academic. A pooled source hands segments back out, and SCQ's
 * reopen used to leave indices stranded in its data ring, so the *next* life of the
 * segment dequeued items belonging to the previous one -- 20004 items consumed against
 * 20000 produced. Nothing here tested reuse, so only a full MPMC run caught it.
 */
#include <gtest/gtest.h>

#include <algo/FAAArray.hpp>
#include <algo/HQ.hpp>
#include <algo/Mutex.hpp>
#include <algo/PRQ.hpp>
#include <algo/SCQ.hpp>
#include <algo/Vyukov.hpp>
#include <core/Segment.hpp>
#include <core/SegmentTraits.hpp>

namespace {

struct Data {
    uint64_t seq;
};
using Item = Data*;

template <typename S>
class SegmentLifecycle : public ::testing::Test {
protected:
    static constexpr std::size_t kCapacity = 64;

    /// Every segment here is built with mem::PtrHandle, so this is `S*`; named rather than
    /// spelled out so the link_next out-parameter does not hard-code the handle policy.
    using Handle = typename S::handle_type;

    S* make() { return S::create(kCapacity); }
    static void drop(S* s) { mem::SingleBlock<S>::destroy(s); }

    /// Fill until refused; returns how many were accepted.
    static std::size_t fill(S* s, std::vector<Data>& store) {
        std::size_t n = 0;
        while (n < store.size() && s->enqueue(&store[n])) ++n;
        return n;
    }
    static std::size_t drain(S* s) {
        Item out = nullptr;
        std::size_t n = 0;
        while (s->dequeue(out)) ++n;
        return n;
    }
};

using SegmentTypes = ::testing::Types<
    seg::Vyukov<Item>, seg::PRQ<Item>, seg::FAAArray<Item>, seg::HQ<Item>, seg::SCQ<Item>,
    seg::Mutex<Item>>;
TYPED_TEST_SUITE(SegmentLifecycle, SegmentTypes);

TYPED_TEST(SegmentLifecycle, StartsOpenWithNoSuccessor) {
    auto* s = this->make();
    EXPECT_FALSE(s->is_closed());
    EXPECT_EQ(s->next(), nullptr);
    this->drop(s);
}

TYPED_TEST(SegmentLifecycle, CloseRefusesEnqueueButPermitsDrain) {
    auto* s = this->make();
    std::vector<Data> store(8);
    for (std::size_t i = 0; i < store.size(); ++i) store[i] = {i + 1};

    const std::size_t placed = this->fill(s, store);
    ASSERT_GT(placed, 0u);

    s->close();
    EXPECT_TRUE(s->is_closed());
    EXPECT_FALSE(s->enqueue(&store[0])) << "a closed segment must refuse new items";

    // Closing must not strand what is already inside.
    EXPECT_EQ(this->drain(s), placed);
    this->drop(s);
}

TYPED_TEST(SegmentLifecycle, FillingToCapacityClosesTheSegment) {
    auto* s = this->make();
    std::vector<Data> store(TestFixture::kCapacity * 4);
    for (std::size_t i = 0; i < store.size(); ++i) store[i] = {i + 1};

    const std::size_t placed = this->fill(s, store);
    EXPECT_GT(placed, 0u);
    EXPECT_LE(placed, TestFixture::kCapacity * 2) << "accepted far beyond its capacity";
    // A segment that refuses an item is the proxy's signal to link a successor, so it
    // has to report itself closed rather than merely returning false.
    EXPECT_TRUE(s->is_closed());
    this->drop(s);
}

TYPED_TEST(SegmentLifecycle, LinkNextSucceedsExactlyOnce) {
    auto* a = this->make();
    auto* b = this->make();
    auto* c = this->make();

    // The out-parameter carries the CAS's `expected`, so it is meaningful on *failure*: the
    // loser is handed whoever won and needs no second read of next(). LinkedProxy would
    // otherwise pay a contended acquire load on exactly the path where producers are racing.
    // It must be passed nil -- the segment asserts that -- and a winner leaves it untouched.
    typename TestFixture::Handle installed{};

    EXPECT_TRUE(a->link_next(b, installed));
    EXPECT_EQ(installed, nullptr) << "a successful link leaves the out-parameter alone";
    EXPECT_EQ(a->next(), b);

    installed = {};
    EXPECT_FALSE(a->link_next(c, installed)) << "a successor may only be installed once";
    EXPECT_EQ(installed, b) << "the loser should be handed the winner, not its own candidate";
    EXPECT_EQ(a->next(), b);

    this->drop(a);
    this->drop(b);
    this->drop(c);
}

TYPED_TEST(SegmentLifecycle, ReopenAgreesWithTheRecyclableTrait) {
    using Traits = core::segment_traits<TypeParam>;
    auto* s = this->make();
    std::vector<Data> store(8);
    for (std::size_t i = 0; i < store.size(); ++i) store[i] = {i + 1};

    this->fill(s, store);
    this->drain(s);
    EXPECT_EQ(s->reopen(), Traits::recyclable)
        << "reopen() and segment_traits<>::recyclable disagree; a pooled source trusts "
           "the trait, so they must not";
    this->drop(s);
}

TYPED_TEST(SegmentLifecycle, ReopenedSegmentIsEmptyAndReusable) {
    using Traits = core::segment_traits<TypeParam>;
    if constexpr (!Traits::recyclable) {
        GTEST_SKIP() << "not recyclable by declaration";
    } else {
        auto* s = this->make();
        std::vector<Data> store(TestFixture::kCapacity * 2);
        for (std::size_t i = 0; i < store.size(); ++i) store[i] = {i + 1};

        // Three lives, so a stale-state bug has somewhere to accumulate.
        for (int life = 0; life < 3; ++life) {
            const std::size_t placed = this->fill(s, store);
            ASSERT_GT(placed, 0u) << "life " << life << ": accepted nothing after reopen";
            EXPECT_EQ(this->drain(s), placed) << "life " << life;

            Item leftover = nullptr;
            EXPECT_FALSE(s->dequeue(leftover))
                << "life " << life << ": drained segment still yields items";

            ASSERT_TRUE(s->reopen()) << "life " << life;
            EXPECT_FALSE(s->is_closed()) << "life " << life << ": still closed after reopen";

            // Emptiness is checked with size(), not with a probing dequeue. For FAAArray
            // and HQ a dequeue on an open empty segment is *destructive*: it fetch-adds
            // the head and exchanges `consumed` into every cell it passes, so probing here
            // would consume the whole segment and the next life would accept nothing. That
            // is inherent to a write-once array, not something reopen introduced -- it is
            // why HQ has a slow path at all. Survivors are still caught, by the
            // fill/drain count equality at the top of the next iteration.
            EXPECT_EQ(s->size(), 0u)
                << "life " << life << ": reopened segment is not empty -- items from the "
                   "previous life survived, which duplicates them under a pooled source";
        }
        this->drop(s);
    }
}

TYPED_TEST(SegmentLifecycle, ReopenClearsTheSuccessor) {
    using Traits = core::segment_traits<TypeParam>;
    if constexpr (!Traits::recyclable) {
        GTEST_SKIP() << "not recyclable by declaration";
    } else {
        auto* a = this->make();
        auto* b = this->make();
        typename TestFixture::Handle installed{};
        ASSERT_TRUE(a->link_next(b, installed));
        ASSERT_TRUE(a->reopen());
        // A recycled segment carrying a stale successor would splice a dead node back
        // into the list.
        EXPECT_EQ(a->next(), nullptr);
        this->drop(a);
        this->drop(b);
    }
}


// ---------------------------------------------------------------------------
// Linked-only algorithms cannot be spelled without a linkage strategy
// ---------------------------------------------------------------------------
//
// PRQ, FAAArray and HQ are unsound as standalone queues: FAAArray and HQ write each cell
// once and never reset their indices, so they accept exactly one fill/drain cycle; PRQ
// depends on a proxy linking a successor when it closes itself. Each is constrained on
// linkage::Linked, so the broken configuration is not a nameable type rather than merely
// a documented mistake.
//
// `requires { typename X; }` is a substitution context, so an unsatisfied constraint on
// the class template is detectable here instead of being a hard error.

template <typename T>
concept PrqStandaloneNameable =
    requires { typename algo::PRQ<T, meta::EmptyOptions, linkage::None>; };
template <typename T>
concept FaaStandaloneNameable =
    requires { typename algo::FAAArray<T, meta::EmptyOptions, linkage::None>; };
template <typename T>
concept HqStandaloneNameable =
    requires { typename algo::HQ<T, meta::EmptyOptions, linkage::None>; };

static_assert(!PrqStandaloneNameable<Item>, "PRQ must not be instantiable without linkage");
static_assert(!FaaStandaloneNameable<Item>, "FAAArray must not be instantiable without linkage");
static_assert(!HqStandaloneNameable<Item>, "HQ must not be instantiable without linkage");

// The linked forms remain perfectly nameable.
template <typename T>
concept PrqLinkedNameable =
    requires { typename algo::PRQ<T, meta::EmptyOptions, linkage::Node<mem::PtrHandle>>; };
static_assert(PrqLinkedNameable<Item>, "the linked form must still exist");

// Vyukov and SCQ are sound both ways, so both spellings must remain available.
template <typename T>
concept VyukovStandaloneNameable = requires { typename queue::Vyukov<T>; };
static_assert(VyukovStandaloneNameable<Item>,
              "Vyukov is sound standalone and must keep its standalone form");

TEST(LinkedOnly, TheConstraintIsWhatForbidsIt) {
    // A runtime marker so the guarantee shows up in the test report rather than only as a
    // compile-time property nobody sees.
    EXPECT_FALSE(PrqStandaloneNameable<Item>);
    EXPECT_FALSE(FaaStandaloneNameable<Item>);
    EXPECT_FALSE(HqStandaloneNameable<Item>);
    EXPECT_TRUE(PrqLinkedNameable<Item>);
    EXPECT_TRUE(VyukovStandaloneNameable<Item>);
}

} // namespace
