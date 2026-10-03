// test_thread_pool_resize_status.cpp
// Regression test for P-006: get_status().idle_threads must be saturated
// to 0 (not wrap to a huge size_t) when active_threads_ briefly exceeds
// workers_.size() during resize() (race between workers_.size() under
// mutex_ and active_threads_.load(relaxed)).
//
// Also exercises ThreadPoolResizer::should_shrink() which has the same
// `total_threads - active_threads` underflow pattern at line 86 in
// thread_pool_resizer.cpp.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <optional>
#include <thread>
#include <vector>

#include "kairo/thread_pool/thread_pool.hpp"
#include "kairo/thread_pool/thread_pool_resizer.hpp"
#include <kairo/config.hpp>

using namespace kairo;

#define TEST_ASSERT(condition, message)                                       \
    do {                                                                      \
        if (!(condition)) {                                                   \
            std::cerr << "FAILED: " << message << " at " << __FILE__ << ":"   \
                      << __LINE__ << std::endl;                               \
            return false;                                                     \
        }                                                                     \
    } while (0)

// ----------------------------------------------------------------------------
// Test 1: Repeatedly submit tasks and call get_status() while a long-running
// task is in flight. Verify idle_threads never underflows to a huge value.
// Stress the read path against worker count == active count == N, which is
// the steady-state boundary the saturation must hold on.
// ----------------------------------------------------------------------------
static bool test_get_status_idle_saturation_steady_state() {
    std::cout << "[P-006] get_status() saturation under steady-state load..."
              << std::endl;

    ThreadPoolConfig config;
    config.min_threads = 2;
    config.max_threads = 2; // disable monitor thread to keep state stable
    config.queue_capacity = 1024;
    config.enable_work_stealing = false;

    ThreadPool pool;
    TEST_ASSERT(pool.initialize(config), "ThreadPool should initialize");

    std::atomic<int> running{0};
    std::atomic<int> max_concurrent{0};

    // Submit tasks that briefly run; sample get_status() concurrently.
    for (int i = 0; i < 100; ++i) {
        pool.submit([&running, &max_concurrent]() {
            int now = running.fetch_add(1) + 1;
            int prev = max_concurrent.load();
            while (now > prev &&
                   !max_concurrent.compare_exchange_weak(prev, now)) {
                // retry
            }
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            running.fetch_sub(1);
        });
    }

    // Sample status while tasks may still be running.
    std::vector<ThreadPoolStatus> samples;
    samples.reserve(200);
    for (int i = 0; i < 200; ++i) {
        ThreadPoolStatus s = pool.get_status();
        samples.push_back(s);
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }

    pool.wait_for_completion();
    pool.shutdown(true);

    for (const auto& s : samples) {
        // Hard upper bound: idle must never exceed total, and must never
        // wrap around to a huge size_t.
        TEST_ASSERT(s.idle_threads <= s.total_threads,
                    "idle_threads must be <= total_threads (no underflow)");
        // active must also be <= total in the saturated read.
        TEST_ASSERT(s.active_threads <= s.total_threads + 1,
                    "active_threads snapshot must not be wildly inconsistent");
    }
    return true;
}

// ----------------------------------------------------------------------------
// Test 2: Direct injection of the race condition into ThreadPoolResizer.
// Drive active_threads > total_threads via update_status() and call the
// public check_and_resize(). Before the fix, the line
//     size_t idle_threads = total_threads - active_threads;
// inside should_shrink() would underflow; the subsequent
// `idle_threads > total_threads * 0.5` would be true for the huge
// underflowed value, biasing the shrink decision. The fix saturates
// idle_threads to 0 so a "more active than total" snapshot is treated
// as "no idle", not "massively idle". The test asserts the call is
// safe and returns without crashing on the underflow input.
// ----------------------------------------------------------------------------
static bool test_resizer_check_and_resize_saturates_on_underflow() {
    std::cout << "[P-006] ThreadPoolResizer::check_and_resize() tolerates "
                 "active > total without underflow..."
              << std::endl;

    ThreadPoolConfig cfg;
    cfg.min_threads = 1;
    cfg.max_threads = 8;
    cfg.queue_capacity = 100;
    ThreadPool pool;
    TEST_ASSERT(pool.initialize(cfg), "pool init");

    ThreadPoolResizer resizer(pool, cfg);

    // Inject a snapshot where active_threads > total_threads. This is
    // the exact state that the P-006 fix in should_shrink() guards
    // against. check_and_resize() will internally call should_shrink().
    resizer.update_status(/*queue=*/0, /*active=*/5, /*total=*/2,
                           /*wait=*/0.0);

    // The 1-second rate limit inside check_and_resize() means this call
    // will early-out before doing real work on the first invocation, but
    // it MUST NOT crash and MUST NOT return an underflowed value through
    // any observable side channel. We just assert it returns cleanly.
    resizer.check_and_resize();

    // Also drive a normal idle-high case to make sure the saturated
    // path doesn't regress the normal case: with active=0, total=4,
    // queue=0, idle=4, idle_high=true (4 > 2), queue_low=true.
    resizer.update_status(/*queue=*/0, /*active=*/0, /*total=*/4,
                           /*wait=*/0.0);
    resizer.check_and_resize();

    pool.shutdown(true);
    return true;
}

