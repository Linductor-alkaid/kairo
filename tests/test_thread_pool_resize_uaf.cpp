// test_thread_pool_resize_uaf.cpp
// Regression tests for P-260617-002: thread_pool resize UAF guard via
// shared_lock on local_queues_mutex_.
//
// 修复前: worker_thread 直接无锁访问 local_queues_[i].pop() / .size(),
// resize 重建 vector 元素时可能让 worker 读到悬空引用 (UAF)。
//
// 修复后: 所有 reader 路径 (worker_thread / try_steal_task / get_status /
// wait_for_completion) 持 shared_lock(local_queues_mutex_); writer 路径
// (resize_local_queues / shutdown 的 clear) 持 unique_lock 配对。
//
// 本测试在以下场景下断言:
//   1. 不会崩溃、不触发 data race (TSAN-friendly)
//   2. 任务计数对账: 提交 N -> 完成后 completed == N
//   3. resize 期间 worker 持续 pop/execute 不触发 UAF
//
// P-260629-001 后 TaskDispatcher 通过 shared_ptr slot 获取队列快照，支持
// resize 期间以原子指针交换替换整个 vector。

#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

#include "kairo/thread_pool/load_balancer.hpp"
#include "kairo/thread_pool/priority_scheduler.hpp"
#include "kairo/thread_pool/task_dispatcher.hpp"
#include "kairo/thread_pool/thread_pool.hpp"
#include "kairo/task/task.hpp"
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
// Test 1: 交替执行真实 worker 扩缩容，期间 worker 持续 pop/execute 任务。
// 每次 resize 都替换本地队列；所有 reader 与 writer 必须通过 shared_lock /
// unique_lock 配对，避免 worker 访问已替换队列时发生 UAF。
// ----------------------------------------------------------------------------
static bool test_resize_during_drain() {
    std::cout << "[P-002] resize_during_drain: concurrent submit + resize..."
              << std::endl;
    std::cout.flush();

    ThreadPoolConfig config;
    config.min_threads = 3;
    config.max_threads = 4;
    config.queue_capacity = 1024;
    config.enable_work_stealing = true;

    ThreadPool pool;
    TEST_ASSERT(pool.initialize(config), "pool init");

    std::atomic<bool> stop{false};
    std::atomic<int> completed{0};

    // 交替调整真实 worker 数，驱动本地队列替换与 worker 生命周期。
    size_t target_size = config.max_threads;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    for (int round = 0; round < 10; ++round) {
        std::cout << "  round " << round << " submit" << std::endl;
        std::cout.flush();
        for (int t = 0; t < 50; ++t) {
            pool.submit([&completed, round, t]() noexcept {
                std::this_thread::yield();
                completed.fetch_add(1, std::memory_order_relaxed);
            });
        }
        std::cout << "  round " << round << " resize" << std::endl;
        std::cout.flush();
        bool ok = pool.resize(target_size);
        TEST_ASSERT(ok, "resize should change the worker count");
        target_size = target_size == config.max_threads
                          ? config.min_threads
                          : config.max_threads;
    }
    std::cout << "  wait_for_completion..." << std::endl;
    std::cout.flush();

    pool.wait_for_completion();
    pool.shutdown(true);

    int done = completed.load();
    std::cout << "  resize_during_drain: completed=" << done << std::endl;
    TEST_ASSERT(done > 0, "at least some tasks must have completed");
    return true;
}

// ----------------------------------------------------------------------------
// Test 2: 多 worker 持续 steal 期间反复执行真实扩缩容。
// 每次队列替换都必须与 worker 的读取路径同步，避免 UAF。
// ----------------------------------------------------------------------------
static bool test_concurrent_steal_and_resize() {
    std::cout << "[P-002] concurrent_steal_and_resize: many workers steal "
                 "during resize..."
              << std::endl;

    ThreadPoolConfig config;
    config.min_threads = 5;
    config.max_threads = 6;
    config.queue_capacity = 2048;
    config.enable_work_stealing = true;

    ThreadPool pool;
    TEST_ASSERT(pool.initialize(config), "pool init");

    std::atomic<int> completed{0};
    const int total_rounds = 20;
    const int tasks_per_round = 60;

    for (int round = 0; round < total_rounds; ++round) {
        for (int i = 0; i < tasks_per_round; ++i) {
            pool.submit([&completed, round, i]() noexcept {
                std::this_thread::yield();
                std::this_thread::yield();
                completed.fetch_add(1, std::memory_order_relaxed);
            });
        }
        const size_t target_size = round % 2 == 0
                                       ? config.max_threads
                                       : config.min_threads;
        bool ok = pool.resize(target_size);
        TEST_ASSERT(ok, "resize should change the worker count");
    }

    pool.wait_for_completion();
    pool.shutdown(true);

    int done = completed.load();
    int expected = total_rounds * tasks_per_round;
    std::cout << "  concurrent_steal_and_resize: completed=" << done
              << " expected=" << expected << std::endl;
    TEST_ASSERT(done == expected,
                "all submitted tasks must complete (no UAF, no loss)");
    return true;
}

