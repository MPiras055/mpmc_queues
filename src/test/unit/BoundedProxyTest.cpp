#include <atomic>
#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <barrier>
#include <CASLoopSegment.hpp>
#include <PRQSegment.hpp>
#include <BoundedCounterProxy.hpp>
#include <BoundedChunkProxy.hpp>
#include <BoundedMemProxy.hpp>
#include <IProxy.hpp>

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
    BoundedChunkProxy<V,LinkedPRQ,SEGMENTS>,
    BoundedCounterProxy<V,LinkedPRQ,SEGMENTS>,
    BoundedMemProxy<V,LinkedPRQ,SEGMENTS>
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
    const size_t maxThreads = 8;
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
    const size_t batchSize = this->q.capacity();
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
    const size_t N = (1024 * 1024 * 10);

    //initialize a batch of data
    std::vector<Data> batch;
    batch.reserve(N);
    for(size_t i = 1; i <= N; i++) {
        batch.emplace_back(Data(0,i));
    }

    std::thread prod([&] {
        //preallocate a batch of items
        EXPECT_TRUE(this->q.acquire()); //thread should get a slot
        for(auto& item : batch) {
            while(!(this->q.enqueue(&item))) {
                //spin;
            }
        }
        this->q.release();
    });

    std::thread cons([&] {
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

    prod.join();
    cons.join();
}

TYPED_TEST(Concurrency,MultiProducerMultiConsumer) {
    const size_t N = 1024 * 1024 * 10;

    const auto test = [&](size_t prod, size_t cons) {
        EXPECT_LE(prod + cons, this->maxThreads);

        //split the items evenly among producers
        size_t per_producer = N / prod;
        size_t remainder = N % prod;    //the first REMAINDER producers will get +1 item

        //make sure producers exit after consumer
        std::barrier<> producerBarrier(prod + 1);
        std::atomic_bool consumerStop;
        std::atomic_uint64_t consumerItems{0};
        std::vector<std::jthread> producerList;

        for(size_t tid = 0; tid < prod; tid++) {
            producerList.emplace_back([&,tid] {
                // each thread allocates a bucket of items
                size_t batch_size = per_producer + (tid < remainder? 1 : 0);
                std::vector<Data> batch;
                batch.reserve(batch_size);
                for(uint64_t i = 0; i < batch_size; i++) {
                    batch.emplace_back(static_cast<uint64_t>(tid),i);
                }
                EXPECT_TRUE(this->q.acquire());
                for(auto& item : batch) {
                    while(!this->q.enqueue(&item)) {
                        //spin
                    }
                }
                this->q.release();
                producerBarrier.arrive_and_wait();
                //main sets the consumers flag (will join the barrier after all consumers are done)
                producerBarrier.arrive_and_wait();
                //at this point vector can get out of scope (consumers are out)
            });
        }
        std::vector<std::thread> consumerList;
        for(size_t tid = 0; tid < cons; tid++) {
            consumerList.emplace_back([&]{
                std::vector<uint64_t> producerLookup(prod);
                for(auto& p : producerLookup){p = 0;}   //init each producer value
                Data* out = nullptr;
                uint64_t items = 0;
                EXPECT_TRUE(this->q.acquire());
                while(!consumerStop.load(std::memory_order_acquire)) {
                    if(!(this->q.dequeue(out))) continue;
                    items++;
                    EXPECT_LE(producerLookup[out->tid],out->epoch);
                    producerLookup[out->tid] = out->epoch;
                }
                //drain the queue
                while(this->q.dequeue(out)) {
                    EXPECT_LE(producerLookup[out->tid],out->epoch);
                    producerLookup[out->tid] = out->epoch;
                    items++;
                }
                this->q.release();
                //add your count to
                consumerItems.fetch_add(items);
            });
        }
        producerBarrier.arrive_and_wait();
        consumerStop.store(true,std::memory_order_release);
        for(auto& t : consumerList) {
            t.join();
        }
        producerBarrier.arrive_and_wait();  //now producers are joinable (automatic)
        //expect items count to match
        EXPECT_EQ(N,consumerItems.load());
    };

    size_t tRatio = this->maxThreads / 4;

    // 25% producers - 75% consumers
    test(tRatio, 3 * tRatio);
    // 50% producers - 50% consumers
    test(2 * tRatio, 2 * tRatio);
    // 75% producers - 25% consumers
    test(3 * tRatio, tRatio);
}
