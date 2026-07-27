/**
 * @file TaggingTest.cpp
 * @brief The cell tagging policies that replaced three hand-rolled sentinel schemes.
 *
 * The three schemes these unify used the word "reserved" to mean different things --
 * for FAAArray it included the empty cell, for PRQ it excluded it -- so the properties
 * worth pinning down are that the four states are mutually exclusive and exhaustive,
 * and that `can_store_null` tells the truth.
 */
#include <gtest/gtest.h>

#include <algo/HQ.hpp>
#include <algo/Vyukov.hpp>
#include <cell/Tagging.hpp>

#include <atomic>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace {

struct Data {
    uint64_t seq;
};
using Item = Data*;

template <typename Tag>
class Tagging : public ::testing::Test {};

using Tags = ::testing::Types<cell::MsbTag<Item>, cell::LowTag<Item>>;
TYPED_TEST_SUITE(Tagging, Tags);

TYPED_TEST(Tagging, PayloadRoundTrips) {
    Data d{7};
    const auto w = TypeParam::encode(&d);
    EXPECT_EQ(TypeParam::decode(w), &d);
    EXPECT_TRUE(TypeParam::is_payload(w));
}

TYPED_TEST(Tagging, SentinelsAreDistinctAndNotPayloads) {
    EXPECT_NE(TypeParam::empty(), TypeParam::consumed());
    EXPECT_TRUE(TypeParam::is_empty(TypeParam::empty()));
    EXPECT_TRUE(TypeParam::is_consumed(TypeParam::consumed()));
    EXPECT_FALSE(TypeParam::is_payload(TypeParam::empty()));
    EXPECT_FALSE(TypeParam::is_payload(TypeParam::consumed()));
}

TYPED_TEST(Tagging, StatesAreMutuallyExclusive) {
    Data d{1};
    for (auto w : {TypeParam::empty(), TypeParam::consumed(), TypeParam::encode(&d)}) {
        const int matches = int(TypeParam::is_empty(w)) + int(TypeParam::is_consumed(w)) +
                            int(TypeParam::is_payload(w));
        EXPECT_EQ(matches, 1) << "word " << w << " matches " << matches << " states";
    }
}

TYPED_TEST(Tagging, CanStoreNullIsHonest) {
    const auto w = TypeParam::encode(nullptr);
    // The distinction that used to live only in a debug assert on HQ's enqueue path.
    EXPECT_EQ(TypeParam::is_payload(w), TypeParam::can_store_null);
    if constexpr (TypeParam::can_store_null) {
        EXPECT_EQ(TypeParam::decode(w), nullptr);
    }
}

TEST(MsbTagClaims, TokensAreUniquePerThreadAndNotSentinels) {
    constexpr int kThreads = 16;
    std::set<uintptr_t> tokens;
    std::mutex mu;
    std::vector<std::thread> ts;
    for (int i = 0; i < kThreads; ++i)
        ts.emplace_back([&] {
            const auto tok = cell::MsbTag<Item>::claim();
            std::lock_guard<std::mutex> g(mu);
            tokens.insert(tok);
        });
    for (auto& t : ts) t.join();

    EXPECT_EQ(tokens.size(), static_cast<size_t>(kThreads)) << "claim tokens collided";
    for (auto tok : tokens) {
        EXPECT_TRUE(cell::MsbTag<Item>::is_claim(tok));
        EXPECT_FALSE(cell::MsbTag<Item>::is_payload(tok));
        EXPECT_NE(tok, cell::MsbTag<Item>::empty());
        EXPECT_NE(tok, cell::MsbTag<Item>::consumed());
    }
}

TEST(MsbTagClaims, ClaimIsStableWithinAThread) {
    EXPECT_EQ(cell::MsbTag<Item>::claim(), cell::MsbTag<Item>::claim());
}

/// The trait must match what the queue built on that policy actually does.
TEST(TaggingIntegration, NullPayloadRoundTripsWhenTheTraitSaysItCan) {
    using Q = queue::Vyukov<Item>;
    static_assert(core::segment_traits<seg::Vyukov<Item>>::can_store_null);
    auto* q = Q::create(8);
    ASSERT_TRUE(q->enqueue(nullptr));
    Item out = reinterpret_cast<Item>(0x1);
    ASSERT_TRUE(q->dequeue(out));
    EXPECT_EQ(out, nullptr);
    mem::SingleBlock<Q>::destroy(q);
}

TEST(TaggingIntegration, HqDeclaresItCannotStoreNull) {
    // HQ uses LowTag, where 0 is the empty sentinel, so a null item is unrepresentable.
    // Enqueuing one asserts in a debug build, so the trait is the contract to check.
    static_assert(!core::segment_traits<seg::HQ<Item>>::can_store_null);
    EXPECT_FALSE(cell::LowTag<Item>::can_store_null);
    EXPECT_TRUE(cell::LowTag<Item>::is_empty(cell::LowTag<Item>::encode(nullptr)));
}

} // namespace
