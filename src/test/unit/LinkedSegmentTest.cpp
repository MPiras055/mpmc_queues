/**
 * @file LinkedSegmentTest.cpp
 * @brief GoogleTest suite for testing Linked Segment lifecycle using a test proxy.
 *
 * This suite validates the lifecycle semantics of linked segments. It covers:
 *  - Explicit open/close lifecycle
 *  - Automatic close when full
 *  - Edge conditions around capacity boundaries
 *  - Idempotency of open/close
 *  - Correctness of draining after closure
 *
 * A test proxy (`LinkedSegmentTestProxy`) is used to access the protected/friend
 * lifecycle methods safely, without breaking production encapsulation.
 */

#include <gtest/gtest.h>
#include <vector>
#include <IProxy.hpp>
#include <CASLoopSegment.hpp>  // replace with your actual linked segment header

// =========================================================
// Test Proxy
// =========================================================
/**
 * @brief Proxy wrapper for LinkedSegment tests.
 *
 * Exposes lifecycle methods (`open`, `close`, `isOpened`, `isClosed`) 
 * and capacity for testing purposes.
 *
 * @tparam Seg Linked segment type (e.g., CASLoopSegment<int*>)
 */
template <typename T, template<typename, typename> typename Seg>
struct TestProxy {
    using Segment = Seg<T, TestProxy>;

    explicit TestProxy(size_t cap) : seg(cap) {}

    bool enqueue(T v) { return seg.enqueue(v); }
    bool dequeue(T& out) { return seg.dequeue(out); }
    bool open() { return seg.open(); }
    bool close() { return seg.close(); }
    bool isOpened() const { return seg.isOpened(); }
    bool isClosed() const { return seg.isClosed(); }
    size_t capacity() const { return seg.capacity(); }

private:
    Segment seg;
};

// =========================================================
// Test Fixture
// =========================================================
typedef ::testing::Types<
    TestProxy<int *,LinkedCASLoop>  ///< Replace `void` with actual proxy type if needed
> LinkedQueueTypes;

template <typename T>
class LinkedSegmentTest : public ::testing::Test {
protected:
    /// Small capacity to trigger auto-close easily
    T q{8};
};
TYPED_TEST_SUITE(LinkedSegmentTest, LinkedQueueTypes);

// =========================================================
// Lifecycle Tests
// =========================================================

/**
 * @test New segments should start opened.
 */
TYPED_TEST(LinkedSegmentTest, StartsOpened) {
    EXPECT_TRUE(this->q.isOpened());
    EXPECT_FALSE(this->q.isClosed());
}

/**
 * @test Close prevents enqueue, but allows dequeue.
 */
TYPED_TEST(LinkedSegmentTest, ClosePreventsEnqueue) {
    int a = 42;
    EXPECT_TRUE(this->q.enqueue(&a));

    EXPECT_TRUE(this->q.close());
    EXPECT_TRUE(this->q.isClosed());
    EXPECT_FALSE(this->q.enqueue(&a));
}

/**
 * @test Dequeue works after segment is closed.
 */
TYPED_TEST(LinkedSegmentTest, DequeueStillWorksAfterClose) {
    int a = 1, b = 2;
    int* out = nullptr;

    EXPECT_TRUE(this->q.enqueue(&a));
    EXPECT_TRUE(this->q.enqueue(&b));

    EXPECT_TRUE(this->q.close());
    EXPECT_TRUE(this->q.dequeue(out));
    EXPECT_EQ(out, &a);
    EXPECT_TRUE(this->q.dequeue(out));
    EXPECT_EQ(out, &b);
    EXPECT_FALSE(this->q.dequeue(out));
}

/**
 * @test Reopen allows reuse after close.
 */
TYPED_TEST(LinkedSegmentTest, ReopenAfterClose) {
    int a = 99;
    int* out = nullptr;

    EXPECT_TRUE(this->q.close());
    EXPECT_TRUE(this->q.open());

    EXPECT_TRUE(this->q.enqueue(&a));
    EXPECT_TRUE(this->q.dequeue(out));
    EXPECT_EQ(out, &a);
}

/**
 * @test open() and close() are idempotent.
 */
TYPED_TEST(LinkedSegmentTest, IdempotentCloseAndOpen) {
    EXPECT_TRUE(this->q.isOpened());

    EXPECT_TRUE(this->q.close());
    EXPECT_TRUE(this->q.close());  // safe to close again
    EXPECT_TRUE(this->q.isClosed());

    EXPECT_TRUE(this->q.open());
    EXPECT_TRUE(this->q.open());   // safe to open again
    EXPECT_TRUE(this->q.isOpened());
}

