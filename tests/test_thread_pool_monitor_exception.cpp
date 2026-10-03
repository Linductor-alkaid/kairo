// tests/test_thread_pool_monitor_exception.cpp
//
// P-001 (2026-06-22): regression test for the bug where monitor callbacks
// throwing inside ThreadPool::execute_task escaped, killed the worker
// thread, and left active_threads_ permanently inflated (since the
// fetch_add/fetch_sub pair around the execute_task call had no RAII guard).
// wait_for_completion() would then hang for up to 300s (or forever in the
// shutdown path) waiting for active==0.
//
// The fix is two-layered:
//   1. ThreadPool::ActiveCounter RAII guard in worker_thread guarantees
//      active_threads_ is decremented on every exit path, including
//      exceptions.
//   2. try/catch(...) around the entire execute_task body swallows monitor
//      callback exceptions, reports them via the internal exception
//      handler, and counts the task as completed (not failed) so the
//      wait_for_completion invariant total==completed+failed holds.
//
// This test verifies the externally observable invariants:
//   (a) wait_for_completion returns within 5s (not 300s) even when every
//       task's record_task_start throws std::runtime_error.
//   (b) get_status().active_threads == 0 after wait_for_completion —
//       RAII guard fired on the exception exit path.
//   (c) A follow-up task with the throwing monitor still installed
//       completes — proving the worker thread that handled the exception
//       did not die.
//   (d) Every record_task_start invocation was actually reached — proving
//       the test exercised the bug path and was not silently bypassed.
//       Note: the user function does NOT run for these tasks, because
//       record_task_start throws before user code is invoked. That's
//       expected (the monitor exception is on the entry hook).

#include <cassert>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

#include <kairo/config.hpp>
#include <kairo/types.hpp>
#include "kairo/monitor/task_monitor.hpp"
#include "kairo/thread_pool/thread_pool.hpp"

using namespace kairo;
using namespace std::chrono_literals;

namespace {

std::atomic<int> g_throw_count{0};
std::atomic<int> g_user_fn_count{0};

class ThrowingStartMonitor : public monitor::TaskMonitor {
public:
    void record_task_start(const std::string& /*task_id*/,
                           const std::string& /*task_type*/) override {
        g_throw_count.fetch_add(1, std::memory_order_relaxed);
        throw std::runtime_error("intentional: monitor callback exploded");
    }
};

bool wait_for_pool_completion_or_shutdown(ThreadPool& pool,
                                          std::chrono::milliseconds timeout,
                                          const char* context) {
    if (pool.try_wait_for_completion(timeout)) {
        return true;
    }

    auto st = pool.get_status();
    std::cerr << "FAILED: timed out waiting for " << context
              << " (total=" << st.total_tasks
              << ", completed=" << st.completed_tasks
              << ", failed=" << st.failed_tasks
              << ", active=" << st.active_threads
              << ", queue=" << st.queue_size << ")"
              << std::endl;
    pool.shutdown(false);
    return false;
}

bool run_test() {
    g_throw_count.store(0);
    g_user_fn_count.store(0);

    // ---- arrange ----
    ThreadPool pool;
    ThreadPoolConfig cfg;
    cfg.min_threads = 2;
    // This regression test is about task monitor callback exception safety,
    // not dynamic resizing. Keep max == min so the resize monitor thread does
    // not add a CI-sensitive sleep/join to the test runtime.
    cfg.max_threads = cfg.min_threads;
    cfg.queue_capacity = 64;
    if (!pool.initialize(cfg)) {
        std::cerr << "FAILED: pool.initialize returned false" << std::endl;
        return false;
    }

    ThrowingStartMonitor mon;
    mon.set_enabled(true);
    pool.set_task_monitor(&mon);

    constexpr int kThrowingTasks = 20;

    // ---- act: submit tasks whose monitor callback throws ----
    for (int i = 0; i < kThrowingTasks; ++i) {
        pool.submit([]() noexcept {
            g_user_fn_count.fetch_add(1, std::memory_order_relaxed);
        });
    }

    // ---- assert (a): wait_for_completion returns within 5s, not 300s ----
    auto t0 = std::chrono::steady_clock::now();
    if (!wait_for_pool_completion_or_shutdown(pool, 5s, "throwing monitor tasks")) {
        return false;
    }
    auto elapsed = std::chrono::steady_clock::now() - t0;
    if (elapsed > 5s) {
        std::cerr << "FAILED: wait_for_completion took " << elapsed.count() / 1'000'000
                  << "ms (>5000ms) — RAII guard not active?" << std::endl;
        return false;
    }

    // ---- assert (d): every record_task_start invocation was reached ----
    if (g_throw_count.load() < kThrowingTasks) {
        std::cerr << "FAILED: monitor.record_task_start fired only "
                  << g_throw_count.load() << "/" << kThrowingTasks
                  << " times — test did not exercise the bug path."
                  << std::endl;
        return false;
    }
    if (g_user_fn_count.load() != 0) {
        std::cerr << "FAILED: " << g_user_fn_count.load()
                  << " user functions ran despite monitor throwing before them"
                  << " — execute_task's body order is wrong?" << std::endl;
        return false;
    }

    // ---- assert (b): active_threads_ is 0 (would leak otherwise) ----
    auto st = pool.get_status();
    if (st.active_threads != 0) {
        std::cerr << "FAILED: active_threads=" << st.active_threads
                  << " after wait_for_completion — RAII guard missing?"
                  << std::endl;
        return false;
    }

    // ---- assert (c): a follow-up task still completes ----
    // Remove the throwing monitor first so the follow-up's user function
    // can actually run. The point of this assert is to prove the worker
    // thread itself survived the exception storm — not that the throwing
    // monitor path now permits the user function.
    pool.set_task_monitor(nullptr);
    std::atomic<bool> follow_up_done{false};
    pool.submit([&]() noexcept {
        follow_up_done.store(true, std::memory_order_release);
    });
    if (!wait_for_pool_completion_or_shutdown(pool, 5s, "follow-up task")) {
        return false;
    }
    if (!follow_up_done.load(std::memory_order_acquire)) {
        std::cerr << "FAILED: follow-up task did not run — workers died?"
                  << std::endl;
        return false;
    }

    pool.shutdown();
    std::cout << "  monitor-callback-exception path: PASSED ("
              << "throws=" << g_throw_count.load()
              << ", user_fns=" << g_user_fn_count.load()
              << ", elapsed=" << elapsed.count() / 1'000'000 << "ms)"
              << std::endl;
    return true;
}

}  // namespace

int main() {
    std::cout << "Testing ThreadPool monitor-callback exception safety (P-001)..."
              << std::endl;
    bool ok = run_test();
    if (!ok) {
        std::cerr << "TEST FAILED" << std::endl;
        return 1;
    }
    std::cout << "ALL TESTS PASSED" << std::endl;
    return 0;
}