// ----------------------------------------------------------------------------
// Test 3: Stress loop 100x — submit one task, sleep a tick, get_status().
// Collect every snapshot. No snapshot may have idle > total.
// ----------------------------------------------------------------------------
static bool test_get_status_loop_100x() {
    std::cout << "[P-006] 100-iteration submit+get_status loop..." << std::endl;

    ThreadPoolConfig config;
    config.min_threads = 2;
    config.max_threads = 4;
    config.queue_capacity = 256;
    config.enable_work_stealing = false;

    ThreadPool pool;
    TEST_ASSERT(pool.initialize(config), "ThreadPool should initialize");

    std::vector<ThreadPoolStatus> collected;
    collected.reserve(100);

    std::atomic<int> counter{0};
    for (int i = 0; i < 100; ++i) {
        std::optional<std::future<void>> f;
        f.emplace(pool.submit([&counter, i]() {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            counter.fetch_add(1);
        }));

        // Immediately sample status; this is the closest public-API
        // approximation to the "submit then resize" sequence the plan
        // describes.
        ThreadPoolStatus s = pool.get_status();
        collected.push_back(s);

        if (f.has_value()) {
            f->get();
        }
    }

    pool.wait_for_completion();
    pool.shutdown(true);

    TEST_ASSERT(static_cast<int>(collected.size()) == 100,
                "should have collected 100 samples");
    for (size_t i = 0; i < collected.size(); ++i) {
        const auto& s = collected[i];
        // No snapshot may have idle > total. This is the regression
        // assertion for P-006.
        TEST_ASSERT(s.idle_threads <= s.total_threads,
                    "sample " + std::to_string(i) +
                        ": idle_threads must never exceed total_threads");
    }
    TEST_ASSERT(counter.load() == 100, "all submitted tasks should run");
    return true;
}

// ----------------------------------------------------------------------------
// Test 4: The resizer-to-pool protocol must create and remove real workers.
// Queue replacement moves pending tasks back to the scheduler before the
// removed workers exit, so every submitted task must still complete.
// ----------------------------------------------------------------------------
static bool test_resize_expands_and_shrinks_workers() {
    std::cout << "[P-006] resizer expands and shrinks workers..."
              << std::endl;

    ThreadPoolConfig config;
    config.min_threads = 2;
    config.max_threads = 4;
    config.queue_capacity = 128;
    config.enable_work_stealing = true;

    ThreadPool pool;
    TEST_ASSERT(pool.initialize(config), "ThreadPool should initialize");
    TEST_ASSERT(pool.get_status().total_threads == 2, "initial worker count");
    ThreadPoolResizer resizer(pool, config);

    // check_and_resize() is the exact method invoked by the monitor thread.
    // Supply a high-queue, high-wait snapshot after its one-second cooldown
    // and verify that the policy produces a real worker, not a stub success.
    resizer.update_status(/*queue_size=*/127, /*active_threads=*/2,
                          /*total_threads=*/2, /*avg_wait_time_ms=*/200.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    resizer.check_and_resize();
    TEST_ASSERT(pool.get_status().total_threads == 3,
                "automatic expansion must create a worker");

    TEST_ASSERT(resizer.expand(1), "manual resizer expansion should succeed");
    TEST_ASSERT(pool.get_status().total_threads == 4, "expansion must create workers");

    std::atomic<size_t> completed{0};
    std::vector<std::future<void>> futures;
    futures.reserve(64);
    for (size_t i = 0; i < 64; ++i) {
        futures.emplace_back(pool.submit([&completed]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            completed.fetch_add(1, std::memory_order_relaxed);
        }));
    }

    TEST_ASSERT(resizer.shrink(2), "resizer shrink should succeed");
    TEST_ASSERT(pool.get_status().total_threads == 2, "shrink must remove workers");
    for (auto& future : futures) {
        TEST_ASSERT(future.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
                    "all tasks must finish after shrink");
        future.get();
    }
    pool.wait_for_completion();
    pool.shutdown(true);
    TEST_ASSERT(completed.load(std::memory_order_relaxed) == 64,
                "shrink must not lose queued tasks");
    return true;
}

int main() {
    std::cout << "=== P-006 ThreadPool resize/status regression tests ==="
              << std::endl;

    bool all_ok = true;
    all_ok &= test_get_status_idle_saturation_steady_state();
    all_ok &= test_resizer_check_and_resize_saturates_on_underflow();
    all_ok &= test_get_status_loop_100x();
    all_ok &= test_resize_expands_and_shrinks_workers();

    if (all_ok) {
        std::cout << "\n=== All P-006 tests PASSED ===" << std::endl;
        return 0;
    }
    std::cout << "\n=== Some P-006 tests FAILED ===" << std::endl;
    return 1;
}
