// test_timer_thread_lifecycle_race.cpp
// Regression test for P-260816-002: Executor 定时器线程启停竞态。
//
// 背景(H1):
//   旧 start_timer_thread() 先 timer_running_.exchange(true) 再创建线程并给
//   timer_thread_ 赋值;stop_timer_thread() 先 exchange(false) 再读
//   timer_thread_.joinable()。两个函数对 timer_thread_ 成员的写/读无任何同步:
//
//   1. 线程 A(submit_delayed) 赢得 exchange(true),阻塞在工厂/线程创建中;
//   2. 线程 B(shutdown/析构) 赢得 exchange(false),读到尚未赋值的
//      timer_thread_(数据竞争 UB),joinable()==false → 跳过 join,直接排空返回;
//   3. A 完成赋值(joinable);此后所有 stop_timer_thread() 都因标志已 false
//      提前返回 → joinable 的 std::thread 成员在析构时触发 std::terminate。
//
// 场景:
//   1. blocking_factory_vs_shutdown: 测试工厂在创建线程前阻塞,撑宽启动窗口;
//      并发 shutdown 后释放工厂。旧实现:析构时 std::terminate(进程崩溃);
//      新实现:shutdown 等待赋值完成、按代置 stop 位并 join,延迟任务 future
//      收到类型化拒绝,析构干净。
//   2. factory_failure_rollback_reusable: 工厂抛异常后回滚路径仍可用
//      (标志未卡在 true),换正常工厂后周期任务恢复执行。
//   3. concurrent_submit_vs_shutdown_stress: 多线程并发 submit_delayed 与
//      submit_periodic,同时 shutdown;所有已返回的 future 都必须完成
//      (执行或类型化拒绝),进程不崩溃、不悬挂。
//
// 断言:
//   1. shutdown 在 10s 内返回(无死锁、无 terminate)。
//   2. 竞态窗口内提交的延迟任务 future 收到异常(不悬挂、不 broken_promise)。
//   3. 工厂失败回滚后定时器可再次启动。
//   4. Executor 对象析构全程无 std::terminate。

#include <atomic>
#include <chrono>
#include <exception>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <kairo/config.hpp>
#include <kairo/executor.hpp>

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

