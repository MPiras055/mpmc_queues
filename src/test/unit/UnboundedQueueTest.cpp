#include "OptionsPack.hpp"
#include <gtest/gtest.h>
#include <thread>
#include <atomic>
#include <vector>
#include <random>
#include <PRQSegment.hpp>
#include <UnboundedProxy.hpp>
#include <HQSegment.hpp>
#include <FAAArray.hpp>
#include <SCQueue.hpp>

// ---- List of queue implementations to test ----
typedef ::testing::Types<
    UnboundedProxy<uint64_t*,queue::segment::LinkedHQ>,
    UnboundedProxy<uint64_t*,queue::segment::LinkedPRQ>,
    UnboundedProxy<uint64_t*,queue::segment::LinkedFAAArray>,
    UnboundedProxy<uint64_t*,queue::segment::LinkedSCQ>
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
    uint64_t a = 1, b = 2, c = 3;
    uint64_t* out = nullptr;

    this->q.enqueue(&a);
    this->q.enqueue(&b);
    this->q.enqueue(&c);

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
    uint64_t dummy;
    uint64_t *dummy_out;
    for (size_t i = 0; i < this->q.capacity(); i++) {
        this->q.enqueue(&dummy);
    }
    EXPECT_EQ(this->q.size(), this->q.capacity());
    this->q.enqueue(&dummy); // full segment (this should trigger adding a new segment)
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
    uint64_t* out = nullptr;
    EXPECT_FALSE(this->q.dequeue(out)); // should not crash or succeed
    this->q.release();
}

TYPED_TEST(QueueTest, FillAndEmpty) {
    EXPECT_TRUE(this->q.acquire());
    uint64_t dummy;
    uint64_t* out = nullptr;

    EXPECT_EQ(this->q.size(),0u);  // empty

    //completely fills 2 segments of the queue
    for (size_t i = 0; i < this->q.capacity() * 2; i++) {
        this->q.enqueue(&dummy);
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
    const uint64_t N = (1024 << 8);
    std::atomic<uint64_t> sum{0};
    uint64_t* out = nullptr;

    std::thread prod([&] {
        EXPECT_TRUE(this->q.acquire()); //each thread should get a slot
        for (int i = 1; i <= N; i++) {
            uint64_t* val = new uint64_t(i); // simulate dynamic allocation
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

    EXPECT_EQ(sum.load(), 1LL * N * (N + 1) / 2);

}

TYPED_TEST(QueueTest, MultiProducerMultiConsumer_DrainMode) {
    // 1. Configuration & Constants
    // Use uint64_t for all data to prevent overflow and handle large N
    using T = uint64_t;
    const uint64_t N = (1024 * 1024); // 1 Million items
    const int P = 1;
    const int C = 7;

    ASSERT_EQ(N % P, 0) << "N must be divisible by P";

    // 2. Shared State
    // Global Verification Structures (Atomic for thread-safe final commit)
    std::atomic<uint64_t> global_sum{0};
    std::vector<std::atomic<uint64_t>> global_seen(N);
    for(auto& x : global_seen) x.store(0, std::memory_order_relaxed);

    // The Stop Flag
    std::atomic<bool> producers_finished{false};

    // Data Pool (Raw integers stored as pointers)
    std::unique_ptr<T[]> raw_data(new T[N]);
    std::vector<T*> items(N);
    for(uint64_t i = 0; i < N; ++i) {
        raw_data[i] = i;
        items[i] = &raw_data[i];
    }

    // 3. Producer Lambda
    auto producer_func = [&](int id) {
        EXPECT_TRUE(this->q.acquire());

        uint64_t chunk = N / P;
        uint64_t start = id * chunk;
        uint64_t end   = start + chunk;

        for(uint64_t i = start; i < end; ++i) {
            this->q.enqueue(items[i]);
        }

        this->q.release();
    };

    // 4. Consumer Lambda
    auto consumer_func = [&](int id) {
        EXPECT_TRUE(this->q.acquire());

        // --- Local Accumulators (No atomic overhead) ---
        uint64_t local_sum = 0;
        // Allocating local vector for speed (8MB per thread is acceptable)
        std::vector<uint64_t> local_seen(N, 0);
        T* val_ptr = nullptr;

        // --- Phase 1: Spin until Signal ---
        // Keep consuming while producers are active
        while (!producers_finished.load(std::memory_order_acquire)) {
            if (this->q.dequeue(val_ptr)) {
                if (val_ptr) {
                    T val = *val_ptr;
                    local_sum += val;
                    if (val < N) local_seen[val]++;
                }
            } else {
                // Reduce contention if queue is temporarily empty
                std::this_thread::yield();
            }
        }

        // --- Phase 2: Final Drain ---
        // Producers are gone. Consume everything left.
        while (this->q.dequeue(val_ptr)) {
            if (val_ptr) {
                T val = *val_ptr;
                local_sum += val;
                if (val < N) local_seen[val]++;
            }
        }

        // --- Phase 3: Commit to Global ---
        // Add local results to global atomics
        global_sum.fetch_add(local_sum, std::memory_order_relaxed);

        // This loop handles the merge.
        // Note: For very large N, merging might take a moment, but it's safe.
        for(size_t i = 0; i < N; ++i) {
            if (local_seen[i] > 0) {
                global_seen[i].fetch_add(local_seen[i], std::memory_order_relaxed);
            }
        }

        this->q.release();
    };

    // 5. Execution
    std::vector<std::thread> producers, consumers;

    // Start Consumers first (they will spin waiting for data/flag)
    for(int i=0; i<C; ++i) consumers.emplace_back(consumer_func, i);

    // Start Producers
    for(int i=0; i<P; ++i) producers.emplace_back(producer_func, i);

    // Wait for Producers to finish
    for(auto& t : producers) t.join();

    // SIGNAL: Tell consumers no more data is coming
    producers_finished.store(true, std::memory_order_release);

    // Wait for Consumers to drain and commit
    for(auto& t : consumers) t.join();

    // 6. Verification (Performed by Main)
    // Formula for sum of 0..N-1: (N-1)*N / 2
    uint64_t expected_sum = (N * (N - 1)) / 2;
    EXPECT_EQ(global_sum.load(), expected_sum) << "Total sum mismatch!";

    // Check for distinctness (No duplicates, No drops)
    for(size_t i = 0; i < N; ++i) {
        uint64_t count = global_seen[i].load(std::memory_order_relaxed);
        ASSERT_EQ(count, 1) << "Value " << i << " seen " << count << " times.";
    }
}


// ------------------------------------------------
// Stress / Randomized Tests
// ------------------------------------------------

TYPED_TEST(QueueTest, RandomizedWorkload) {
    const int OPS = 1000000;
    uint64_t a = 42;
    uint64_t* out = nullptr;
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
