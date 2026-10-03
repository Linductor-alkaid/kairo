// test_shutdown_drain_reentrancy.cpp
// Regression test for P-260816-001: ExecutorManager::shutdown() 曾在持有
// default_async_mutex_ 的情况下执行默认执行器的阻塞排空
// (stop(wait_for_tasks) / wait_for_completion() 内含 worker join,可阻塞
// 数秒)。而 submit()/get_default_async_executor_snapshot()/状态查询等读
// 路径都要拿同一把锁 —— 池内任务在排空期间再入这些读路径时,会与
// shutdown 互相等待形成自死锁:
//
//   shutdown 线程: 持 default_async_mutex_ -> stop(true) -> 等待任务排空
//   worker 任务:   调 is_default_async_shutdown()/snapshot() -> 阻塞在
//                  default_async_mutex_ 上 -> 永远不会结束
//   => 排空永远等不到任务完成。
//
// 场景:
//   1. reentrant_read_during_drain: 池内任务持续轮询
//      is_default_async_shutdown() (即反复获取 default_async_mutex_),
//      主线程并发 shutdown(true)。旧实现在此确定性死锁;新实现中任务
//      快速观察到 shutdown 闩并退出,排空正常完成。
//   2. snapshot_rejected_after_latch: 任务观察到闩后,snapshot() 立即返回
//      nullptr (走拒绝分支,而非阻塞等待排空结束)。
//   3. concurrent_double_shutdown: 两个线程同时 shutdown(true),第二个
//      调用者等待第一个排空结束后才返回,不出现双重排空/双重 join。
//   4. drain_executes_accepted_task: shutdown(true) 排空语义回归 —— 已
//      接受的任务在关停前仍被执行。
//
// 断言:
//   1. shutdown(true) 不死锁(<= 10s 完成,与 P-260626-002 测试同规格)。
//   2. 排空期间池内任务可以完成 default_async_mutex_ 读路径调用。
//   3. 并发双 shutdown 均正常返回且状态一致。
//   4. shutdown 后 snapshot() == nullptr、has_default_async_executor() ==
//      false、is_default_async_shutdown() == true。

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <thread>

#include <kairo/config.hpp>
#include <kairo/executor_manager.hpp>

using namespace kairo;
using namespace std::chrono_literals;

#define TEST_ASSERT(condition, message)                                       \
    do {                                                                      \
        if (!(condition)) {                                                   \
            std::cerr << "FAILED: " << message << " at " << __FILE__ << ":"   \
                      << __LINE__ << std::endl;                               \
            return false;                                                     \
        }                                                                     \
    } while (0)

// 独立的 ExecutorManager 实例(实例化模式),避免单例路径的 atexit 干扰。
static std::unique_ptr<ExecutorManager> make_manager_with_pool() {
    auto manager = std::make_unique<ExecutorManager>();
    ExecutorConfig config{};
    config.min_threads = 2;
    config.max_threads = 2;
    if (!manager->initialize_async_executor(config)) {
        std::cerr << "FAILED: initialize_async_executor at " << __FILE__ << ":"
                  << __LINE__ << std::endl;
        return nullptr;
    }
    return manager;
}

