#include <chrono>
#include <future>
#include <iostream>
#include <thread>

#include <kairo/executor.hpp>

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

ExecutorConfig single_thread_config() {
    ExecutorConfig config;
    config.min_threads = 1;
    config.max_threads = 1;
    return config;
}

bool test_wait_completion_result_success() {
    Executor executor;
    TEST_ASSERT(executor.initialize(single_thread_config()),
                "executor should initialize");

    auto future = executor.submit([]() { return 7; });
    auto result = executor.wait_for_completion_ex(std::chrono::seconds(1));

    TEST_ASSERT(result.completed, "wait result should report completion");
    TEST_ASSERT(!result.timed_out, "successful wait should not time out");
    TEST_ASSERT(result.status.is_idle, "successful wait should report idle");
    TEST_ASSERT(result.status.pending_tasks == 0,
                "successful wait should report no pending tasks");
    TEST_ASSERT(future.get() == 7, "future result should remain available");
    TEST_ASSERT(executor.get_failure_status().wait_timeout_count == 0,
                "successful wait should not count timeout");

    executor.shutdown();
    return true;
}

bool test_wait_completion_result_timeout_keeps_pending_status() {
    Executor executor;
    TEST_ASSERT(executor.initialize(single_thread_config()),
                "executor should initialize");

    auto future = executor.submit([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    });

    auto result = executor.wait_for_completion_ex(std::chrono::milliseconds(20));
    TEST_ASSERT(!result.completed, "timeout wait should report incomplete");
    TEST_ASSERT(result.timed_out, "timeout wait should mark timed_out");
    TEST_ASSERT(!result.status.is_idle,
                "timeout wait should report non-idle status");
    TEST_ASSERT(result.status.pending_tasks > 0,
                "timeout wait should retain pending task count");
    TEST_ASSERT(result.status.active_tasks > 0 || result.status.queued_tasks > 0,
                "timeout wait should show active or queued tasks");
    TEST_ASSERT(executor.get_failure_status().wait_timeout_count == 1,
                "timeout wait should count WaitTimeout");

    future.get();
    TEST_ASSERT(executor.wait_for_completion_for(std::chrono::seconds(1)),
                "executor should complete after slow task finishes");

    executor.shutdown();
    return true;
}

bool test_first_submission_does_not_miss_worker_wakeup() {
    constexpr int iterations = 1000;

    for (int i = 0; i < iterations; ++i) {
        Executor executor;
        TEST_ASSERT(executor.initialize(single_thread_config()),
                    "executor should initialize");

        auto future = executor.submit([]() { return 7; });
        // 断言的是"唤醒不丢失"而非时延上限：错过唤醒的提交在池存续期内
        // 永远不会完成，等待 1s 仍能判定；反之饥饿的 Debug Windows runner
        // 上一次 100ms 窗口不足以覆盖正常的调度+执行（PR #192 与 master
        // 各环境性失败一次），属于误报。
        auto result =
            executor.wait_for_completion_ex(std::chrono::seconds(1));

        TEST_ASSERT(result.completed,
                    "first submission should not miss the worker wakeup");
        TEST_ASSERT(future.get() == 7, "first submitted task should run");
        executor.shutdown();
    }

    return true;
}

}  // namespace

int main() {
    bool all_passed = true;
    all_passed &= test_wait_completion_result_success();
    all_passed &= test_wait_completion_result_timeout_keeps_pending_status();
    all_passed &= test_first_submission_does_not_miss_worker_wakeup();

    if (all_passed) {
        std::cout << "All wait completion result tests passed." << std::endl;
        return 0;
    }

    std::cerr << "Some wait completion result tests failed." << std::endl;
    return 1;
}
