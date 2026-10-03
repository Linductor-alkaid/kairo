#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#include <kairo/executor.hpp>

using namespace kairo;

#define TEST_ASSERT(condition, message)                                      \
    do {                                                                    \
        if (!(condition)) {                                                  \
            std::cerr << "FAILED: " << message << " at " << __FILE__      \
                      << ":" << __LINE__ << std::endl;                     \
            return false;                                                    \
        }                                                                   \
    } while (0)

namespace {

ExecutorConfig small_config() {
    ExecutorConfig config;
    config.min_threads = 1;
    config.max_threads = 1;
    return config;
}

bool wait_until(std::chrono::milliseconds timeout, const std::function<bool()>& ready) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (ready()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return ready();
}

}  // namespace

bool test_periodic_exception_observable_without_future() {
    std::cout << "Testing periodic exception observable without future..."
              << std::endl;

    Executor executor;
    TEST_ASSERT(executor.initialize(small_config()), "executor should initialize");

    std::atomic<int> callback_count{0};
    executor.set_failure_callback([&callback_count](const ExecutorFailureEvent& event) {
        if (event.kind == FailureKind::TaskException) {
            callback_count.fetch_add(1, std::memory_order_relaxed);
        }
    });

    auto task_id = executor.submit_periodic(10, []() {
        throw std::runtime_error("periodic boom");
    });

    TEST_ASSERT(wait_until(std::chrono::seconds(2), [&executor, &task_id]() {
                    auto status = executor.get_periodic_task_status(task_id);
                    return status && status->failed_count >= 2;
                }),
                "periodic failures should become visible in periodic status");

    TEST_ASSERT(executor.cancel_task(task_id), "periodic task cancellation should succeed");
    executor.wait_for_completion();

    auto periodic_status = executor.get_periodic_task_status(task_id);
    TEST_ASSERT(!periodic_status, "cancelled periodic task should no longer be registered");

    auto failure_status = executor.get_failure_status();
    TEST_ASSERT(failure_status.task_exception_count >= 2,
                "periodic task exceptions should increment failure status");
    TEST_ASSERT(callback_count.load(std::memory_order_relaxed) >= 2,
                "periodic task exceptions should invoke callback");

    auto recent = executor.get_recent_failures(1);
    TEST_ASSERT(!recent.empty(), "recent failures should include periodic exception");
    TEST_ASSERT(recent[0].task_id == task_id,
                "periodic failure event should carry task id");

    executor.shutdown();

    std::cout << "  Periodic exception observable: PASSED" << std::endl;
    return true;
}

bool test_periodic_status_tracks_success_and_failure_streak() {
    std::cout << "Testing periodic status counters..." << std::endl;

    Executor executor;
    TEST_ASSERT(executor.initialize(small_config()), "executor should initialize");

    std::atomic<int> runs{0};
    auto task_id = executor.submit_periodic(10, [&runs]() {
        const int run = runs.fetch_add(1, std::memory_order_relaxed);
        if (run < 2) {
            throw std::runtime_error("initial periodic failure");
        }
    });

    TEST_ASSERT(wait_until(std::chrono::seconds(2), [&executor, &task_id]() {
                    auto status = executor.get_periodic_task_status(task_id);
                    return status && status->failed_count >= 2 &&
                           status->execution_count >= 3 &&
                           status->consecutive_failure_count == 0;
                }),
                "periodic status should track failures and reset streak after success");

    auto status = executor.get_periodic_task_status(task_id);
    TEST_ASSERT(status.has_value(), "periodic status should be queryable");
    TEST_ASSERT(status->failed_count >= 2,
                "periodic failed_count should include user exceptions");
    TEST_ASSERT(status->execution_count >= status->failed_count,
                "execution_count should include failures and successes");
    TEST_ASSERT(status->last_error_message.empty(),
                "successful run should clear last error message");

    auto all_statuses = executor.get_all_periodic_task_status();
    TEST_ASSERT(all_statuses.size() == 1, "all periodic statuses should include task");
    TEST_ASSERT(all_statuses[0].task_id == task_id,
                "all periodic status should preserve task id");

    TEST_ASSERT(executor.cancel_task(task_id), "periodic task cancellation should succeed");
    executor.shutdown();

    std::cout << "  Periodic status counters: PASSED" << std::endl;
    return true;
}

