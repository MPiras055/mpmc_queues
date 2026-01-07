#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>
#include <algorithm>
#include <barrier>
#include <atomic>
#include <OptionsPack.hpp>

// Include your SegmentRecycler header
#include "Recycler.hpp"

// ---------------- MockSegment ----------------
// Minimal segment to test open/close/reset behavior via the Recycler
template <typename T, typename Recycler>
struct MockSegment {
    friend Recycler;
    std::atomic<bool> opened{true};

    // Recycler often calls open() when retrieving from cache
    bool open() {
        opened.store(true);
        return true;
    }

    // Recycler doesn't strictly call close(), but the proxy does.
    // We track state for assertions.
    bool close() {
        opened.store(false);
        return true;
    }

    bool isOpen() const { return opened.load(); }
    bool isClosed() const { return !isOpen(); }

    size_t size() const { return 0; }
};

// ---------------- Test Fixture ----------------
class SegmentRecyclerTest : public ::testing::Test {
protected:
    static constexpr size_t SEGMENTS = 4;
    static constexpr size_t CAPACITY = 8;
    static constexpr size_t THREADS  = 4;

    using Segment = MockSegment<int*, void>;
};

// ---------------- Tests ----------------

// 1. Test Basic Cache Interaction
TEST_F(SegmentRecyclerTest, BasicCachePutGet) {
    static constexpr size_t CAPACITY    = 100;
    static constexpr size_t threads     = 1;
    using Rec = util::hazard::recycler::Recycler<Segment,CAPACITY>;
    using Index = Rec::Index;
    std::vector<Index> indexes;
    indexes.reserve(CAPACITY);

    Rec r(threads); //initialize the recycler

    Index idx = 0xDEAD;

    //initialize the recyclers cache
    while(r.reclaim(idx)) {
        r.put_cache(idx);
    }

    //drain the recyclers cache
    for(size_t i = 0; i < CAPACITY; i++) {
        EXPECT_TRUE(r.get_cache(idx));
        indexes.emplace_back(idx);
    }

    EXPECT_FALSE(r.get_cache(idx));

    //sort indexes and see if they map to an interval
    std::sort(indexes.begin(),indexes.end());
    for(size_t i = 1; i < CAPACITY; i++) {
        EXPECT_EQ(indexes[i-1] + 1, indexes[i]);
    }

    //check if indexes map to valid segment pointers
    for(size_t i = 0; i < CAPACITY; i++) {
        Segment* seg = r.decode(indexes[i]);
        EXPECT_TRUE(seg->isOpen());
    }

    //put back indexes to cache
    while(!indexes.empty()) {
        r.put_cache(indexes.back());
        indexes.pop_back();
    }

    //drain the recycler cache again
    for(size_t i = 0; i < CAPACITY; i++) {
        EXPECT_TRUE(r.get_cache(idx));
        indexes.emplace_back(idx);
    }

    EXPECT_FALSE(r.get_cache(idx));
}

