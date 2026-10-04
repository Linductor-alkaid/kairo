// test_task_cancellation_fallback.cpp
// C1：KAIRO_STOP_TOKEN_FORCE_FALLBACK 强制实例化的取消语义冒烟测试。
//
// 该目标连同 executor 库源一起以 -DKAIRO_STOP_TOKEN_FORCE_FALLBACK 重编，
// 验证 Android fallback StopToken/StopSource 在任务取消协议下的行为与桌面
// std::stop_token 一致（可复制 token、每任务独立 state、stop_requested 轮询、
// 排队/运行中取消仲裁）。standalone（无 GTest），桌面 CI 与 Android 均可跑。

#include <atomic>
#include <chrono>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include <kairo/executor.hpp>
#include <kairo/stop_token.hpp>

#if defined(KAIRO_STOP_TOKEN_FORCE_FALLBACK)
static_assert(!std::is_same_v<kairo::StopToken, std::stop_token>,
              "forced fallback must instantiate executor's own StopToken");
#endif

using namespace kairo;
using namespace std::chrono_literals;

#define TEST_ASSERT(condition, message)                                      \
    do {                                                                     \
        if (!(condition)) {                                                  \
            std::cerr << "FAILED: " << message << " at " << __FILE__      \
                      << ":" << __LINE__ << std::endl;                     \
            return false;                                                    \
        }                                                                    \
    } while (0)

namespace {

ExecutorConfig one_thread_config() {
    ExecutorConfig config;
    config.min_threads = 1;
    config.max_threads = 1;
    return config;
}

bool test_queued_cancel_with_fallback_token() {
    std::cout << "fallback queued cancel..." << std::endl;
    Executor executor;
    TEST_ASSERT(executor.initialize(one_thread_config()), "initialize");

    auto gate = std::make_shared<std::promise<void>>();
    auto blocker = executor.submit_with_handle([gate]() noexcept {
        gate->get_future().wait();
        return 0;
    });

    std::atomic<bool> ran{false};
    auto submission = executor.submit_cancellable(
        [&ran](StopToken token) noexcept {
            ran.store(true, std::memory_order_release);
            while (!token.stop_requested()) {
                std::this_thread::yield();
            }
            return 1;
        });

    const auto response = executor.request_task_cancel(submission.handle);
    TEST_ASSERT(response.result == TaskCancellationResult::RequestedBeforeStart,
                "queued cancel must win before start");

    gate->set_value();
    (void)blocker.future.wait_for(5s);

    TEST_ASSERT(submission.future.wait_for(5s) == std::future_status::ready,
                "cancelled future must resolve");
    bool cancelled = false;
    try {
        (void)submission.future.get();
    } catch (const TaskCancelled& exception) {
        cancelled = exception.reason() == TaskCancellationReason::Explicit;
    } catch (...) {
    }
    TEST_ASSERT(cancelled, "future must carry TaskCancelled(Explicit)");
    TEST_ASSERT(!ran.load(), "cancelled queued task must not run");
    TEST_ASSERT(executor.get_failure_status().total_count == 0,
                "cancellation is lifecycle, not failure");

    executor.shutdown();
    std::cout << "  fallback queued cancel: PASSED" << std::endl;
    return true;
}

bool test_running_cooperative_cancel_with_fallback_token() {
    std::cout << "fallback running cancel..." << std::endl;
    Executor executor;
    TEST_ASSERT(executor.initialize(one_thread_config()), "initialize");

    std::atomic<bool> started{false};
    auto submission = executor.submit_cancellable(
        [&started](StopToken token) noexcept {
            started.store(true, std::memory_order_release);
            while (!token.stop_requested()) {
                std::this_thread::yield();
            }
            return 9;
        });

    while (!started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    const auto response = executor.request_task_cancel(submission.handle);
    TEST_ASSERT(response.result == TaskCancellationResult::RequestedRunning,
                "running cancel must request the token");
    TEST_ASSERT(executor.request_task_cancel(submission.handle).result ==
                    TaskCancellationResult::AlreadyRequested,
                "repeat running cancel must be idempotent");

    TEST_ASSERT(submission.future.wait_for(10s) == std::future_status::ready,
                "cooperatively cancelled task must finish");
    TEST_ASSERT(submission.future.get() == 9,
                "task result is preserved after cooperative stop");

    const auto status = executor.get_cancellation_status();
    TEST_ASSERT(status.request_count == 1, "first request counted once");
    TEST_ASSERT(status.running_request_count == 1, "running request counted");
    TEST_ASSERT(status.completed_after_request_count == 1,
                "completed-after-request counted");

    executor.shutdown();
    std::cout << "  fallback running cancel: PASSED" << std::endl;
    return true;
}

bool test_fallback_delayed_timer_cancel() {
    std::cout << "fallback timer cancel..." << std::endl;
    Executor executor;
    TEST_ASSERT(executor.initialize(one_thread_config()), "initialize");

    std::atomic<bool> ran{false};
    auto submission = executor.submit_delayed_cancellable(
        60'000, [&ran](StopToken) noexcept {
            ran.store(true, std::memory_order_release);
            return 1;
        });

    TEST_ASSERT(submission.handle.cancel() ==
                    TimerOperationResult::CancelledBeforeDispatch,
                "pending timer cancel must succeed");
    TEST_ASSERT(submission.future.wait_for(5s) == std::future_status::ready,
                "cancelled timer future must resolve");
    bool cancelled = false;
    try {
        (void)submission.future.get();
    } catch (const TaskCancelled&) {
        cancelled = true;
    } catch (...) {
    }
    TEST_ASSERT(cancelled, "timer future must carry TaskCancelled");
    TEST_ASSERT(!ran.load(), "cancelled timer must not run");

    executor.shutdown();
    std::cout << "  fallback timer cancel: PASSED" << std::endl;
    return true;
}

}  // namespace

int main() {
    bool all_passed = true;
    all_passed &= test_queued_cancel_with_fallback_token();
    all_passed &= test_running_cooperative_cancel_with_fallback_token();
    all_passed &= test_fallback_delayed_timer_cancel();

    if (all_passed) {
        std::cout << "All cancellation fallback instantiation tests passed."
                  << std::endl;
        return 0;
    }
    return 1;
}
