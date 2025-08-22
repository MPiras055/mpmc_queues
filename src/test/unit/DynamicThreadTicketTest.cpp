#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>
#include <unordered_set>
#include "DynamicThreadTicket.hpp"

using util::threading::DynamicThreadTicket;

class DynamicThreadTicketTest : public ::testing::Test {
protected:
    static constexpr std::uint64_t maxThreads = 16;
    DynamicThreadTicket dtt{maxThreads};
};

/**
 * @test Verify that a single thread can acquire and release a ticket.
 */
TEST_F(DynamicThreadTicketTest, SingleThreadAcquireRelease) {
    std::uint64_t t;
    ASSERT_TRUE(dtt.acquire(t));
    EXPECT_LT(t, maxThreads);
    dtt.release();

    // Reacquire should yield the same ticket (cached per-thread).
    ASSERT_TRUE(dtt.acquire(t));
    EXPECT_LT(t, maxThreads);
    dtt.release();
}

/**
 * @test Verify that different threads acquire unique tickets until exhaustion.
 */
TEST_F(DynamicThreadTicketTest, MultiThreadExhaustion) {
    std::vector<std::thread> threads;
    std::mutex set_mutex;
    std::unordered_set<std::uint64_t> acquired;
    std::atomic<int> successes{0};

    // Launch maxThreads threads, each should get a distinct ticket.
    for (std::uint64_t i = 0; i < maxThreads; ++i) {
        threads.emplace_back([&]() {
            std::uint64_t t;
            if (dtt.acquire(t)) {
                {
                    std::lock_guard lk(set_mutex);
                    acquired.insert(t);
                }
                ++successes;
                // Hold ticket until join
            }
        });
    }
    for (auto &th : threads) th.join();

    EXPECT_EQ(successes.load(), maxThreads);
    EXPECT_EQ(acquired.size(), maxThreads);

    // Now no tickets should be available to the main thread.
    std::uint64_t extra;
    EXPECT_FALSE(dtt.acquire(extra));

    // Clean up: release in each thread
    threads.clear();
    for (std::uint64_t i = 0; i < maxThreads; ++i) {
        threads.emplace_back([&]() { dtt.release(); });
    }
    for (auto &th : threads) th.join();
}

/**
 * @test Verify that tickets are reused after release.
 */
TEST_F(DynamicThreadTicketTest, ReuseAfterRelease) {
    std::uint64_t t1, t2;
    ASSERT_TRUE(dtt.acquire(t1));
    EXPECT_LT(t1, maxThreads);

    dtt.release();

    // After release, reacquire should succeed.
    ASSERT_TRUE(dtt.acquire(t2));
    EXPECT_LT(t2, maxThreads);
    dtt.release();
}

/**
 * @test Stress test: many threads repeatedly acquire/release tickets.
 */
TEST_F(DynamicThreadTicketTest, StressAcquireRelease) {
    constexpr int kThreads = 16;
    constexpr int kIters   = 100000;

    std::atomic<int> counter{0};
    std::vector<std::thread> threads;

    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&]() {
            for (int j = 0; j < kIters; ++j) {
                std::uint64_t t;
                if (dtt.acquire(t)) {
                    EXPECT_LT(t, maxThreads);
                    dtt.release();
                    counter.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto &th : threads) th.join();

    EXPECT_EQ(counter.load(), kThreads * kIters);
}

/**
 * @brief Test that a thread gets the same ticket for multiple acquires
 *        **before** calling release, and that after release, it can acquire
 *        a ticket again (possibly the same or a different one).
 */
TEST_F(DynamicThreadTicketTest, TicketIsThreadLocal) {
    std::uint64_t t1 = DynamicThreadTicket::INVALID_ID;
    std::uint64_t t2 = DynamicThreadTicket::INVALID_ID;

    // First acquire must succeed
    ASSERT_TRUE(dtt.acquire(t1));
    // Second acquire on same thread should succeed and return the cached ticket
    ASSERT_TRUE(dtt.acquire(t2));
    ASSERT_EQ(t1, t2);

    dtt.release();

    // After release, acquire should reassign the same ticket (lowest free)
    ASSERT_TRUE(dtt.acquire(t2));
    EXPECT_EQ(t1, t2);

    dtt.release();
}



/**
 * @test Ensure that multiple instances of DynamicThreadTicket do not interfere.
 */
TEST(DynamicThreadTicketStandalone, MultipleInstancesIndependent) {
    DynamicThreadTicket dtt1{8};
    DynamicThreadTicket dtt2{8};

    std::uint64_t t1, t2;
    ASSERT_TRUE(dtt1.acquire(t1));
    ASSERT_TRUE(dtt2.acquire(t2));

    EXPECT_NE(t1, DynamicThreadTicket::INVALID_ID);
    EXPECT_NE(t2, DynamicThreadTicket::INVALID_ID);

    dtt1.release();
    dtt2.release();
}