// ----------------------------------------------------------------------------
// Test 1 + 2: 池内任务在排空期间再入 default_async_mutex_ 读路径。
// 旧实现(P-260816-001 之前): 任务阻塞在互斥锁上 -> 排空等不到任务 -> 死锁。
// 新实现: 任务快速观察到闩,snapshot() 返回 nullptr,shutdown 正常完成。
// ----------------------------------------------------------------------------
static bool test_reentrant_read_during_drain() {
    std::cout << "[P-260816-001] reentrant_read_during_drain: pool task "
                 "polls default_async_mutex_ readers during shutdown(true)..."
              << std::endl;

    auto manager = make_manager_with_pool();
    auto executor_snapshot = manager->get_default_async_executor_snapshot();
    TEST_ASSERT(executor_snapshot != nullptr, "default executor snapshot");

    std::atomic<bool> polling_started{false};
    std::atomic<bool> observed_shutdown_latch{false};
    std::atomic<bool> snapshot_rejected_after_latch{false};

    // 任务体: 反复走 is_default_async_shutdown()/snapshot() 这两条持锁读路径。
    // 这正是 C1 死锁中 worker 侧的行为(等价于任务内再入 submit())。
    auto task = [&]() {
        polling_started.store(true, std::memory_order_release);
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (std::chrono::steady_clock::now() < deadline) {
            if (manager->is_default_async_shutdown()) {
                observed_shutdown_latch.store(true, std::memory_order_release);
                // 观察到闩之后再走一次 snapshot(): 必须立即拿到 nullptr
                // (拒绝分支),而不是阻塞等排空结束。
                snapshot_rejected_after_latch.store(
                    manager->get_default_async_executor_snapshot() == nullptr,
                    std::memory_order_release);
                return;
            }
            (void)manager->get_default_async_executor_snapshot();
            std::this_thread::yield();
        }
    };
    auto task_future = executor_snapshot->submit(std::move(task));

    // 确保任务已在轮询中(100% 命中排空窗口),再触发 shutdown。
    while (!polling_started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    auto shutdown_future = std::async(std::launch::async,
                                      [&manager]() { manager->shutdown(true); });
    if (shutdown_future.wait_for(10s) != std::future_status::ready) {
        std::cerr << "FATAL: shutdown(true) 与池内持锁读路径死锁,超过 10s "
                     "未返回 (P-260816-001 回归)"
                  << std::endl;
        return false;
    }
    shutdown_future.get();
    task_future.wait_for(10s);

    std::cout << "  observed_shutdown_latch="
              << observed_shutdown_latch.load()
              << " snapshot_rejected_after_latch="
              << snapshot_rejected_after_latch.load() << std::endl;

    TEST_ASSERT(observed_shutdown_latch.load(),
                "pool task must observe the shutdown latch (not block on "
                "default_async_mutex_)");
    TEST_ASSERT(snapshot_rejected_after_latch.load(),
                "snapshot() after latch must return nullptr immediately");
    TEST_ASSERT(manager->is_default_async_shutdown(),
                "latch must be set after shutdown");
    TEST_ASSERT(!manager->has_default_async_executor(),
                "external shutdown must release the default executor");
    return true;
}

// ----------------------------------------------------------------------------
// Test 3: 两线程并发 shutdown(true)。第二个调用者必须等第一个排空结束后
// 才返回(不出现并发双重排空/双重 join),且最终状态一致。
// ----------------------------------------------------------------------------
static bool test_concurrent_double_shutdown() {
    std::cout << "[P-260816-001] concurrent_double_shutdown: two threads "
                 "call shutdown(true) simultaneously..."
              << std::endl;

    auto manager = make_manager_with_pool();
    auto executor_snapshot = manager->get_default_async_executor_snapshot();
    TEST_ASSERT(executor_snapshot != nullptr, "default executor snapshot");

    std::atomic<int> completed{0};
    // 已接受的任务: 验证排空语义仍生效(第一个 shutdown 排空它)。
    auto task_future = executor_snapshot->submit(
        [&completed]() { completed.fetch_add(1, std::memory_order_relaxed); });

    auto shutdown_one = std::async(std::launch::async,
                                   [&manager]() { manager->shutdown(true); });
    auto shutdown_two = std::async(std::launch::async,
                                   [&manager]() { manager->shutdown(true); });

    TEST_ASSERT(shutdown_one.wait_for(10s) == std::future_status::ready,
                "first shutdown(true) must complete within 10s");
    TEST_ASSERT(shutdown_two.wait_for(10s) == std::future_status::ready,
                "second concurrent shutdown(true) must complete within 10s");
    shutdown_one.get();
    shutdown_two.get();
    task_future.wait_for(10s);

    std::cout << "  completed=" << completed.load() << std::endl;
    TEST_ASSERT(completed.load() == 1,
                "accepted task must run exactly once during drain");
    TEST_ASSERT(manager->is_default_async_shutdown(),
                "latch must be set after double shutdown");
    TEST_ASSERT(!manager->has_default_async_executor(),
                "executor must be released after double shutdown");
    // shutdown 之后再次调用: 幂等,立即返回,不再有无谓排空。
    const auto result = manager->shutdown(true);
    TEST_ASSERT(result == ShutdownResult::Completed,
                "post-shutdown shutdown(true) must be idempotent Completed");
    return true;
}

// ----------------------------------------------------------------------------
// Test 4: shutdown(true) 排空语义回归 —— 已接受任务在关停前被执行。
// ----------------------------------------------------------------------------
static bool test_drain_executes_accepted_task() {
    std::cout << "[P-260816-001] drain_executes_accepted_task..." << std::endl;

    auto manager = make_manager_with_pool();
    auto executor_snapshot = manager->get_default_async_executor_snapshot();
    TEST_ASSERT(executor_snapshot != nullptr, "default executor snapshot");

    std::atomic<int> completed{0};
    for (int i = 0; i < 32; ++i) {
        executor_snapshot->submit(
            [&completed]() { completed.fetch_add(1, std::memory_order_relaxed); });
    }

    const auto result = manager->shutdown(true);
    TEST_ASSERT(result == ShutdownResult::Completed, "shutdown result");
    std::cout << "  completed=" << completed.load() << "/32" << std::endl;
    TEST_ASSERT(completed.load() == 32,
                "all accepted tasks must complete before drain returns");
    return true;
}

int main() {
    std::cout << "=== P-260816-001 shutdown drain reentrancy tests ==="
              << std::endl;

    bool all_ok = true;
    all_ok &= test_reentrant_read_during_drain();
    all_ok &= test_concurrent_double_shutdown();
    all_ok &= test_drain_executes_accepted_task();

    if (all_ok) {
        std::cout << "\n=== All P-260816-001 tests PASSED ===" << std::endl;
        return 0;
    }
    std::cout << "\n=== Some P-260816-001 tests FAILED ===" << std::endl;
    return 1;
}
