#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
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

RealtimeThreadConfig make_rt_config(const std::string& thread_name,
                                    int64_t period_ns = 5'000'000) {
    RealtimeThreadConfig config;
    config.thread_name = thread_name;
    config.cycle_period_ns = period_ns;
    config.thread_priority = 0;
    config.enable_process_memory_lock = false;
    config.timer_slack_ns = 0;
    config.cycle_callback = []() {};
    return config;
}

bool wait_until(std::function<bool()> predicate,
                std::chrono::milliseconds timeout = std::chrono::milliseconds(500)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

}  // namespace

static bool test_facade_push_success() {
    std::cout << "Testing realtime facade push success..." << std::endl;

    Executor executor;
    TEST_ASSERT(executor.register_realtime_task(
                    "rt_success", make_rt_config("rt_success_thread")),
                "realtime task should register");
    TEST_ASSERT(executor.start_realtime_task("rt_success"),
                "realtime task should start");

    std::atomic<int> ran{0};
    TEST_ASSERT(executor.push_realtime_task("rt_success", [&ran]() {
                    ran.fetch_add(1, std::memory_order_relaxed);
                }),
                "facade push should accept task");

    TEST_ASSERT(wait_until([&ran]() {
                    return ran.load(std::memory_order_relaxed) == 1;
                }),
                "facade-pushed realtime task should run");

    auto failure_status = executor.get_failure_status();
    TEST_ASSERT(failure_status.realtime_drop_count == 0,
                "successful realtime push should not record drops");

    executor.stop_realtime_task("rt_success");

    std::cout << "  Realtime facade push success: PASSED" << std::endl;
    return true;
}

static bool test_missing_realtime_executor_push_is_visible() {
    std::cout << "Testing missing realtime facade push visibility..." << std::endl;

    Executor executor;
    TEST_ASSERT(!executor.try_push_realtime_task("missing_rt", []() {}),
                "push to missing realtime executor should return false");

    auto failure_status = executor.get_failure_status();
    TEST_ASSERT(failure_status.submit_rejected_count == 1,
                "missing realtime executor should count as submit rejection");
    TEST_ASSERT(failure_status.realtime_drop_count == 0,
                "missing realtime executor should not be counted as queue drop");

    auto recent = executor.get_recent_failures(1);
    TEST_ASSERT(recent.size() == 1, "missing realtime push should record event");
    TEST_ASSERT(recent[0].kind == FailureKind::SubmitRejected,
                "missing realtime push event should be SubmitRejected");

    std::cout << "  Missing realtime facade push visibility: PASSED" << std::endl;
    return true;
}

static bool test_not_running_realtime_push_is_visible() {
    std::cout << "Testing not-running realtime facade push visibility..." << std::endl;

    Executor executor;
    TEST_ASSERT(executor.register_realtime_task(
                    "rt_not_running", make_rt_config("rt_not_running_thread")),
                "realtime task should register");

    TEST_ASSERT(!executor.push_realtime_task("rt_not_running", []() {}),
                "push before start should return false");

    auto rt_status = executor.get_realtime_executor_status("rt_not_running");
    TEST_ASSERT(rt_status.dropped_task_count == 1,
                "not-running push should increment dropped_task_count");
    TEST_ASSERT(rt_status.rejected_not_running_count == 1,
                "not-running push should increment reason counter");

    auto failure_status = executor.get_failure_status();
    TEST_ASSERT(failure_status.realtime_drop_count == 1,
                "not-running push should record realtime drop");

    auto recent = executor.get_recent_failures(1);
    TEST_ASSERT(recent.size() == 1, "not-running push should record event");
    TEST_ASSERT(recent[0].kind == FailureKind::RealtimeDrop,
                "not-running push event should be RealtimeDrop");
    TEST_ASSERT(recent[0].message.find("not running") != std::string::npos,
                "not-running push event should explain reason");

    std::cout << "  Not-running realtime facade push visibility: PASSED"
              << std::endl;
    return true;
}

