#include <gtest/gtest.h>
#include <LFring.hpp>
#include <thread>
#include <vector>
#include <cstdint>

static void verify_queue_behaviour(auto* q) {
    const size_t cap = q->capacity();

    for(size_t i = 0; i < cap; i++) {
        EXPECT_TRUE(q->enqueue(i));
    }

    // for(size_t i = cap; i < 2*cap; i++) {
    //     EXPECT_FALSE(q->enqueue(i));
    // }

    for(size_t i = 0; i < cap; i++) {
        size_t val = -1;
        EXPECT_TRUE(q->dequeue(val));
        EXPECT_EQ(val,i);
    }

    for(size_t i = 0; i < cap; i++) {
        size_t val = -1;
        EXPECT_FALSE(q->dequeue(val));
        EXPECT_EQ(val,static_cast<size_t>(-1));
    }
}

// Helper to check alignment
bool is_aligned(void* ptr, size_t alignment) {
    return reinterpret_cast<uintptr_t>(ptr) % alignment == 0;
}

// =========================================================================
// 1. STANDARD ALLOCATION TESTS (Stack/Heap Separate)
// =========================================================================

TEST(LFringTest, StandardConstruction) {
    // 1. Stack allocation
    queue::LFring<size_t> q(1024);

    // 2. Functional check
    verify_queue_behaviour(&q);

    // 3. Destructor runs automatically here (and frees separate buffer)
}

TEST(LFringTest, StandardHeapAllocation) {
    // 1. Standard new (not optimized create)
    auto* q = new queue::LFring<size_t>(1024);

    verify_queue_behaviour(q);

    delete q; // Should free everything correctly
}

// =========================================================================
// 2. OPTIMIZED FACTORY TESTS (Single Block)
// =========================================================================

TEST(LFringTest, OptimizedCreate) {
    size_t capacity = 1024;

    // 1. Create using optimized factory
    auto* q = queue::LFring<size_t>::create(capacity);
    ASSERT_NE(q, nullptr);

    verify_queue_behaviour(q);

    // 4. Clean up using standard delete (Relies on operator delete override)
    delete q;
}

// =========================================================================
// 3. SLAB ALLOCATION TESTS (Bulk)
// =========================================================================

TEST(LFringSlabTest, SlabAllocationBasics) {
    size_t count = 4;
    size_t size = 1024;

    // 1. Create Slab
    queue::LFringSlab<size_t> slab(count, size);

    EXPECT_EQ(slab.count(), count);

    // 2. Access Queues
    for(size_t i = 0; i < count; ++i) {
        auto* q = slab.get(i);
        ASSERT_NE(q, nullptr);

        // Enqueue unique values to each queue
        const size_t cap = q->capacity();
        for(size_t i = 0; i < cap; i++) {
            EXPECT_TRUE(q->enqueue(i));
        }
    }

    // 3. Verify Independence
    for(size_t i = 0; i < count; ++i) {
        auto* q = slab.get(i);
        ASSERT_NE(q, nullptr);

        // Enqueue unique values to each queue
        const size_t cap = q->capacity();
        for(size_t i = 0; i < cap; i++) {
            size_t val = -1;
            EXPECT_TRUE(q->dequeue(val));
            EXPECT_EQ(i,val);
        }

        for(size_t i = 0; i < cap * 2; i++) {
            size_t val = -1;
            EXPECT_FALSE(q->dequeue(val));
            EXPECT_EQ(val,static_cast<size_t>(-1));

        }
    }
}

TEST(LFringSlabTest, CacheLineIsolation) {
    size_t count = 2;
    size_t size = 1024; // Power of 2

    queue::LFringSlab<size_t> slab(count, size);

    auto* q0 = slab.get(0);
    auto* q1 = slab.get(1);

    uintptr_t addr0 = reinterpret_cast<uintptr_t>(q0);
    uintptr_t addr1 = reinterpret_cast<uintptr_t>(q1);

    // 1. Alignment Check
    EXPECT_TRUE(is_aligned(q0, CACHE_LINE));
    EXPECT_TRUE(is_aligned(q1, CACHE_LINE));

    // 2. Stride Check
    size_t diff = addr1 - addr0;

    // Calculate expected stride manually
    size_t expected_stride = queue::LFring<size_t>::bytes_needed(size);

    verify_queue_behaviour(q0);
    verify_queue_behaviour(q1);

    EXPECT_EQ(diff, expected_stride);
    EXPECT_GE(diff, CACHE_LINE) << "Queues must be at least a cache line apart";
    EXPECT_EQ(diff % CACHE_LINE, 0) << "Stride must be a multiple of cache line size";
}

// =========================================================================
// 4. STRESS & CONCURRENCY
// =========================================================================

TEST(LFringSlabTest, ConcurrentAccess) {
    size_t num_threads = 4;
    size_t ops_per_thread = 4095;
    queue::LFringSlab<size_t> slab(num_threads, 4096);

    std::vector<std::thread> threads;

    // Each thread gets its own private queue from the slab
    for(size_t i = 0; i < num_threads; ++i) {
        threads.emplace_back([&slab, i, ops_per_thread]() {
            auto* q = slab.get(i);

            // Push
            for(size_t k = 0; k < ops_per_thread; ++k) {
                while(!q->enqueue(k)); // Busy wait if full (shouldn't be with 4096)
            }

            // Pop
            for(size_t k = 0; k < ops_per_thread; ++k) {
                size_t val;
                while(!q->dequeue(val));
                ASSERT_EQ(val, k);
            }
        });
    }

    for(auto& t : threads) t.join();
}

// =========================================================================
// 5. MEMORY CALCULATION LOGIC
// =========================================================================

TEST(LFringTest, BytesNeededLogic) {
    size_t size = 1024;
    // Assuming LFRING uses 8 bytes per slot for size_t (on CACHE_LINE-bit system)
    // LFRING_SIZE(10) -> roughly 1024 * 8 + metadata

    size_t bytes = queue::LFring<size_t>::bytes_needed(size);

    // Must be > sizeof(LFring)
    EXPECT_GT(bytes, sizeof(queue::LFring<size_t>));

    // Must be multiple of CACHE_LINE
    EXPECT_EQ(bytes % CACHE_LINE, 0);

    // Verify it handles padding correctly
    // If we request a weird small size, it should still align up
    size_t small_bytes = queue::LFring<size_t>::bytes_needed(1);
    EXPECT_EQ(small_bytes % CACHE_LINE, 0);
}
