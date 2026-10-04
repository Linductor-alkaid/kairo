// tests/test_object_pool_release_bench.cpp
//
// Micro-benchmark for ObjectPool::release (P-260618-003).
//
// Before P-260618-003 each release() performed two O(n) scans (over storage_
// to find the owning node, and over free_list_ to detect double-release), so
// draining a full pool of size N was O(N^2). After the change release() is
// O(1) and draining is O(N). This test verifies the per-release cost grows
// LINEARLY (not quadratically) by comparing the time to drain pools of
// {256, 1024, 4096} objects.
//
// It asserts on TIME RATIOS between capacities, never on absolute nanoseconds,
// and takes the minimum of a few samples so it stays stable on shared/noisy
// CI. Linear growth is a ~4x ratio per 4x capacity step; quadratic growth
// would be ~16x. The thresholds below (10x and 20x) leave wide margin while
// still catching an O(n^2) regression (which would blow past 10x at the
// 256 -> 1024 step).

#include "util/object_pool.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

// Builds a fresh pool of `capacity` ints, drains it (acquires every slot),
// then times releasing them all back. Repeats `repeats` times and returns the
// fastest sample to suppress background scheduling noise.
long long time_release_full_pool(size_t capacity, int repeats) {
    long long best = 0;
    for (int r = 0; r < repeats; ++r) {
        kairo::util::ObjectPool<int> pool(capacity);
        std::vector<int*> objs;
        objs.reserve(capacity);
        for (size_t i = 0; i < capacity; ++i) {
            // Pool has exactly `capacity` slots, so acquire() never returns
            // nullptr here; no release() of these handles happens until timed.
            objs.push_back(pool.acquire());
        }

        auto t0 = Clock::now();
        for (int* p : objs) {
            pool.release(p);
        }
        auto t1 = Clock::now();

        long long ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        if (best == 0 || ns < best) {
            best = ns;
        }
    }
    return best;
}

TEST(ObjectPoolReleaseBench, DrainTimeGrowsLinearlyNotQuadratically) {
    const long long ns_256  = time_release_full_pool(256, 3);
    const long long ns_1024 = time_release_full_pool(1024, 3);
    const long long ns_4096 = time_release_full_pool(4096, 3);

    // Sanity: the smallest pool must take a measurable, non-zero amount of
    // time so the ratios below are meaningful rather than a division artefact.
    ASSERT_GT(ns_256, 0);

    // Linear growth gives ~4x per 4x capacity step; quadratic gives ~16x.
    // Thresholds carry generous margin for noisy CI.
    const double ratio_1024_over_256 =
        static_cast<double>(ns_1024) / static_cast<double>(ns_256);
    const double ratio_4096_over_1024 =
        static_cast<double>(ns_4096) / static_cast<double>(ns_1024);

    EXPECT_LT(ratio_1024_over_256, 10.0)
        << "drain(1024)/drain(256) = " << ratio_1024_over_256
        << ", expected linear (~4x); an O(n^2) regression would be ~16x";

    EXPECT_LT(ratio_4096_over_1024, 20.0)
        << "drain(4096)/drain(1024) = " << ratio_4096_over_1024
        << ", expected linear (~4x); an O(n^2) regression would be ~16x";
}

}  // namespace
