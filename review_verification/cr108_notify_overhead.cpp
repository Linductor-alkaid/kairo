// CR-108 (P2, performance): execute_task() fires TWO wake-ups per task on the
// completion path — update_statistics() -> notify_completion_waiters()
// (thread_pool.cpp:422-434) plus ActiveCounter dtor ->
// notify_completion_waiters() (thread_pool.hpp:319-335). Both are
// completion_cv_.notify_all().
//
// With zero waiters a notify_all is nearly free; with N threads parked in
// try_wait_for_completion() each notify_all wakes them all per task.
// Measurement: submit+future.get round trips of an empty task on a
// single-worker pool, best of 3 runs, no waiters vs 8 waiter threads.
//
// NOTE: waiters use try_wait_for_completion which also calls
// dispatch_pending_tasks() between slices, so the delta is an upper bound of
// the notify cost (includes lock contention from the waiting path itself).
#include "kairo/thread_pool/thread_pool.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <thread>
#include <vector>

using namespace kairo;
namespace chrono = std::chrono;

static double bench_once(ThreadPool& pool, int n) {
    const auto t0 = chrono::steady_clock::now();
    for (int i = 0; i < n; ++i) {
        pool.submit([] {}).get();
    }
    const auto t1 = chrono::steady_clock::now();
    return n / chrono::duration<double>(t1 - t0).count();
}

static double bench_best_of_3(ThreadPool& pool, int n, const char* label) {
    double best = 0.0;
    for (int r = 0; r < 3; ++r) {
        double ops = bench_once(pool, n);
        if (ops > best) best = ops;
        printf("  [%s] run %d: %.0f ops/s\n", label, r, ops);
    }
    return best;
}

int main() {
    setbuf(stdout, nullptr);

    ThreadPool pool;
    ThreadPoolConfig config;
    config.min_threads = 1;
    config.max_threads = 1;
    config.queue_capacity = 1000;
    if (!pool.initialize(config)) {
        printf("FATAL: initialize failed\n");
        return 2;
    }

    const int kN = 100000;
    printf("warming up...\n");
    bench_once(pool, 2000);

    printf("phase 1: no waiters, %d submit+get round trips\n", kN);
    double ops_none = bench_best_of_3(pool, kN, "no-waiters");

    printf("phase 2: 8 waiter threads spinning on try_wait_for_completion(1ms)\n");
    std::atomic<bool> run{true};
    std::vector<std::thread> waiters;
    for (int w = 0; w < 8; ++w) {
        waiters.emplace_back([&pool, &run]() {
            while (run.load(std::memory_order_relaxed)) {
                pool.try_wait_for_completion(chrono::milliseconds(1));
            }
        });
    }
    std::this_thread::sleep_for(chrono::milliseconds(300));
    double ops_waiters = bench_best_of_3(pool, kN, "8-waiters");
    run.store(false);
    for (auto& t : waiters) t.join();

    double delta = (ops_none - ops_waiters) / ops_none * 100.0;
    printf("best ops/s: no-waiters=%.0f, 8-waiters=%.0f, delta=%.1f%%\n",
           ops_none, ops_waiters, delta);
    printf("CR-108 verdict: %s\n",
           delta >= 15.0
               ? "MEASURABLE (>=15% throughput drop with waiters; notify_all "
                 "cost visible end-to-end)"
               : "MINOR IMPACT (<15% delta; per-task notify_all cost not "
                 "significant end-to-end)");
    return 0;
}