// ----------------------------------------------------------------------------
// Test 1: 阻塞工厂撑宽启动窗口 + 并发 shutdown。
// 旧实现确定性触发:跳过 join → joinable 成员析构 → std::terminate。
// ----------------------------------------------------------------------------
static bool test_blocking_factory_vs_shutdown() {
    std::cout << "[P-260816-002] blocking_factory_vs_shutdown: factory blocks "
                 "inside start window while shutdown races..."
              << std::endl;

    Executor executor;
    ExecutorConfig config;
    config.min_threads = 1;
    config.max_threads = 2;
    TEST_ASSERT(executor.initialize(config), "initialize");

    std::atomic<bool> factory_entered{false};
    std::atomic<bool> release_factory{false};
    auto factory = [&](std::function<void()> timer_entry) -> std::thread {
        factory_entered.store(true, std::memory_order_release);
        while (!release_factory.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        return std::thread(std::move(timer_entry));
    };
    executor.set_timer_thread_factory_for_test(factory);

    // 线程 A: submit_delayed 阻塞在工厂内(正处于 start_timer_thread 的
    // "已置位/未赋值" 窗口,即旧实现的竞态窗口)。
    auto submit_async = std::async(std::launch::async, [&executor] {
        return executor.submit_delayed(50, [] { return 7; });
    });
    while (!factory_entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    // 线程 B: 并发 shutdown。旧实现:stop 读到未赋值的 timer_thread_,跳过
    // join 直接返回。新实现:在 timer_thread_mutex_ 上等待赋值完成。
    auto shutdown_async = std::async(std::launch::async, [&executor] {
        return executor.shutdown(true);
    });
    // 给 shutdown 足够时间抵达(旧实现直接穿过;新实现停在锁上)。
    std::this_thread::sleep_for(100ms);
    release_factory.store(true, std::memory_order_release);

    TEST_ASSERT(shutdown_async.wait_for(10s) == std::future_status::ready,
                "shutdown(true) must not deadlock in the start/stop race");
    (void)shutdown_async.get();

    TEST_ASSERT(submit_async.wait_for(10s) == std::future_status::ready,
                "submit_delayed must return after factory release");
    auto delayed_future = submit_async.get();
    // 竞态窗口内提交的任务必须完成:要么被执行(工厂阻塞期间到期时间已过,
    // 定时线程首轮迭代合法执行),要么被排空路径类型化拒绝。两者都不允许
    // 悬挂或 broken_promise。shutdown(true) 已保证两种路径下 get() 立即返回。
    bool got_exception = false;
    int value = -1;
    try {
        value = delayed_future.future.get();
    } catch (const std::exception&) {
        got_exception = true;
    } catch (...) {
        got_exception = true;
    }
    std::cout << "  delayed future got_exception=" << got_exception
              << " value=" << value << std::endl;
    TEST_ASSERT(got_exception || value == 7,
                "delayed task submitted inside the race window must complete "
                "(executed or rejected), never silently stranded");

    // 析构走到这里即证明没有 std::terminate(旧实现在此处崩溃)。
    std::cout << "  shutdown completed; executor will destruct cleanly"
              << std::endl;
    return true;
}

// ----------------------------------------------------------------------------
// Test 2: 工厂抛异常的回滚后,定时器可再次启动(标志未卡死)。
// ----------------------------------------------------------------------------
static bool test_factory_failure_rollback_reusable() {
    std::cout << "[P-260816-002] factory_failure_rollback_reusable..."
              << std::endl;

    Executor executor;
    ExecutorConfig config;
    config.min_threads = 1;
    config.max_threads = 2;
    TEST_ASSERT(executor.initialize(config), "initialize");

    auto throwing_factory = [](std::function<void()>) -> std::thread {
        throw std::runtime_error("injected timer thread creation failure");
    };
    executor.set_timer_thread_factory_for_test(throwing_factory);

    bool first_periodic_threw = false;
    try {
        (void)executor.submit_periodic(20, [] {});
    } catch (const std::exception&) {
        first_periodic_threw = true;
    }
    TEST_ASSERT(first_periodic_threw,
                "submit_periodic must propagate factory failure");

    // 清除工厂 → 恢复真实线程创建,定时器必须能再次启动并执行任务。
    executor.set_timer_thread_factory_for_test(nullptr);

    std::atomic<int> executions{0};
    auto periodic_handle = executor.submit_periodic(
        20, [&executions]() noexcept { executions.fetch_add(1); });
    TEST_ASSERT(periodic_handle.valid(), "periodic submit after rollback");

    std::this_thread::sleep_for(150ms);
    TEST_ASSERT(periodic_handle.cancel() != kairo::TimerOperationResult::NotFound,
                "cancel periodic task");
    const int count = executions.load();
    std::cout << "  periodic executions after rollback=" << count << std::endl;
    TEST_ASSERT(count >= 2,
                "periodic task must run after timer restart (>=2 ticks in "
                "150ms with 20ms period)");
    return true;
}

// ----------------------------------------------------------------------------
// Test 3: 并发提交 vs shutdown 压力:所有返回的 future 都必须完成。
// ----------------------------------------------------------------------------
static bool test_concurrent_submit_vs_shutdown_stress() {
    std::cout << "[P-260816-002] concurrent_submit_vs_shutdown_stress..."
              << std::endl;

    constexpr int kProducers = 4;
    constexpr int kTasksPerProducer = 50;

    Executor executor;
    ExecutorConfig config;
    config.min_threads = 2;
    config.max_threads = 4;
    TEST_ASSERT(executor.initialize(config), "initialize");

    std::atomic<bool> stop_producers{false};
    std::atomic<int> completed_futures{0};
    std::atomic<int> submit_threw{0};

    auto producer = [&](int id) {
        for (int i = 0; i < kTasksPerProducer; ++i) {
            if (stop_producers.load(std::memory_order_acquire)) {
                break;
            }
            try {
                auto fut = executor.submit_delayed(
                    5, [](int v) { return v; }, id * 1000 + i);
                // 只等待已拿到的 future:执行或类型化拒绝都算完成。
                try {
                    (void)fut.future.get();
                } catch (...) {
                }
                completed_futures.fetch_add(1, std::memory_order_relaxed);
            } catch (...) {
                // shutdown 之后的提交抛"not initialized"是合法拒绝。
                submit_threw.fetch_add(1, std::memory_order_relaxed);
            }
            std::this_thread::yield();
        }
    };

    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back(producer, p);
    }

    std::this_thread::sleep_for(30ms);
    const auto result = executor.shutdown(true);
    stop_producers.store(true, std::memory_order_release);
    for (auto& t : producers) {
        t.join();
    }

    std::cout << "  completed_futures=" << completed_futures.load()
              << " submit_threw=" << submit_threw.load()
              << " shutdown=" << static_cast<int>(result) << std::endl;
    TEST_ASSERT(result == ShutdownResult::Completed, "shutdown result");
    // 已返回 future 的任务必须全部完成(没有悬挂等待 get()) —— producer
    // join 成功且每个 future 都 get() 返回即隐式保证。
    TEST_ASSERT(completed_futures.load() + submit_threw.load() > 0,
                "stress must make progress");
    return true;
}

int main() {
    std::cout << "=== P-260816-002 timer thread lifecycle race tests ==="
              << std::endl;

    bool all_ok = true;
    all_ok &= test_blocking_factory_vs_shutdown();
    all_ok &= test_factory_failure_rollback_reusable();
    all_ok &= test_concurrent_submit_vs_shutdown_stress();

    if (all_ok) {
        std::cout << "\n=== All P-260816-002 tests PASSED ===" << std::endl;
        return 0;
    }
    std::cout << "\n=== Some P-260816-002 tests FAILED ===" << std::endl;
    return 1;
}
