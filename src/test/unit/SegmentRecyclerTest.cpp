#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>

// Include your SegmentRecycler header
#include "Recycler.hpp"

using namespace util::hazard;

// ---------------- MockSegment ----------------
template <typename T, typename Recycler>
struct MockSegment {
    friend Recycler;
    std::atomic<bool> opened{true};

    bool open() {
        opened.store(true);
        return true;
    }

    bool close() {
        opened.store(false);
        return true;
    }

    bool isOpen() const { return opened.load(); }
    bool isClosed() const { return !isOpen(); }

    size_t size() const { return 0; } // always empty for testing
};

// ---------------- Test Fixture ----------------
class SegmentRecyclerTest : public ::testing::Test {
protected:
    static constexpr size_t SEGMENTS = 4;
    static constexpr size_t CAPACITY = 8;
    static constexpr size_t THREADS  = 8;
    using Segment = MockSegment<int*,void>;
    using TestRecycler = Recycler<Segment>;
    TestRecycler recycler{SEGMENTS, THREADS, CAPACITY};
};

// ---------------- Tests ----------------

// API Correctness
TEST_F(SegmentRecyclerTest, BasicCachePutGet) {
    TestRecycler::Tml index;

    // Get from cache
    ASSERT_TRUE(recycler.get_cache(index));
    Segment* seg = recycler.decode(index);
    ASSERT_NE(seg, nullptr);
    EXPECT_TRUE(seg->isOpen());

    // Close & recycle into cache
    ASSERT_TRUE(seg->close());
    seg->open();
    ASSERT_TRUE(recycler.put_cache(index));

    // Get again from cache
    ASSERT_TRUE(recycler.get_cache(index));
}

//Test if the cache contains all indexes
TEST_F(SegmentRecyclerTest, CacheIsConsistent) {
    std::vector<TestRecycler::Tml> tracked;
    std::vector<Segment*> segments;
    TestRecycler::Tml current;
    for(size_t i = 0; i < SEGMENTS; i++) {
        ASSERT_TRUE(recycler.get_cache(current));
        tracked.push_back(current);
        segments.push_back(recycler.decode(current));
    }

    //Cache should be empty
    ASSERT_FALSE(recycler.get_cache(current));

    //sort and check if no missing spots
    std::sort(tracked.begin(),tracked.end());
    for(size_t i = 1; i < tracked.size(); i++) {
        ASSERT_EQ(tracked[i-1] + 1, tracked[i]);    //no missing spots
    }

    //check if all indexes map to consistent segments
    for(auto& seg : segments) {
        ASSERT_TRUE(seg->isOpen());
    }

}

TEST_F(SegmentRecyclerTest, ReclaimSingle) {
    TestRecycler::Tml idx;
    TestRecycler::Tml idx_cmp;
    ASSERT_TRUE(recycler.get_cache(idx));
    recycler.protect_epoch(0);
    recycler.retire(idx,0,true);    //automatically drops protection
    ASSERT_TRUE(recycler.reclaim(idx_cmp,0));
    ASSERT_EQ(idx,idx_cmp);
}

TEST_F(SegmentRecyclerTest, ReclaimAll) {
    std::vector<TestRecycler::Tml> tracked;
    TestRecycler::Tml idx;
    for(size_t i = 0; i < SEGMENTS; i++) {
        ASSERT_TRUE(recycler.get_cache(idx));
        tracked.push_back(idx);
    }
    ASSERT_FALSE(recycler.get_cache(idx));
    ASSERT_FALSE(recycler.reclaim(idx,0));  //quarantine empty
    recycler.protect_epoch(0);
    for(auto& tml : tracked) {
        recycler.retire(idx,0,false);   //don't drop protection
    }
    recycler.clear_epoch(0);    //drop protection here
    for(size_t i = 0; i < tracked.size(); i++) {
        ASSERT_TRUE(recycler.reclaim(idx,0));
    }
    ASSERT_FALSE(recycler.reclaim(idx,0));  //quarantine empty

}

// ---------------- Concurrency ----------------
TEST_F(SegmentRecyclerTest, ConcurrentGetRecycle) {
    constexpr size_t OPS = 100000;
    std::vector<std::thread> workers;

    for (size_t t = 0; t < THREADS; ++t) {
        workers.emplace_back([&, t]() {
            TestRecycler::Tml index;
            Segment* seg;
            for (size_t i = 0; i < OPS; i++) {
                if(recycler.get_cache(index) || recycler.reclaim(index,t))
                    seg = recycler.decode(index);
                else {
                    i--;
                    continue;
                }

                // //don't use every few iteration
                if((i % 37) == 0) {
                    ASSERT_TRUE(recycler.put_cache(index));
                    continue;
                }
                //simulate usage
                recycler.protect_epoch(t);
                if(!seg->isOpen())
                    seg->open();
                for(size_t i = 0; i < 4; i++) {
                    std::this_thread::yield();
                }
                recycler.retire(index,t,true);  //drop protection
            }
        });
    }

    for (auto& w : workers) w.join();
}
