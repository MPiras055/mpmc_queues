#include <gtest/gtest.h>
#include <thread>
#include <atomic>
#include <barrier>
#include <vector>
#include <random>
#include <chrono>
#include <CASLoopSegment.hpp>
#include <PRQSegment.hpp>
#include <BoundedCounterProxy.hpp>
#include <BoundedChunkProxy.hpp>
#include <BoundedMemProxy.hpp>


static constexpr size_t segments = 128;
static constexpr size_t full_capacity = 1024 * 16;
static constexpr size_t segment_capacity = full_capacity / segments;

// ---- List of queue implementations to test ----
typedef ::testing::Types<
    // BoundedChunkProxy<int*,LinkedCASLoop,segments>,
    // BoundedCounterProxy<int*,LinkedCASLoop,segments>,
    // BoundedCounterProxy<int*,LinkedPRQ,segments>,
    // BoundedChunkProxy<int*,LinkedPRQ,segments>,
    BoundedMemProxy<int*,LinkedCASLoop,segments>


    //, Other queues to add here
> QueueTypes;

// ---- Fixture ----
template <typename T>
class QueueTest : public ::testing::Test {
protected:
    T q{full_capacity,20}; //queues where each segment has capacity of 128 and can be used by 8 threads concurrently

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
    EXPECT_EQ(this->q.size(), 0);

    //fill the queue
    for(size_t i = 0; i < segments; i++) {
        for(size_t i = 0; i < segment_capacity; i++) {
            EXPECT_TRUE(this->q.enqueue(&dummy));
        }
        //for each segment check if the size is correct
        EXPECT_EQ(this->q.size(), segment_capacity * (i + 1));
    }

    EXPECT_EQ(this->q.size(),this->q.capacity());

    //check if capacity constraint is respected
    //check if fragmentation constraint is respected
    for(size_t i = 0; i < segment_capacity; i++) {
        EXPECT_FALSE(this->q.enqueue(&dummy));
    }

    //empty the queue, and for each segment emptied check if size matches
    for(signed int i = segments - 1; i >= 0; i--) {
        for(size_t i = 0; i < segment_capacity; i++) {
            EXPECT_TRUE(this->q.dequeue(dummy_out));
        }
    }

    EXPECT_FALSE(this->q.dequeue(dummy_out));
    EXPECT_EQ(this->q.size(),0);
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
    const size_t segments_fill = segments - 1;
    assert(segments_fill != 0 && "Wrong test parameter");

    EXPECT_EQ(this->q.size(),0u);  // empty

    //completely fills most segments of the queue
    for (size_t i = 0; i < segment_capacity * segments_fill; i++) {
        EXPECT_TRUE(this->q.enqueue(&dummy));
    }

    EXPECT_EQ(this->q.size(), (segments - 1) * segment_capacity);  // check if size matches

    //completely drains 2 segments of the queue
    for (size_t i = 0; i < segments_fill * segment_capacity; i++) {
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
    const int N = (1024 * 1024);
    std::atomic<long long> sum{0};
    int* out = nullptr;

    std::thread prod([&] {
        EXPECT_TRUE(this->q.acquire()); //each thread should get a slot
        for (int i = 1; i <= N; i++) {
            int* val = new int(i); // simulate dynamic allocation
            while(!this->q.enqueue(val)){};   //unbounded queues are always successful on enqueues
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
    const int N = (1024 * 1024 * 10), P = 8, C = 8;
    
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
    std::barrier b(P + 1);
    for (auto& s : seen) s.store(0, std::memory_order_relaxed);

    // Enqueue pointers partitioned by producer
    for (int p = 0; p < P; ++p) {
        producers.emplace_back([&, p]{
            EXPECT_TRUE(this->q.acquire()); //each thread should successfuly acquire a ticket from the queue
            b.arrive_and_wait();
            int start = p * (N / P) + 1;
            int end   = (p + 1) * (N / P);
            for (int i = start; i <= end; ++i) {
                int* val = produced[i-1];
                while (!this->q.enqueue(val)){}
            }
            this->q.release(); //each thread successfuly releases the ticket from the queue
            std::puts("Producer exiting");
        });
    }

    b.arrive_and_wait();

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
    const int OPS = 1024 * 1024 * 20;
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