// ----------------------------------------------------------------------------
// Test 3: get_status() 与 resize 并发 — 验证 reader 路径也持锁。
// 修复前 get_status 内部对 local_queues_ 迭代无锁，与 resize 并发会
// 触发 TSAN data race。修复后 shared_lock 串行化。
// ----------------------------------------------------------------------------
static bool test_get_status_during_resize() {
    std::cout << "[P-002] get_status_during_resize: reader and writer "
                 "concurrently hit local_queues_..."
              << std::endl;

    ThreadPoolConfig config;
    config.min_threads = 1;
    config.max_threads = 2;
    config.queue_capacity = 512;
    config.enable_work_stealing = true;

    ThreadPool pool;
    TEST_ASSERT(pool.initialize(config), "pool init");

    std::atomic<bool> stop{false};
    std::thread reader([&]() {
        while (!stop.load(std::memory_order_acquire)) {
            (void)pool.get_status();
            // 读循环限速：get_status() 每次都持有 mutex_，零间隔循环在慢速
            // runner（TSan 放大）上会饿死等待同一把锁的 worker/resize 路径，
            // 曾把本用例拖到 CTest 180s 超时（master run 32096500953）。
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    std::thread resizer([&]() {
        for (int i = 0; i < 200 && !stop.load(); ++i) {
            const size_t target_size = i % 2 == 0
                                           ? config.max_threads
                                           : config.min_threads;
            (void)pool.resize(target_size);
        }
    });

    std::atomic<int> done{0};
    for (int i = 0; i < 200; ++i) {
        pool.submit([&done]() noexcept {
            done.fetch_add(1, std::memory_order_relaxed);
        });
    }

    resizer.join();
    pool.wait_for_completion();
    stop.store(true, std::memory_order_release);
    reader.join();
    pool.shutdown(true);

    std::cout << "  get_status_during_resize: tasks done=" << done.load()
              << std::endl;
    TEST_ASSERT(done.load() == 200, "all tasks must complete");
    return true;
}


// ----------------------------------------------------------------------------
// Stub queue type for P-260626-004 dispatch_batch num_workers=0 guard test.
//
// Real WorkerLocalQueue is non-movable (contains std::mutex), which makes
// constructing std::vector<WorkerLocalQueue> in test code painful.
//
// StubQueue implements only the minimum surface that TaskDispatcher<QueueT>
// touches.  This test replaces the queue slot with an empty vector before
// dispatch_batch acquires its shared lock, so no queue operation is reached.
// ----------------------------------------------------------------------------
struct StubQueue {
    bool push(const Task&) { return false; }
    size_t push_batch(const Task*, size_t n) { (void)n; return 0; }
    size_t push_batch_move(std::unique_ptr<Task>*, size_t n) { (void)n; return 0; }
    size_t size() const { return 0; }
    bool pop(Task&) { return false; }
    bool steal(Task&) { return false; }
    bool try_steal(Task&) { return false; }
};

// ----------------------------------------------------------------------------
// Test 4: dispatch_batch must return without dequeuing or indexing when the
// current local-queue slot contains an empty vector.
//
// Test strategy: ThreadPool::resize_local_queues(0) is rejected at the
// public API (returns false on size change), so this test bypasses that
// gate and constructs a TaskDispatcher<StubQueue> directly.  Main thread
// holds unique_lock(lq_mutex) and swaps in an empty vector; worker thread is
// blocked on shared_lock acquisition; once the unique_lock is released, the
// worker resumes with an empty queue snapshot.
// ----------------------------------------------------------------------------
static bool test_dispatch_batch_resize_zero_workers() {
    std::cout << "[P-260626-004] dispatch_batch empty-slot guard: "
                 "race pointer-swap under unique_lock vs dispatch_batch "
                 "shared_lock..." << std::endl;
    std::cout.flush();

    auto balancer_ptr = std::make_unique<LoadBalancer>(1);
    PriorityScheduler scheduler;

    auto queues = std::make_unique<std::vector<StubQueue>>();
    queues->emplace_back();

    std::shared_mutex lq_mutex;

    TaskDispatcher<StubQueue> dispatcher(
        *balancer_ptr, scheduler, &queues, &lq_mutex);

    // Pre-fill scheduler with 100 tasks.
    for (int i = 0; i < 100; ++i) {
        Task t;
        t.task_id = "p260626004-" + std::to_string(i);
        t.priority = TaskPriority::NORMAL;
        t.function = []() noexcept {};
        t.submit_time_ns = 0;
        t.timeout_ms = 0;
        scheduler.enqueue(t);
    }
    size_t pre_size = scheduler.size();
    TEST_ASSERT(pre_size == 100,
                "scheduler should hold 100 tasks before dispatch");

    // Main thread grabs unique_lock first; worker will block on shared_lock.
    std::unique_lock<std::shared_mutex> writer_lock(lq_mutex);

    std::atomic<bool> worker_done{false};
    size_t dispatch_return = std::numeric_limits<size_t>::max();

    std::thread worker([&]() {
        // Will block on shared_lock until writer releases; upon wakeup,
        // the local queue snapshot will be empty.
        size_t ret = dispatcher.dispatch_batch(100);
        dispatch_return = ret;
        worker_done.store(true, std::memory_order_release);
    });

    // Give worker thread time to reach the shared_lock acquisition.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::cout.flush();

    // Inside the unique_lock window: replace the local queue snapshot with
    // an empty vector under the writer lock, matching the real resize model
    // (PA-9: local_queues_ is a unique_ptr guarded by local_queues_mutex_).
    queues = std::make_unique<std::vector<StubQueue>>();
    std::cout << "  main thread: replaced local_queues_, size="
              << queues->size() << std::endl;
    std::cout.flush();

    writer_lock.unlock();  // Worker now acquires shared_lock, sees size==0
    worker.join();

    bool ok = worker_done.load(std::memory_order_acquire);
    TEST_ASSERT(ok, "worker thread must complete (no crash/UB)");

    std::cout << "  dispatch_batch return=" << dispatch_return << std::endl;
    TEST_ASSERT(dispatch_return == 0,
                "dispatch_batch should return 0 when num_workers==0");

    size_t post_size = scheduler.size();
    std::cout << "  scheduler pre=" << pre_size << " post=" << post_size
              << std::endl;
    TEST_ASSERT(post_size == 100,
                "empty queue snapshot must leave scheduler tasks untouched");

    // With the local queue snapshot permanently empty, repeated calls must
    // leave all tasks in the scheduler without crashing.
    for (int round = 0; round < 5; ++round) {
        size_t ret = dispatcher.dispatch_batch(100);
        TEST_ASSERT(ret == 0,
                    "subsequent dispatch_batch with empty queues must "
                    "return 0");
        TEST_ASSERT(scheduler.size() == 100,
                    "scheduler count must remain 100 across rounds "
                    "(no tasks lost)");
    }

    std::cout << "  test_dispatch_batch_resize_zero_workers: PASSED"
              << std::endl;
    return true;
}

int main() {
    std::cout << "=== P-260617-002 thread_pool resize UAF guard tests ==="
              << std::endl;

    bool all_ok = true;
    all_ok &= test_resize_during_drain();
    all_ok &= test_concurrent_steal_and_resize();
    all_ok &= test_get_status_during_resize();
    all_ok &= test_dispatch_batch_resize_zero_workers();

    if (all_ok) {
        std::cout << "\n=== All P-002 / P-260626-004 tests PASSED ==="
                  << std::endl;
        return 0;
    }
    std::cout << "\n=== Some tests FAILED ===" << std::endl;
    return 1;
}