// 2. Test Basic Reclaim and Retire Interaction
TEST_F(SegmentRecyclerTest, BasicRetireReclaim) {
    static constexpr size_t CAPACITY    = 100;
    static constexpr size_t threads     = 1;
    using Opt = util::hazard::recycler::RecyclerOpt;
    using Rec = util::hazard::recycler::Recycler<Segment,CAPACITY,meta::OptionsPack<Opt::Disable_Cache>>;
    using Index = Rec::Index;
    std::vector<Index> indexes;

    indexes.reserve(CAPACITY);
    Rec r(threads);
    Index idx;

    /// No need to register_thread() since
    /// the number of threads used in the
    /// recycler never exceeds the max
    /// initialization cap
    ///
    /// @note: remember using register_thread() if
    /// unsure if the threads using the recycler could
    /// exceed the init bound
    // r.register_thread();

    //drain the recyclers free bucket
    for(size_t i = 0; i < CAPACITY; i++) {
        EXPECT_TRUE(r.reclaim(idx));
        indexes.emplace_back(idx);
    }

    EXPECT_FALSE(r.reclaim(idx));

    //sort indexes and see if they map to an interval
    std::sort(indexes.begin(),indexes.end());
    for(size_t i = 1; i < CAPACITY; i++) {
        EXPECT_EQ(indexes[i-1] + 1, indexes[i]);
    }

    //check if indexes map to valid segment pointers
    for(size_t i = 0; i < CAPACITY; i++) {
        Segment* seg = r.decode(indexes[i]);
        EXPECT_TRUE(seg->isOpen());
    }

    //protect epoch in order to retire all segments
    r.protect_epoch();

    //put back indexes to cache
    while(!indexes.empty()) {
        r.retire(indexes.back());
        indexes.pop_back();
    }

    r.clear_epoch();

    //drain the recycler cache again
    for(size_t i = 0; i < CAPACITY; i++) {
        EXPECT_TRUE(r.reclaim(idx));
        indexes.emplace_back(idx);
    }

    //Free pool is empty again
    EXPECT_FALSE(r.reclaim(idx));
}

TEST_F(SegmentRecyclerTest, ThreadRegistrationCap) {
    static constexpr size_t CAPACITY    = 1;
    static constexpr size_t threads     = 1;
    using Opt = util::hazard::recycler::RecyclerOpt;
    using Rec = util::hazard::recycler::Recycler<Segment,CAPACITY,meta::OptionsPack<Opt::Disable_Cache>>;

    size_t actual_threads = 2;

    Rec r(threads);
    std::vector<std::thread> th;

    std::barrier threadsBarrier(actual_threads);

    //spawn 2 threads and make them fight for the tickets
    for(size_t i = 0; i < actual_threads; i++) {
        th.emplace_back([&]() {
            bool res = r.register_thread();
            if(res) {
                threadsBarrier.arrive_and_wait();
                r.unregister_thread();
                threadsBarrier.arrive_and_wait();
                threadsBarrier.arrive_and_wait();
            } else {
                //try to register but ticket is occupied
                EXPECT_FALSE(r.register_thread());
                //unlock other thread
                threadsBarrier.arrive_and_wait();
                //wait for ticket to be released
                threadsBarrier.arrive_and_wait();
                EXPECT_TRUE(r.register_thread());
                threadsBarrier.arrive_and_wait();
                r.unregister_thread();
            }
        });
    }

    for(auto& t : th) t.join();
    EXPECT_TRUE(r.register_thread());
    r.unregister_thread();
}

TEST_F(SegmentRecyclerTest,MetadataUtils) {
    static constexpr size_t CAPACITY    = 1;
    static constexpr size_t threads     = 10;
    using Opt   = meta::EmptyOptions;
    using Meta  = std::atomic<unsigned int>;

    using Rec = util::hazard::recycler::Recycler<Segment,CAPACITY,Opt,Meta>;

    Rec r(threads);
    std::vector<std::thread> th;
    std::barrier b(threads + 1);  //barrier for threads and main

    static constexpr unsigned int SET_VAL   = 1;
    static constexpr unsigned int RESET_VAL = 2;
    for(size_t i = 1; i <= threads; i++) {
        th.emplace_back([&,i](){
            EXPECT_TRUE(r.register_thread());
            //wait for main to set the data
            b.arrive_and_wait();
            Meta& m = r.getMetadata();
            EXPECT_EQ(m.load(std::memory_order_acquire),SET_VAL);
            m.store(i); //store the thread id
            b.arrive_and_wait();
            //wait for main to validate and reset the data
            b.arrive_and_wait();
            Meta& m1 = r.getMetadata();
            EXPECT_EQ(m.load(std::memory_order_acquire),RESET_VAL);
        });
    }

    //set threads metadata
    const auto init = [](Meta& meta) {
        meta.store(SET_VAL,std::memory_order_release);
    };
    r.metadataInit(init);
    b.arrive_and_wait();
    //wait for thread to set their metadata
    b.arrive_and_wait();
    unsigned int sum = 0;
    const auto validate = [&sum](const Meta& meta) {
        sum += meta.load(std::memory_order_acquire);
    };
    r.metadataIter(validate);
    //check the sum
    EXPECT_EQ(sum,((threads + 1)*(threads))/2);
    const auto reset = [](Meta& meta) {
        meta.store(RESET_VAL,std::memory_order_release);
    };
    r.metadataInit(reset);
    b.arrive_and_wait();
    for(auto& t : th) t.join();
}

