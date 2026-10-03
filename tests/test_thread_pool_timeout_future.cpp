#include <atomic>
#include <chrono>
#include <exception>
#include <future>
#include <iostream>
#include <limits>
#include <string>
#include <thread>

#include <kairo/config.hpp>
#include <kairo/executor.hpp>
#include <kairo/monitor/task_monitor.hpp>
#include <kairo/thread_pool/thread_pool.hpp>
#include <kairo/types.hpp>

using namespace kairo;

#define TEST_ASSERT(condition, message)                                      \
    do {                                                                     \
        if (!(condition)) {                                                   \
            std::cerr << "FAILED: " << message << " at " << __FILE__       \
                      << ":" << __LINE__ << std::endl;                       \
            return false;                                                     \
        }                                                                    \
    } while (0)

namespace {

// CI（尤其 Windows runner）负载下，任务从提交到被 worker 出队可能超过
// 原 20ms 裕度：blocker 自身被软超时后 future.get() 抛 TimedOutException，
// 经 TEST_ASSERT 之外的路径传播为 terminate/MSVC fail-fast(0xc0000409)。
// 断言改为异常安全并给出诊断；超时/阻塞裕度放宽 5 倍，被测语义
//（排队任务在 worker 忙碌期间过期）不变。
bool future_returns(std::future<int>& future, int expected, const char* what) {
    try {
        const int value = future.get();
        if (value == expected) return true;
        std::cerr << what << " returned " << value << ", expected " << expected
                  << std::endl;
        return false;
    } catch (const std::exception& ex) {
        std::cerr << what << " threw unexpected exception: " << ex.what()
                  << std::endl;
        return false;
    }
}

// 确定性启动门：blocker 任务体首条语句置位 started（release），主线程有界
// 等待（acquire）其置位后再提交后续任务。观察到置位即证明 worker 已将
// blocker 出队并开始执行——软超时仅在出队后的执行前检查一次（见
// thread_pool.cpp），运行中的任务不会被中断，因此 blocker 此后必然正常
// 完成；而其后提交的任务必然排队等待 blocker 剩余 ~300ms，远超 100ms
// 预算，必然被软超时跳过。固定 sleep 无法提供该保证：负载下 worker 出队
// 延迟可超过任务自身的超时预算，导致 blocker 在出队时即被跳过。
// 有界等待防止异常路径下主线程挂死；超时按断言失败处理。
bool wait_for_started(const std::atomic<bool>& started,
                      std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!started.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

bool future_throws_timed_out(std::future<int>& future) {
    try {
        (void)future.get();
    } catch (const TimedOutException& ex) {
        const std::string message = ex.what();
        return message.find("Task timed out after ") != std::string::npos;
    } catch (const std::future_error& ex) {
        std::cerr << "future.get() threw std::future_error instead: "
                  << ex.what() << std::endl;
        return false;
    } catch (const std::exception& ex) {
        std::cerr << "future.get() threw unexpected exception: "
                  << ex.what() << std::endl;
        return false;
    }
    std::cerr << "future.get() did not throw" << std::endl;
    return false;
}

bool test_thread_pool_timeout_satisfies_future_and_monitor() {
    std::cout << "Testing ThreadPool timeout future and monitor stats..."
              << std::endl;

    ThreadPool pool;
    ThreadPoolConfig config;
    config.min_threads = 1;
    config.max_threads = 1;
    config.task_timeout_ms = 100;
    TEST_ASSERT(pool.initialize(config), "thread pool should initialize");

    monitor::TaskMonitor monitor;
    pool.set_task_monitor(&monitor);

    std::atomic<bool> blocker_started{false};
    auto blocker = pool.submit([&blocker_started]() {
        blocker_started.store(true, std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        return 1;
    });

    TEST_ASSERT(
        wait_for_started(blocker_started, std::chrono::milliseconds(10000)),
        "blocker task should be picked up by a worker");

    std::atomic<bool> timed_out_task_ran{false};
    auto timed_out = pool.submit([&timed_out_task_ran]() {
        timed_out_task_ran.store(true, std::memory_order_release);
        return 7;
    });

    TEST_ASSERT(future_returns(blocker, 1, "blocker task"),
                "blocker task should complete normally");
    TEST_ASSERT(future_throws_timed_out(timed_out),
                "timed-out ThreadPool future should throw TimedOutException");

    pool.wait_for_completion();

    TEST_ASSERT(!timed_out_task_ran.load(std::memory_order_acquire),
                "timed-out task function should not run");
    TEST_ASSERT(pool.get_timeout_count() == 1,
                "ThreadPool timeout_count should be 1");

    auto stats = monitor.get_statistics("default");
    TEST_ASSERT(stats.timeout_count == 1,
                "TaskMonitor timeout_count should be 1");
    TEST_ASSERT(stats.fail_count == 0,
                "TaskMonitor fail_count should not include timeout");

    pool.shutdown();

    std::cout << "  ThreadPool timeout future and monitor stats: PASSED"
              << std::endl;
    return true;
}

bool test_executor_timeout_satisfies_future_and_failure_status() {
    std::cout << "Testing Executor timeout future and failure status..."
              << std::endl;

    Executor executor;
    ExecutorConfig config;
    config.min_threads = 1;
    config.max_threads = 1;
    config.task_timeout_ms = 100;
    TEST_ASSERT(executor.initialize(config), "executor should initialize");

    std::atomic<bool> blocker_started{false};
    auto blocker = executor.submit([&blocker_started]() {
        blocker_started.store(true, std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        return 1;
    });

    TEST_ASSERT(
        wait_for_started(blocker_started, std::chrono::milliseconds(10000)),
        "facade blocker should be picked up by a worker");

    std::atomic<bool> timed_out_task_ran{false};
    auto timed_out = executor.submit([&timed_out_task_ran]() {
        timed_out_task_ran.store(true, std::memory_order_release);
        return 9;
    });

    TEST_ASSERT(future_returns(blocker, 1, "facade blocker"),
                "facade blocker should complete normally");
    TEST_ASSERT(future_throws_timed_out(timed_out),
                "timed-out Executor future should throw TimedOutException");

    (void)executor.wait_for_completion(std::chrono::seconds{300});

    TEST_ASSERT(!timed_out_task_ran.load(std::memory_order_acquire),
                "timed-out facade task function should not run");

    auto failure_status = executor.get_failure_status();
    TEST_ASSERT(failure_status.timeout_count == 1,
                "ExecutorFailureStatus timeout_count should be 1");
    TEST_ASSERT(failure_status.task_exception_count == 0,
                "timeout should not increment task_exception_count");

    auto async_status = executor.get_async_executor_status();
    TEST_ASSERT(async_status.failed_tasks == 0,
                "timeout should not increment async failed_tasks");

    executor.shutdown();

    std::cout << "  Executor timeout future and failure status: PASSED"
              << std::endl;
    return true;
}

bool test_extreme_timeout_does_not_overflow() {
    std::cout << "Testing ThreadPool extreme timeout saturation..." << std::endl;

    ThreadPool pool;
    ThreadPoolConfig config;
    config.min_threads = 1;
    config.max_threads = 1;
    config.task_timeout_ms = std::numeric_limits<int64_t>::max() / 2;
    TEST_ASSERT(pool.initialize(config), "thread pool should initialize");

    std::atomic<bool> task_ran{false};
    auto result = pool.submit([&task_ran]() {
        task_ran.store(true, std::memory_order_release);
        return 42;
    });

    TEST_ASSERT(result.get() == 42, "extreme-timeout task should complete");
    pool.wait_for_completion();
    TEST_ASSERT(task_ran.load(std::memory_order_acquire),
                "extreme-timeout task function should run");
    TEST_ASSERT(pool.get_timeout_count() == 0,
                "extreme timeout should not increment timeout_count");

    pool.shutdown();

    std::cout << "  ThreadPool extreme timeout saturation: PASSED" << std::endl;
    return true;
}

}  // namespace

int main() {
    bool all_passed = true;
    all_passed &= test_thread_pool_timeout_satisfies_future_and_monitor();
    all_passed &= test_executor_timeout_satisfies_future_and_failure_status();
    all_passed &= test_extreme_timeout_does_not_overflow();

    if (all_passed) {
        std::cout << "All thread pool timeout future tests passed."
                  << std::endl;
        return 0;
    }

    std::cerr << "Some thread pool timeout future tests failed." << std::endl;
    return 1;
}
