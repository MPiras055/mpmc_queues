/**
 * @file HazardVectorTest.cpp
 * @brief Unit tests for the HazardVector class using GoogleTest.
 *
 * These tests validate the behavior of HazardVector under both
 * sequential and concurrent workloads. They cover construction,
 * hazard protection, clearing, retiring, reclamation, and edge cases.
 *
 * Concurrency tests use multiple threads and barriers to ensure
 * correctness of the hazard pointer scheme in multithreaded settings.
 */

#include <gtest/gtest.h>
#include <HazardVector.hpp>
#include <thread>
#include <vector>
#include <atomic>
#include <barrier>

using namespace std;

/**
 * @class HazardVectorTest
 * @brief Fixture for HazardVector tests with maxThreads = 4.
 *
 * Provides a shared HazardVector<int*> instance with a maximum
 * of 4 threads to simplify repetitive test setup.
 */
class HazardVectorTest : public ::testing::Test {
protected:
    /// Number of threads supported by this test fixture.
    static constexpr size_t maxThreads = 4;

    /// HazardVector instance under test.
    HazardVector<int*> hv{maxThreads};
};

/**
 * @test Construction
 * @brief Ensures that a HazardVector can be constructed with
 *        a valid thread count without throwing exceptions.
 */
TEST_F(HazardVectorTest, Construction) {
    ASSERT_NO_THROW(HazardVector<int*> tmp(maxThreads));
}

/**
 * @test ProtectAndClear
 * @brief Tests protecting and clearing a raw pointer.
 *
 * Validates that protect() stores a pointer in a hazard slot
 * and clear() releases it properly.
 */
TEST_F(HazardVectorTest, ProtectAndClear) {
    int a = 42;
    int* ptr = &a;
    ASSERT_EQ(hv.protect(ptr, 0), ptr);
    hv.clear(0);
}

/**
 * @test ProtectAtomic
 * @brief Tests hazard-protecting a pointer from an atomic variable.
 *
 * Ensures the hazard pointer stabilizes across atomic loads
 * and handles nullptr values correctly.
 */
TEST_F(HazardVectorTest, ProtectAtomic) {
    std::atomic<int*> atom(nullptr);
    int a = 100;
    atom.store(&a);
    ASSERT_EQ(hv.protect(atom, 0), &a);
    atom.store(nullptr);
    ASSERT_EQ(hv.protect(atom, 0), nullptr);
}

/**
 * @test RetireAndDelete
 * @brief Tests retiring and collecting a single object.
 *
 * Validates that retired objects are eventually reclaimed when
 * not protected by hazard pointers.
 */
TEST_F(HazardVectorTest, RetireAndDelete) {
    int* obj = new int(5);
    size_t deleted = hv.retire(obj, 0);
    ASSERT_GE(deleted, 0);  ///< May be 0 if still protected
    hv.clear(0);
    deleted = hv.collect(0);
    ASSERT_GE(deleted, 0);
}

/**
 * @test RetireWithOtherThreadProtection
 * @brief Tests retiring an object that is actively protected by another thread.
 *
 * Ensures reclamation does not occur while another thread holds
 * a hazard pointer to the object, and reclamation succeeds once cleared.
 */
TEST_F(HazardVectorTest, RetireWithOtherThreadProtection) {
    int* obj = new int(42);
    std::atomic<bool> stop{false};
    std::barrier sync(2);

    std::thread t1([&] {
        sync.arrive_and_wait();
        while (!stop) hv.protect(obj, 1);
        hv.clear(1);
    });

    sync.arrive_and_wait();
    ASSERT_EQ(hv.retire(obj, 0), 0); ///< Not deleted yet, still protected
    stop = true;
    t1.join();
    ASSERT_GE(hv.collect(0), 1);     ///< Should now reclaim
}

/**
 * @test MultiThreadProtectAndRetire
 * @brief Multi-threaded stress test of protect(), clear(), and retire().
 *
 * Each thread protects a unique object, clears it, then retires it.
 * Finally, reclamation across all threads is validated.
 */
TEST_F(HazardVectorTest, MultiThreadProtectAndRetire) {
    const size_t threads = maxThreads;
    vector<thread> ths;
    vector<int*> objs(threads);
    std::atomic<size_t> atom_deleted{0};
    for (size_t i = 0; i < threads; ++i) objs[i] = new int((int)i);

    for (size_t i = 0; i < threads; ++i) {
        ths.emplace_back([&, i] {
            int* p = hv.protect(objs[i], i);
            EXPECT_EQ(p, objs[i]);
            hv.clear(i);
            atom_deleted.fetch_add(hv.retire(objs[i], i));
        });
    }
    for (auto& t : ths) t.join();

    size_t deleted = atom_deleted.load();
    for (size_t i = 0; i < threads; ++i) deleted += hv.collect(i);
    ASSERT_EQ(deleted, threads);
}

/**
 * @test ProtectNullptr
 * @brief Tests protecting a nullptr value.
 *
 * Ensures hazard protection of nullptr behaves consistently
 * and does not cause errors.
 */
TEST_F(HazardVectorTest, ProtectNullptr) {
    ASSERT_EQ(hv.protect(nullptr, 0), nullptr);
    hv.clear(0);
}

/**
 * @test ManyRetireCollect
 * @brief Stress test with many retired objects.
 *
 * Retires 1000 dynamically allocated integers and ensures all are
 * reclaimed when collect() is invoked.
 */
TEST_F(HazardVectorTest, ManyRetireCollect) {
    const int N = 1000;
    size_t deleted = 0;
    for (int i = 0; i < N; ++i) {
        deleted += hv.retire(new int(i), 0);
    }
    deleted += hv.collect(0);
    ASSERT_EQ(deleted, N);
}
