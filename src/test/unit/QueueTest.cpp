#include <gtest/gtest.h>
#include <thread>
#include <atomic>
#include <vector>
#include <random>
#include <CASLoopSegment.hpp>
#include <PRQSegment.hpp>

// ---- List of queue implementations to test ----
typedef ::testing::Types<
    //CASLoopQueue<int*>,
    PRQueue<int*>
    //, Other queues to add here
> QueueTypes;

// ---- Fixture ----
template <typename T>
class QueueTest : public ::testing::Test {
protected:
    T q{128}; // construct with capacity 128
};
TYPED_TEST_SUITE(QueueTest, QueueTypes);

// ------------------------------------------------
// Basic Functional Tests
// ------------------------------------------------

TYPED_TEST(QueueTest, EnqueueDequeueBasic) {
    int a = 1, b = 2, c = 3;
    int* out = nullptr;

    EXPECT_TRUE(this->q.enqueue(&a));
    EXPECT_TRUE(this->q.enqueue(&b));
    EXPECT_TRUE(this->q.enqueue(&c));

    EXPECT_TRUE(this->q.dequeue(out));
    EXPECT_EQ(out, &a);
    EXPECT_TRUE(this->q.dequeue(out));
    EXPECT_EQ(out, &b);
    EXPECT_TRUE(this->q.dequeue(out));
    EXPECT_EQ(out, &c);

    EXPECT_FALSE(this->q.dequeue(out)); // empty
}

TYPED_TEST(QueueTest, CapacityRespected) {
    int dummy;
    for (size_t i = 0; i < this->q.capacity(); i++) {
        EXPECT_TRUE(this->q.enqueue(&dummy));
    }
    EXPECT_EQ(this->q.size(), this->q.capacity());
    EXPECT_FALSE(this->q.enqueue(&dummy)); // full
}

// TYPED_TEST(QueueTest, OpenCloseLifecycle) {
//     int x;
//     int* out = nullptr;

//     EXPECT_TRUE(this->q.isOpened());
//     EXPECT_FALSE(this->q.isClosed());

//     EXPECT_TRUE(this->q.close());
//     EXPECT_TRUE(this->q.isClosed());
//     EXPECT_FALSE(this->q.enqueue(&x)); // enqueue fails when closed

//     EXPECT_TRUE(this->q.open());
//     EXPECT_TRUE(this->q.enqueue(&x)); // works again
//     EXPECT_TRUE(this->q.dequeue(out));
//     EXPECT_EQ(out, &x);
// }

// ------------------------------------------------
// Boundary Tests
// ------------------------------------------------

TYPED_TEST(QueueTest, DequeueFromEmpty) {
    int* out = nullptr;
    EXPECT_FALSE(this->q.dequeue(out)); // should not crash or succeed
}

TYPED_TEST(QueueTest, FillAndEmpty) {
    int dummy;
    int* out = nullptr;

    EXPECT_EQ(this->q.size(),0u);  // empty

    for (size_t i = 0; i < this->q.capacity(); i++) {
        EXPECT_TRUE(this->q.enqueue(&dummy));
    }
    EXPECT_EQ(this->q.size(), this->q.capacity());  // full

    EXPECT_FALSE(this->q.enqueue(&dummy));

    for (size_t i = 0; i < this->q.capacity(); i++) {
        EXPECT_TRUE(this->q.dequeue(out));
    }

    EXPECT_FALSE(this->q.dequeue(out)); // empty again
    EXPECT_EQ(this->q.size(), 0);
}

// ------------------------------------------------
// Concurrency Tests
// ------------------------------------------------

// TYPED_TEST(QueueTest, SingleProducerSingleConsumer) {
//     const int N = 200000;
//     std::atomic<long long> sum{0};
//     int* out = nullptr;

//     std::thread prod([&] {
//         int yield_count = 0;
//         for (int i = 1; i <= N; i++) {
//             int* val = new int(i); // simulate dynamic allocation
//             while (!this->q.enqueue(val)) {
//                 ++yield_count = 0;
//                 for(size_t i = 0; i < yield_count; i++) {
//                     std::this_thread::yield();
//                 }
//             } // spin
//             yield_count = 0;
//         }
//     });