TEST_F(SegmentRecyclerTest, EpochProtectionBlocksReclaim) {
    static constexpr size_t CAPACITY      = 4;
    static constexpr size_t WORKERS       = 5;
    static constexpr size_t PROTECTORS    = 1;
    static constexpr size_t TOTAL_THREADS = WORKERS + PROTECTORS;
    static constexpr size_t GAUSS_SUM     = (CAPACITY * (CAPACITY - 1)) / 2; // Sum of 0..N-1

    using Opt = util::hazard::recycler::RecyclerOpt;
    using Rec = util::hazard::recycler::Recycler<Segment, CAPACITY, meta::OptionsPack<Opt::Disable_Cache>>;
    using Index = Rec::Index;
    Rec r(TOTAL_THREADS);
    // Increased barriers to strictly order the "Check Fail" and "Clear Epoch" phases
    std::barrier sync_point(TOTAL_THREADS);
    std::barrier sync_point_w_main(TOTAL_THREADS + 1);

    std::vector<std::thread> threads;
    std::atomic<size_t> reclaimed_count_initial{0};
    std::atomic<size_t> reclaimed_sum_initial{0};
    std::atomic<size_t> reclaimed_count_final{0};
    std::atomic<size_t> reclaimed_sum_final{0};

    auto worker_task = [&](int id) {
        EXPECT_TRUE(r.register_thread());
        sync_point.arrive_and_wait(); // Sync 1: Start

        sync_point.arrive_and_wait();

        // 1. Initial Drain
        std::vector<Index> held_segments;
        Index idx;
        while(r.reclaim(idx)) {
            held_segments.push_back(idx);
            reclaimed_count_initial.fetch_add(1);
            reclaimed_sum_initial.fetch_add(idx);
        }

        sync_point.arrive_and_wait(); // Sync 2: Drain Complete E = 2 or 3

        // 2. Retire (Protector is protecting now)
        // r.protect_epoch();
        for(auto i : held_segments) r.retire(i);
        held_segments.clear();
        // r.clear_epoch();

        sync_point.arrive_and_wait(); // Sync 3: Retire Complete

        // 3. Failed Reclaim (Epoch IS protected)
        // Race fix: We check BEFORE signaling we are done
        EXPECT_FALSE(r.reclaim(idx)); // E = 2 => thread is blocking 1

        sync_point.arrive_and_wait(); // Sync 4: Checks Complete (Signal to protector)

        sync_point.arrive_and_wait(); // Sync 5: Epoch Cleared (Wait for protector)

        EXPECT_TRUE(held_segments.empty());
        // 4. Final Reclaim (Epoch released)
        while(r.reclaim(idx)) {
            held_segments.push_back(idx);
            reclaimed_count_final.fetch_add(1);
            reclaimed_sum_final.fetch_add(idx);
        }

        sync_point.arrive_and_wait();

        for(auto i : held_segments) r.retire(i);

        r.unregister_thread();
    };

    auto protector_task = [&]() {
        EXPECT_TRUE(r.register_thread());
        sync_point.arrive_and_wait(); // Sync 1: Start

        r.protect_epoch(); // Block reclamation NOW protects epoch 1

        sync_point.arrive_and_wait();
        //drain complete
        sync_point.arrive_and_wait(); // Sync 2: Wait for workers to Drain

        sync_point.arrive_and_wait(); // Sync 3: Wait for workers to Retire

        // Workers are checking EXPECT_FALSE now...
        sync_point.arrive_and_wait(); // Sync 4: Wait for workers to finish checks

        r.clear_epoch();   // Release SAFE: Workers are done checking

        sync_point.arrive_and_wait(); // Sync 5: Signal release

        sync_point.arrive_and_wait();

        r.unregister_thread();
    };

    for(size_t i = 0; i < WORKERS; ++i) threads.emplace_back(worker_task, i);
    threads.emplace_back(protector_task);

    for(auto& t : threads) if(t.joinable()) t.join();

    // Validate Counts
    EXPECT_EQ(reclaimed_count_initial.load(), CAPACITY);
    EXPECT_EQ(reclaimed_count_final.load(), CAPACITY);

    // Validate Sums (Check for data integrity using Gauss Sum)
    EXPECT_EQ(reclaimed_sum_initial.load(), GAUSS_SUM);
    EXPECT_EQ(reclaimed_sum_final.load(), GAUSS_SUM);

}


