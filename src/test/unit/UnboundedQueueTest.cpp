#include <gtest/gtest.h>
#include <thread>
#include <atomic>
#include <vector>
#include <random>
#include <CASLoopSegment.hpp>
#include <PRQSegment.hpp>
#include <UnboundedProxy.hpp>

// ---- List of queue implementations to test ----
typedef ::testing::Types<
    UnboundedProxy<int*,LinkedPRQ>
    // UnboundedProxy<int*,LinkedCASLoop>
    //, Other queues to add here
> QueueTypes;

// ---- Fixture ----
template <typename T>
class QueueTest : public ::testing::Test {
protected:
    T q{1024,16}; //queues where each segment has capacity of 128 and can be used by 8 threads concurrently
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

TYPED_TEST(QueueTest, FreshAllocation) {
    EXPECT_TRUE(this->q.acquire());
    int dummy;
    int *dummy_out;
    for (size_t i = 0; i < this->q.capacity(); i++) {
        EXPECT_TRUE(this->q.enqueue(&dummy));
    }
    EXPECT_EQ(this->q.size(), this->q.capacity());
    EXPECT_TRUE(this->q.enqueue(&dummy)); // full segment (this should trigger adding a new segment)
    EXPECT_EQ(this->q.size(),this->q.capacity() + 1);

    for(size_t i = 0; i < this->q.capacity() + 1; i++) {
        EXPECT_TRUE(this->q.dequeue(dummy_out));
    }
    EXPECT_FALSE(this->q.dequeue(dummy_out));
    this->q.release();
}

// ------------------------------------------------
// Boundary Tests
// ------------------------------------------------

TYPED_TEST(QueueTest, DequeueFromEmpty) {
    EXPECT_TRUE(this->q.acquire());
    int* out = nullptr;
    EXPECT_FALSE(this->q.dequeue(out)); // should not crash or succeed
    this->q.release();
}

TYPED_TEST(QueueTest, FillAndEmpty) {
    EXPECT_TRUE(this->q.acquire());
    int dummy;
    int* out = nullptr;

    EXPECT_EQ(this->q.size(),0u);  // empty

    //completely fills 2 segments of the queue
    for (size_t i = 0; i < this->q.capacity() * 2; i++) {
        EXPECT_TRUE(this->q.enqueue(&dummy));
    }

    // EXPECT_EQ(this->q.size(), this->q.capacity() * 2);  // full segments

    //completely drains 2 segments of the queue
    for (size_t i = 0; i < this->q.capacity() * 2; i++) {
        EXPECT_TRUE(this->q.dequeue(out));
    }

    EXPECT_FALSE(this->q.dequeue(out)); // empty again
    EXPECT_EQ(this->q.size(), 0);
    this->q.release();
}

// ------------------------------------------------
// Concurrency Tests
// ------------------------------------------------

TYPED_TEST(QueueTest, SingleProducerSingleConsumer) {
    const int N = (1024 << 7) * 2;
    std::atomic<long long> sum{0};
    int* out = nullptr;

    std::thread prod([&] {
        EXPECT_TRUE(this->q.acquire()); //each thread should get a slot
        for (int i = 1; i <= N; i++) {
            int* val = new int(i); // simulate dynamic allocation
            this->q.enqueue(val);   //unbounded queues are always successful on enqueues
        }
        this->q.release();
    });

    std::thread cons([&] {
        EXPECT_TRUE(this->q.acquire());
        for (int i = 1; i <= N; i++) {
            while (!this->q.dequeue(out)) {}
            sum.fetch_add(*out, std::memory_order_relaxed);
            delete out; // cleanup
        }
        this->q.release();
    });

    prod.join();
    cons.join();

    EXPECT_EQ(sum, 1LL * N * (N + 1) / 2);

}

TYPED_TEST(QueueTest, MultiProducerMultiConsumer) {
    const int N = (1024 << 15), P = 8, C = 8;

    std::vector<int*> produced;
    produced.reserve(N);

    // Preallocate unique addresses so there’s no allocator reuse confusion.
    std::vector<std::unique_ptr<int>> pool;
    pool.reserve(N);
    for (int i = 1; i <= N; ++i) {
        pool.emplace_back(std::make_unique<int>(i));
        produced.push_back(pool.back().get());
    }

    std::atomic<long long> sum{0};
    std::vector<std::thread> producers, consumers;
    std::vector<std::atomic<int>> seen(N + 1); // index by value, count occurrences
    for (auto& s : seen) s.store(0, std::memory_order_relaxed);

    // Enqueue pointers partitioned by producer
    for (int p = 0; p < P; ++p) {
        producers.emplace_back([&, p]{
            EXPECT_TRUE(this->q.acquire()); //each thread should successfuly acquire a ticket from the queue
            int start = p * (N / P) + 1;
            int end   = (p + 1) * (N / P);
            for (int i = start; i <= end; ++i) {
                int* val = produced[i-1];
                while (!this->q.enqueue(val)){}
            }
            this->q.release(); //each thread successfuly releases the ticket from the queue
        });
    }

    // Consumers: no delete; just record and sum
    for (int c = 0; c < C; ++c) {
        consumers.emplace_back([&]{
            EXPECT_TRUE(this->q.acquire()); //each thread should successfuly acquire a ticket from the queue
            int* out = nullptr;
            for (int i = 0; i < N / C; ++i) {
                size_t attempt = 0;
                while (!this->q.dequeue(out)) {}
                sum.fetch_add(*out, std::memory_order_relaxed);
                seen[*out].fetch_add(1, std::memory_order_relaxed);
            }
            this->q.release(); //each thread successfuly releases the previously acquired ticket
        });
    }

    for (auto& t : producers) t.join();
    for (auto& t : consumers) t.join();

    EXPECT_EQ(sum, 1LL * N * (N + 1) / 2);

    // Check for duplicates or drops
    for (int v = 1; v <= N; ++v) {
        int c = seen[v].load(std::memory_order_relaxed);
        EXPECT_EQ(c, 1) << "value " << v << " seen " << c << " times";
    }
}


// ------------------------------------------------
// Stress / Randomized Tests
// ------------------------------------------------

TYPED_TEST(QueueTest, RandomizedWorkload) {
    const int OPS = 1000000;
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