bool test_cancel_missing_periodic_task_is_diagnostic() {
    std::cout << "Testing missing periodic cancel diagnostic..." << std::endl;

    Executor executor;

    TEST_ASSERT(!executor.cancel_task("missing-periodic-task"),
                "cancel_task should return false for missing task");

    auto status = executor.get_failure_status();
    TEST_ASSERT(status.submit_rejected_count == 1,
                "missing cancel should record diagnostic rejection");

    auto recent = executor.get_recent_failures(1);
    TEST_ASSERT(recent.size() == 1, "missing cancel should produce recent event");
    TEST_ASSERT(recent[0].task_id == "missing-periodic-task",
                "missing cancel event should carry requested task id");

    std::cout << "  Missing periodic cancel diagnostic: PASSED" << std::endl;
    return true;
}

bool test_delayed_exception_observable() {
    std::cout << "Testing delayed task exception observable..." << std::endl;

    Executor executor;
    TEST_ASSERT(executor.initialize(small_config()), "executor should initialize");

    auto future = executor.submit_delayed(5, []() -> int {
        throw std::runtime_error("delayed boom");
    });

    bool future_threw = false;
    try {
        (void)future.get();
    } catch (const std::exception&) {
        future_threw = true;
    }
    TEST_ASSERT(future_threw, "delayed task future should carry exception");

    executor.wait_for_completion();

    auto failure_status = executor.get_failure_status();
    TEST_ASSERT(failure_status.task_exception_count == 1,
                "delayed exception should increment task exception count");

    auto async_status = executor.get_async_executor_status();
    TEST_ASSERT(async_status.failed_tasks == 1,
                "delayed exception should increment async failed_tasks");

    executor.shutdown();

    std::cout << "  Delayed task exception observable: PASSED" << std::endl;
    return true;
}

bool test_shutdown_marks_pending_delayed_task_failed() {
    std::cout << "Testing pending delayed task shutdown visibility..." << std::endl;

    Executor executor;
    TEST_ASSERT(executor.initialize(small_config()), "executor should initialize");

    auto future = executor.submit_delayed(5000, []() {
        return 7;
    });

    executor.shutdown();

    TEST_ASSERT(future.wait_for(std::chrono::seconds(0)) == std::future_status::ready,
                "pending delayed future should be completed on shutdown");

    // C1/T1 设计契约：shutdown 清理未到期 delayed 任务是生命周期事件，
    // 不再记 SubmitRejected failure，改以 TaskCancelled(Shutdown) 终态
    // future + 定时任务取消计数可观察。
    bool cancelled_by_shutdown = false;
    try {
        (void)future.get();
    } catch (const TaskCancelled& cancelled) {
        cancelled_by_shutdown =
            cancelled.reason() == TaskCancellationReason::Shutdown;
    } catch (const std::exception&) {
        cancelled_by_shutdown = false;
    }
    TEST_ASSERT(cancelled_by_shutdown,
                "pending delayed future should be cancelled with "
                "TaskCancelled(Shutdown)");

    auto failure_status = executor.get_failure_status();
    TEST_ASSERT(failure_status.submit_rejected_count == 0,
                "shutdown cancellation must not produce failure events");

    const auto timers = executor.get_timer_status_summary();
    TEST_ASSERT(timers.cancelled_count >= 1,
                "shutdown of pending delayed task should be observable in "
                "timer summary counters");

    std::cout << "  Pending delayed shutdown visibility: PASSED" << std::endl;
    return true;
}

int main() {
    bool all_passed = true;
    all_passed &= test_periodic_exception_observable_without_future();
    all_passed &= test_periodic_status_tracks_success_and_failure_streak();
    all_passed &= test_cancel_missing_periodic_task_is_diagnostic();
    all_passed &= test_delayed_exception_observable();
    all_passed &= test_shutdown_marks_pending_delayed_task_failed();

    if (all_passed) {
        std::cout << "All periodic failure observability tests passed."
                  << std::endl;
        return 0;
    }

    std::cerr << "Some periodic failure observability tests failed."
              << std::endl;
    return 1;
}