static bool test_stopped_realtime_push_is_visible() {
    std::cout << "Testing stopped realtime facade push visibility..." << std::endl;

    Executor executor;
    TEST_ASSERT(executor.register_realtime_task(
                    "rt_stopped", make_rt_config("rt_stopped_thread")),
                "realtime task should register");
    TEST_ASSERT(executor.start_realtime_task("rt_stopped"),
                "realtime task should start");
    executor.stop_realtime_task("rt_stopped");

    TEST_ASSERT(!executor.try_push_realtime_task("rt_stopped", []() {}),
                "push after stop should return false");

    auto rt_status = executor.get_realtime_executor_status("rt_stopped");
    TEST_ASSERT(rt_status.dropped_task_count >= 1,
                "stopped push should increment dropped_task_count");
    TEST_ASSERT(rt_status.rejected_not_running_count >= 1,
                "stopped push should increment not-running reason counter");

    auto failure_status = executor.get_failure_status();
    TEST_ASSERT(failure_status.realtime_drop_count >= 1,
                "stopped push should record realtime drop");

    std::cout << "  Stopped realtime facade push visibility: PASSED"
              << std::endl;
    return true;
}

static bool test_empty_realtime_push_is_visible() {
    std::cout << "Testing empty realtime facade push visibility..." << std::endl;

    Executor executor;
    TEST_ASSERT(executor.register_realtime_task(
                    "rt_empty", make_rt_config("rt_empty_thread")),
                "realtime task should register");
    TEST_ASSERT(executor.start_realtime_task("rt_empty"),
                "realtime task should start");

    TEST_ASSERT(!executor.push_realtime_task("rt_empty", std::function<void()>{}),
                "empty realtime task should return false");

    auto rt_status = executor.get_realtime_executor_status("rt_empty");
    TEST_ASSERT(rt_status.dropped_task_count == 1,
                "empty push should increment dropped_task_count");
    TEST_ASSERT(rt_status.rejected_empty_task_count == 1,
                "empty push should increment empty-task reason counter");

    auto failure_status = executor.get_failure_status();
    TEST_ASSERT(failure_status.realtime_drop_count == 1,
                "empty push should record realtime drop");

    executor.stop_realtime_task("rt_empty");

    std::cout << "  Empty realtime facade push visibility: PASSED" << std::endl;
    return true;
}

static bool test_queue_full_realtime_push_is_visible() {
    std::cout << "Testing queue-full realtime facade push visibility..." << std::endl;

    std::atomic<bool> block_cycle{true};
    RealtimeThreadConfig config = make_rt_config("rt_queue_full_thread", 1'000'000'000);
    config.cycle_callback = [&block_cycle]() {
        while (block_cycle.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    };

    Executor executor;
    TEST_ASSERT(executor.register_realtime_task("rt_queue_full", config),
                "realtime task should register");
    TEST_ASSERT(executor.start_realtime_task("rt_queue_full"),
                "realtime task should start");

    TEST_ASSERT(wait_until([&executor]() {
                    return executor.get_realtime_executor_status("rt_queue_full").cycle_count == 0;
                }, std::chrono::milliseconds(5)),
                "initial cycle state should be readable");

    bool saw_drop = false;
    for (int i = 0; i < 2000; ++i) {
        if (!executor.try_push_realtime_task("rt_queue_full", []() {})) {
            saw_drop = true;
            break;
        }
    }

    block_cycle.store(false, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    TEST_ASSERT(saw_drop, "facade push should return false once queue is full");

    auto rt_status = executor.get_realtime_executor_status("rt_queue_full");
    TEST_ASSERT(rt_status.dropped_task_count > 0,
                "queue-full push should increment dropped_task_count");
    TEST_ASSERT(rt_status.queue_full_count > 0 || rt_status.pool_exhausted_count > 0,
                "queue-full/pool-exhausted reason counter should increment");

    auto failure_status = executor.get_failure_status();
    TEST_ASSERT(failure_status.realtime_drop_count > 0,
                "queue-full push should record realtime drop events");

    executor.stop_realtime_task("rt_queue_full");

    std::cout << "  Queue-full realtime facade push visibility: PASSED"
              << std::endl;
    return true;
}

int main() {
    std::cout << "========== Realtime facade push observability tests ==========\n\n";

    bool all_passed = true;
    all_passed &= test_facade_push_success();
    all_passed &= test_missing_realtime_executor_push_is_visible();
    all_passed &= test_not_running_realtime_push_is_visible();
    all_passed &= test_stopped_realtime_push_is_visible();
    all_passed &= test_empty_realtime_push_is_visible();
    all_passed &= test_queue_full_realtime_push_is_visible();

    std::cout << "\n============================================================\n";
    if (all_passed) {
        std::cout << "All realtime facade push tests PASSED!" << std::endl;
        return 0;
    }

    std::cout << "Some realtime facade push tests FAILED!" << std::endl;
    return 1;
}