// =========================================================
// Auto-Close Tests
// =========================================================

/**
 * @test Segment auto-closes when capacity is reached.
 */
TYPED_TEST(LinkedSegmentTest, AutoCloseWhenFull) {
    int dummy;
    for (size_t i = 0; i < this->q.capacity(); i++) {
        EXPECT_TRUE(this->q.enqueue(&dummy));
    }
    EXPECT_FALSE(this->q.enqueue(&dummy));//queue is actually fall 
    EXPECT_TRUE(this->q.isClosed());
    EXPECT_FALSE(this->q.enqueue(&dummy));
}

/**
 * @test Dequeue drains correctly after auto-close.
 */
TYPED_TEST(LinkedSegmentTest, AutoCloseThenDequeueRemaining) {
    std::vector<int> values(this->q.capacity());
    int* out = nullptr;

    // Fill the queue to capacity
    for (size_t i = 0; i < values.size(); i++) {
        values[i] = static_cast<int>(i);
        EXPECT_TRUE(this->q.enqueue(&values[i]));
    }

    // The next enqueue should fail and trigger auto-close
    int extra = 999;
    EXPECT_FALSE(this->q.enqueue(&extra));
    EXPECT_TRUE(this->q.isClosed());

    // Now dequeue exactly capacity() items
    for (size_t i = 0; i < values.size(); i++) {
        EXPECT_TRUE(this->q.dequeue(out));
        EXPECT_EQ(out, &values[i]);
    }

    // Queue should now be empty
    EXPECT_FALSE(this->q.dequeue(out));
}


/**
 * @test Reopen after auto-close allows reuse.
 */
TYPED_TEST(LinkedSegmentTest, AutoCloseThenReopen) {
    int val = 123;
    int* out = nullptr;

    for (size_t i = 0; i < this->q.capacity(); i++) {
        EXPECT_TRUE(this->q.enqueue(&val));
    }

    // enqueue on a full queue triggers closure
    EXPECT_FALSE(this->q.enqueue(&val));
    EXPECT_TRUE(this->q.isClosed());

    // closed segments allow for dequeue
    EXPECT_TRUE(this->q.dequeue(out));
    EXPECT_TRUE(this->q.open());

    // open segments that are not full allow for enqueues
    EXPECT_TRUE(this->q.enqueue(&val));
    EXPECT_TRUE(this->q.dequeue(out));
    EXPECT_EQ(out, &val);
}

// =========================================================
// Edge Case Tests
// =========================================================

/**
 * @test Enqueue capacity() - 1 keeps segment open.
 */
TYPED_TEST(LinkedSegmentTest, EdgeAlmostFull) {
    int dummy;
    for (size_t i = 0; i < this->q.capacity() - 1; i++) {
        EXPECT_TRUE(this->q.enqueue(&dummy));
    }
    EXPECT_TRUE(this->q.isOpened());
}

/**
 * @test Enqueue exactly capacity keeps segment open
 */
TYPED_TEST(LinkedSegmentTest, EdgeFullCloses) {
    int dummy;
    for (size_t i = 0; i < this->q.capacity(); i++) {
        EXPECT_TRUE(this->q.enqueue(&dummy));
    }
    EXPECT_FALSE(this->q.isClosed());
}

/**
 * @test Closing empty segment allows safe reuse.
 */
TYPED_TEST(LinkedSegmentTest, CloseEmptySegmentAndReuse) {
    int val = 77;
    int* out = nullptr;

    EXPECT_TRUE(this->q.close());
    EXPECT_TRUE(this->q.open());
    EXPECT_TRUE(this->q.enqueue(&val));
    EXPECT_TRUE(this->q.dequeue(out));
    EXPECT_EQ(out, &val);
}

/**
 * @test Closing after last enqueue retains all data.
 */
TYPED_TEST(LinkedSegmentTest, CloseAfterLastEnqueueKeepsData) {
    int val = 5;
    int* out = nullptr;

    for (size_t i = 0; i < this->q.capacity() - 1; i++) {
        EXPECT_TRUE(this->q.enqueue(&val));
    }

    EXPECT_TRUE(this->q.enqueue(&val)); // last slot
    EXPECT_TRUE(this->q.close());

    size_t drained = 0;
    while (this->q.dequeue(out)) {
        drained++;
    }
    EXPECT_EQ(drained, this->q.capacity());
}