//     std::thread cons([&] {
//         int yield_count = 0;
//         for (int i = 1; i <= N; i++) {
//             // this makes livelock (in livelock prone segments)
//             // harder to occur
//             while (!this->q.dequeue(out)) {
//                 ++yield_count;
//                 for(size_t i = 0; i < yield_count; i++){
//                     std::this_thread::yield();
//                 }
//             }

//             yield_count = 0;
//             sum.fetch_add(*out, std::memory_order_relaxed);
//             delete out; // cleanup
//         }
//     });

//     prod.join();
//     cons.join();

//     EXPECT_EQ(sum, 1LL * N * (N + 1) / 2);
    
// }

// TYPED_TEST(QueueTest, MultiProducerMultiConsumer) {
//     const int N = 200000, P = 4, C = 4;
//     std::vector<int*> produced;
//     produced.reserve(N);

//     // Preallocate unique addresses so there’s no allocator reuse confusion.
//     std::vector<std::unique_ptr<int>> pool;
//     pool.reserve(N);
//     for (int i = 1; i <= N; ++i) {
//         pool.emplace_back(std::make_unique<int>(i));
//         produced.push_back(pool.back().get());
//     }

//     std::atomic<long long> sum{0};
//     std::vector<std::thread> producers, consumers;
//     std::vector<std::atomic<int>> seen(N + 1); // index by value, count occurrences
//     for (auto& s : seen) s.store(0, std::memory_order_relaxed);

//     // Enqueue pointers partitioned by producer
//     for (int p = 0; p < P; ++p) {
//         producers.emplace_back([&, p]{
//             int yield_count = 0;
//             int start = p * (N / P) + 1;
//             int end   = (p + 1) * (N / P);
//             for (int i = start; i <= end; ++i) {
//                 int* val = produced[i-1];
//                 while (!this->q.enqueue(val)) {
//                     ++yield_count;
//                     for (int k = 0; k < yield_count; ++k) std::this_thread::yield();
//                 }
//                 yield_count = 0;
//             }
//         });
//     }

//     // Consumers: no delete; just record and sum
//     for (int c = 0; c < C; ++c) {
//         consumers.emplace_back([&]{
//             int* out = nullptr;
//             int yield_count = 0;
//             for (int i = 0; i < N / C; ++i) {
//                 while (!this->q.dequeue(out)) {
//                     ++yield_count;
//                     for (int k = 0; k < yield_count; ++k) std::this_thread::yield();
//                 }
//                 yield_count = 0;
//                 sum.fetch_add(*out, std::memory_order_relaxed);
//                 seen[*out].fetch_add(1, std::memory_order_relaxed);
//             }
//         });
//     }

//     for (auto& t : producers) t.join();
//     for (auto& t : consumers) t.join();

//     EXPECT_EQ(sum, 1LL * N * (N + 1) / 2);

//     // Check for duplicates or drops
//     for (int v = 1; v <= N; ++v) {
//         int c = seen[v].load(std::memory_order_relaxed);
//         EXPECT_EQ(c, 1) << "value " << v << " seen " << c << " times";
//     }
// }


// ------------------------------------------------
// Stress / Randomized Tests
// ------------------------------------------------

TYPED_TEST(QueueTest, RandomizedWorkload) {
    const int OPS = 10000;
    int a = 42;
    int* out = nullptr;
    std::mt19937 rng(12345);
    std::uniform_int_distribution<int> dist(0, 1);

    for (int i = 0; i < OPS; i++) {
        if (dist(rng)) {
            this->q.enqueue(&a); // ignore failure if full
        } else {
            this->q.dequeue(out); // ignore failure if empty
        }
    }

    // After random workload, queue must still be consistent
    size_t s = this->q.size();
    EXPECT_LE(s, this->q.capacity());
}
