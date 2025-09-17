#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <CASLoopSegment.hpp>
// #include <PRQSegment.hpp>
#include <BoundedCounterProxy.hpp>
#include <BoundedChunkProxy.hpp>
#include <BoundedMemProxy.hpp>

struct Data {
    uint64_t tid;uint64_t epoch;
    Data() = default;
    Data(uint64_t t, uint64_t e): tid{t},epoch{e}{};
};

/**
 * static enviromental test variables
 */
static constexpr size_t SEGMENTS = 4;
static constexpr size_t FULL_CAPACITY = 1024 * 16;
static constexpr size_t SEG_CAPACITY = FULL_CAPACITY / SEGMENTS;

// ---- List of queue implementations to test ----
template<typename V>
using QueueTypes = ::testing::Types<
    BoundedChunkProxy<V,LinkedCASLoop,SEGMENTS>,
    BoundedCounterProxy<V,LinkedCASLoop,SEGMENTS>,
    BoundedMemProxy<V,LinkedCASLoop,SEGMENTS>
    //, Other queues to add here
>;

using QueuesOfData  = QueueTypes<Data*>;

// ---- Fixture ----
template <typename Q>
class Semantics: public ::testing::Test {
protected:
    const size_t maxThreads = 1;
    Q q{FULL_CAPACITY,maxThreads};

};
template <typename Q>
class Concurrency: public ::testing::Test {
protected:
    const size_t maxThreads = 16;
    Q q{FULL_CAPACITY,maxThreads};
};
TYPED_TEST_SUITE(Semantics, QueuesOfData);
TYPED_TEST_SUITE(Concurrency, QueuesOfData);

// ------------------------------------------------
// Basic Functional Tests
// ------------------------------------------------
TYPED_TEST(Semantics,CachedTicket) {
    /**
     * checks if the ticket is cached properly
     */
    EXPECT_TRUE(this->q.acquire()); //caches the ticket
    const size_t ignore_iter = this->maxThreads * 10;   //can be any number (meaningful if >> maxThreads)
    for(size_t i = 0; i < ignore_iter; i++) {
        //any number of iteration is successful since the ticket is cached by the first acquire (not cleared)
        EXPECT_TRUE(this->q.acquire());
    }
    this->q.release(); //release the ticket (clears the cache)
}


TYPED_TEST(Semantics,EnqueueDequeueBasic) {
    const size_t batchSize = 10;
    assert(batchSize <= this->q.capacity() && "Wrong test parameter batchSize");
    auto batch = std::vector<Data>(batchSize,{0,0});

    EXPECT_TRUE(this->q.acquire()); //acquire a ticket for queue operation

    for(size_t i = 0; i < batchSize; i++) {
        EXPECT_TRUE(this->q.enqueue(&(batch[i])));
    }
    EXPECT_EQ(this->q.size(),batchSize);
    Data* out{};
    //check fifo ordering
    for(size_t i = 0; i < batchSize; i++) {
        EXPECT_TRUE(this->q.dequeue(out));
        EXPECT_EQ(out,&(batch[i]));
    }
    Data* cmp = out;
    EXPECT_EQ(this->q.size(),0);
    EXPECT_FALSE(this->q.dequeue(out));
    EXPECT_EQ(out,cmp); //check that failed dequeue doesn't overwrite the out value
    EXPECT_EQ(this->q.size(),0);    //check if size is affected by a failed dequeue

    this->q.release();          //release the previously acquired ticket
}

TYPED_TEST(Semantics, SegmentLinking) {
    const size_t batchSize = this->q.capacity();
    auto batch = std::vector<Data>(batchSize, {0, 0});
    const size_t Segments = this->q.Segments; // static constexpr member
    const size_t SegmentCapacity = batchSize / Segments;

    // Check no rounding error in division
    EXPECT_EQ(Segments * SegmentCapacity, batchSize);

    EXPECT_TRUE(this->q.acquire()); // acquire ticket for queue operation

    // Enqueue: fill one segment at a time
    for (size_t i = 0; i < Segments; ++i) {
        const size_t segmentStart = i * SegmentCapacity;
        for (size_t j = 0; j < SegmentCapacity; ++j) {
            EXPECT_TRUE(this->q.enqueue(&(batch[segmentStart + j])));
        }
        // Queue size should match number of enqueued items so far
        EXPECT_EQ(this->q.size(), (i + 1) * SegmentCapacity);
    }

    // Queue should now be full
    EXPECT_EQ(this->q.size(), batchSize);

    // Try enqueuing when full: should fail
    Data dummyVal{};
    Data* dummy = &dummyVal;
    EXPECT_FALSE(this->q.enqueue(dummy));
    EXPECT_EQ(this->q.size(), batchSize);

    // Dequeue: one segment at a time
    Data* out = nullptr;
    for (size_t i = 0; i < Segments; ++i) {
        const size_t segmentStart = i * SegmentCapacity;
        for (size_t j = 0; j < SegmentCapacity; ++j) {
            EXPECT_TRUE(this->q.dequeue(out));
            EXPECT_EQ(out, &(batch[segmentStart + j])); // FIFO order
        }
        // After dequeuing one segment, check size
        EXPECT_EQ(this->q.size(), batchSize - (i + 1) * SegmentCapacity);
    }

    // Queue should now be empty
    Data* cmp = out;
    EXPECT_EQ(this->q.size(), 0);
    EXPECT_FALSE(this->q.dequeue(out));
    EXPECT_EQ(out, cmp); // make sure out wasn't overwritten
}


// ------------------------------------------------
// Concurrency Tests
// ------------------------------------------------
//
TYPED_TEST(Concurrency,SingleProducerSingleConsumer) {
    const size_t N = (1024 * 1024);

    //initialize a batch of data
    std::vector<Data> batch;
    batch.resize(N);

    std::jthread prod([&] {
        //preallocate a batch of items
        EXPECT_TRUE(this->q.acquire()); //thread should get a slot
        for(auto& item : batch) {
            while(!(this->q.enqueue(&item))) {
                //spin;
            }
        }
        this->q.release();
    });

    std::jthread cons([&] {
        EXPECT_TRUE(this->q.acquire());
        Data *out{reinterpret_cast<Data*>(0x1)};
        for(auto& cmp : batch) {
            while(!(this->q.dequeue(out))) {
                //spin
            }
            EXPECT_EQ(&cmp,out);    //check for fifo ordering of pointers
        }
        this->q.release();
    });
}