TEST_F(SegmentRecyclerTest, StressCacheAndReclaim) {
    static constexpr size_t TEST_THREADS     = 4;
    static constexpr size_t CAPACITY         = 3;
    static constexpr size_t TOTAL_ITERATIONS = 1000000;

    using Rec = util::hazard::recycler::Recycler<Segment, CAPACITY>;
    using Index = Rec::Index;

    Rec r(TEST_THREADS);
    std::barrier sync_point(TEST_THREADS);
    std::vector<std::thread> threads;

    auto worker_task = [&](size_t t_id) {
        EXPECT_TRUE(r.register_thread());

        // Per-thread iteration count
        size_t my_iters = TOTAL_ITERATIONS / TEST_THREADS;
        if (t_id < TOTAL_ITERATIONS % TEST_THREADS)
            my_iters++;

        // Small PRNG
        uint32_t seed = 0x12345678u + t_id * 77777u;
        auto next_rand = [&]() {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            return seed;
        };

        sync_point.arrive_and_wait();

        for (size_t i = 0; i < my_iters; ++i) {
            Index idx;
            bool got = false;

            // Acquire an index
            do {
                if (r.get_cache(idx)) {
                    got = true;
                    EXPECT_TRUE(r.decode(idx)->isOpen());
                } else if (r.reclaim(idx)) {
                    got = true;
                    Segment* s = r.decode(idx);
                    EXPECT_TRUE(s->open());
                } else {
                    std::this_thread::yield();
                }
            } while (!got);

            // 50%: return to cache
            // if (next_rand() % 2) {
            //     Segment* seg = r.decode(idx);
            //     r.put_cache(idx);  // MUST not overfill (design guarantee)
            //     continue;
            // } else
            {
                // 50%: retire
                Segment* seg = r.decode(idx);
                r.protect_epoch();
                seg->close();
                std::this_thread::yield();
                r.retire(idx);
                r.clear_epoch();
            }
        }

        r.unregister_thread();
    };

    // Launch
    for (size_t i = 0; i < TEST_THREADS; ++i)
        threads.emplace_back(worker_task, i);

    for (auto& t : threads)
        if (t.joinable()) t.join();

    // Final consistency check
    EXPECT_TRUE(r.register_thread());
    std::vector<Index> rec_state;
    rec_state.reserve(CAPACITY);

    Index idx;
    while (r.get_cache(idx))
        rec_state.push_back(idx);
    while (r.reclaim(idx))
        rec_state.push_back(idx);

    EXPECT_EQ(rec_state.size(), CAPACITY);

    std::sort(rec_state.begin(), rec_state.end());
    for (size_t i = 1; i < CAPACITY; ++i)
        EXPECT_EQ(rec_state[i - 1] + 1, rec_state[i]);

    r.unregister_thread();
}
