/**
 * @file MemoryLayoutTest.cpp
 * @brief The single-block allocator lays segments out correctly, and provably so.
 *
 * This is the regression guard for a bug that shipped three times: the header offset was
 * computed as `(n + a - 1) & (~a - 1)` instead of `& ~(a - 1)`. That does not align up --
 * it clears bit 0 and bit log2(a). On the real types it put PRQ's cell array two bytes
 * *inside* the object (offset 638 against sizeof 640, overlapping head/tail) and left
 * FAAArray's misaligned by 6.
 *
 * mem::align_up is now the only implementation, and mem::Plan::valid() is checked at
 * compile time inside SingleBlock::create, so the class of bug is unrepresentable rather
 * than merely fixed. These tests assert the properties directly as well.
 */
#include <gtest/gtest.h>

#include <algo/FAAArray.hpp>
#include <algo/HQ.hpp>
#include <algo/PRQ.hpp>
#include <algo/SCQ.hpp>
#include <algo/Vyukov.hpp>
#include <mem/Align.hpp>
#include <mem/SingleBlock.hpp>

namespace {

using Item = int*;

TEST(Align, RoundsUpAndIsIdempotent) {
    EXPECT_EQ(mem::align_up(0, 8), 0u);
    EXPECT_EQ(mem::align_up(1, 8), 8u);
    EXPECT_EQ(mem::align_up(8, 8), 8u);
    EXPECT_EQ(mem::align_up(384, 128), 384u);
    EXPECT_EQ(mem::align_up(385, 128), 512u);
    for (std::size_t a : {8u, 16u, 64u, 128u})
        for (std::size_t n = 0; n < 300; ++n) {
            const auto r = mem::align_up(n, a);
            EXPECT_GE(r, n);
            EXPECT_EQ(r % a, 0u);
            EXPECT_LT(r - n, a);
        }
}

TEST(Align, RejectsTheHistoricalMistake) {
    // The shipped-and-wrong form, kept here so the difference stays visible.
    const auto buggy = [](std::size_t n, std::size_t a) { return (n + a - 1) & (~a - 1); };
    EXPECT_EQ(mem::align_up(640, 128), 640u);
    EXPECT_EQ(buggy(640, 128), 638u) << "the old form under-shoots the header";
    EXPECT_LT(buggy(640, 128), 640u) << "which is exactly how the buffer overlapped it";
}

/// Every layout must place its regions after the header, in order, inside the block.
template <typename Q>
void CheckLayout(const char* name) {
    for (std::size_t n : {2u, 8u, 64u, 1024u}) {
        const auto p = Q::plan(n);
        EXPECT_TRUE(p.valid(sizeof(Q))) << name << " n=" << n;
        std::size_t prev_end = sizeof(Q);
        for (const auto& r : p.regions) {
            EXPECT_GE(r.offset, prev_end) << name << ": region overlaps header/previous";
            EXPECT_LE(r.end(), p.total) << name << ": region runs past the block";
            prev_end = r.end();
        }
        EXPECT_EQ(p.total % p.block_align, 0u)
            << name << ": aligned_alloc requires size to be a multiple of alignment";
    }
}

TEST(SingleBlockLayout, RegionsAreWellFormed) {
    CheckLayout<queue::Vyukov<Item>>("Vyukov");
    // PRQ/FAAArray/HQ are linked-only, so their layouts are checked in the linked form.
    CheckLayout<seg::PRQ<Item>>("PRQ");
    CheckLayout<seg::FAAArray<Item>>("FAAArray");
    CheckLayout<seg::HQ<Item>>("HQ");
    CheckLayout<queue::SCQ<Item>>("SCQ");
}

/// SCQ is the reason layouts must support N regions: it carves five from one block.
TEST(SingleBlockLayout, ScqUsesFiveDisjointRegions) {
    const auto p = queue::SCQ<Item>::plan(64);
    ASSERT_EQ(p.regions.size(), 5u);
    EXPECT_TRUE(p.valid(sizeof(queue::SCQ<Item>)));
    for (size_t i = 1; i < p.regions.size(); ++i)
        EXPECT_GE(p.regions[i].offset, p.regions[i - 1].end()) << "regions " << i - 1 << "," << i;
}

/// The live object must actually sit inside its own block, not merely plan to.
template <typename Q>
void CheckAllocation(const char* name) {
    Q* q = Q::create(64);
    ASSERT_NE(q, nullptr) << name;
    const auto p = Q::plan(64);
    const auto base = reinterpret_cast<std::uintptr_t>(q);
    EXPECT_EQ(base % p.block_align, 0u) << name << ": block is under-aligned";
    for (const auto& r : p.regions)
        EXPECT_GE(base + r.offset, base + sizeof(Q)) << name << ": region inside the header";
    // Exercise it, so the checks above are not merely arithmetic.
    int payload = 0;
    Item out = nullptr;
    EXPECT_TRUE(q->enqueue(&payload)) << name;
    EXPECT_TRUE(q->dequeue(out)) << name;
    EXPECT_EQ(out, &payload) << name;
    Q::destroy(q);
}

TEST(SingleBlockAllocation, ObjectAndRegionsDoNotOverlap) {
    CheckAllocation<queue::Vyukov<Item>>("Vyukov");
    CheckAllocation<seg::PRQ<Item>>("PRQ");
    CheckAllocation<seg::FAAArray<Item>>("FAAArray");
    CheckAllocation<seg::HQ<Item>>("HQ");
    CheckAllocation<queue::SCQ<Item>>("SCQ");
}

/// linkage::None must cost nothing: the next pointer is the only difference.
TEST(Linkage, StandaloneCostsNothingExtra) {
    // Only algorithms with a standalone form can be compared this way; PRQ, FAAArray and
    // HQ are constrained to linkage::Linked and have no standalone type to measure.
    EXPECT_LT(sizeof(queue::Vyukov<Item>), sizeof(seg::Vyukov<Item>));
    EXPECT_LT(sizeof(queue::SCQ<Item>), sizeof(seg::SCQ<Item>));
    EXPECT_EQ(sizeof(seg::Vyukov<Item>) - sizeof(queue::Vyukov<Item>), CACHE_LINE)
        << "the linked form should differ by exactly one padded next pointer";
}

} // namespace
