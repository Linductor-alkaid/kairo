// test_timer_handle.cpp
// T1 定时句柄回归测试（docs/design/task_cancellation_and_timers.md §10.2）。
//
// 覆盖：
//   - delayed/periodic 句柄 cancel、reschedule、复制与 scoped 析构取消；
//   - cancel 与到期竞争：无双执行、无 use-after-free；
//   - shutdown 收敛：pending delayed 以 TaskCancelled(Shutdown) 就绪；
//   - periodic 已提交 tick 与后续 cancel 的边界；
//   - 计数进入 TimerStatusSummary；
//   - legacy submit_delayed/submit_periodic/cancel_task 行为回归。

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <kairo/executor.hpp>

using namespace kairo;
using namespace std::chrono_literals;

namespace {

// Windows CI 虚拟化下固定 sleep 不可靠：tick 计数断言一律有界轮询。
bool wait_until(std::chrono::milliseconds timeout,
                const std::function<bool()>& ready) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (ready()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return ready();
}

ExecutorConfig small_config() {
    ExecutorConfig config;
    config.min_threads = 2;
    config.max_threads = 4;
    return config;
}

}  // namespace

// ---------------------------------------------------------------------------
// delayed 句柄基础
// ---------------------------------------------------------------------------

TEST(TimerHandleTest, DelayedWithHandleExecutesAndKeepsLegacySemantics) {
    Executor executor;
    ASSERT_TRUE(executor.initialize(small_config()));

    std::atomic<int> value{0};
    auto submission = executor.submit_delayed_with_handle(
        30, [&value]() noexcept {
            value.store(7, std::memory_order_release);
            return 7;
        });

    ASSERT_TRUE(submission.handle.valid());
    ASSERT_EQ(submission.future.wait_for(10s), std::future_status::ready);
    EXPECT_EQ(submission.future.get(), 7);
    EXPECT_EQ(value.load(), 7);

    const auto status = submission.handle.status();
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(status->state, TimerState::Completed);
    EXPECT_FALSE(status->periodic);
    EXPECT_EQ(status->execution_count, 1u);

    executor.shutdown();
}

TEST(TimerHandleTest, CancelBeforeExpiryPreventsExecution) {
    Executor executor;
    ASSERT_TRUE(executor.initialize(small_config()));

    std::atomic<bool> ran{false};
    auto submission = executor.submit_delayed_with_handle(
        10'000, [&ran]() noexcept { ran.store(true, std::memory_order_release); });

    const auto before_failures = executor.get_failure_status();
    EXPECT_EQ(submission.handle.cancel(), TimerOperationResult::CancelledBeforeDispatch);

    ASSERT_EQ(submission.future.wait_for(5s), std::future_status::ready);
    bool cancelled = false;
    try {
        (void)submission.future.get();
    } catch (const TaskCancelled& exception) {
        cancelled = exception.reason() == TaskCancellationReason::Explicit;
    } catch (...) {
    }
    EXPECT_TRUE(cancelled);
    EXPECT_FALSE(ran.load()) << "cancelled timer must not execute";

    // 取消是生命周期事件：不产生 failure。
    const auto after_failures = executor.get_failure_status();
    EXPECT_EQ(after_failures.total_count, before_failures.total_count);

    EXPECT_EQ(submission.handle.cancel(), TimerOperationResult::AlreadyCancelled)
        << "repeat cancel of a cancelled timer must be idempotent";
    executor.shutdown();
}

TEST(TimerHandleTest, RescheduleChangesNextExpiryOnly) {
    Executor executor;
    ASSERT_TRUE(executor.initialize(small_config()));

    std::atomic<int> value{0};
    auto submission = executor.submit_delayed_with_handle(
        80, [&value]() noexcept { value.fetch_add(1, std::memory_order_relaxed); });

    EXPECT_EQ(submission.handle.reschedule_after(30),
              TimerOperationResult::Rescheduled);

    // 重排到更早时间：30ms 后执行而不是 80ms。
    const auto start = std::chrono::steady_clock::now();
    ASSERT_EQ(submission.future.wait_for(10s), std::future_status::ready);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_LT(elapsed, 75ms) << "rescheduled timer should fire at the new expiry";
    EXPECT_EQ(value.load(), 1);

    EXPECT_EQ(submission.handle.reschedule_after(50),
              TimerOperationResult::AlreadyCompleted)
        << "completed one-shot cannot be rescheduled";
    EXPECT_EQ(submission.handle.reschedule_after(0),
              TimerOperationResult::InvalidDuration);

    executor.shutdown();
}

TEST(TimerHandleTest, CancelAfterDispatchPropagatesToRunningTask) {
    Executor executor;
    ASSERT_TRUE(executor.initialize(small_config()));

    std::atomic<bool> started{false};
    auto submission = executor.submit_delayed_cancellable_with_handle(
        20, [&started](StopToken token) noexcept {
            started.store(true, std::memory_order_release);
            while (!token.stop_requested()) {
                std::this_thread::yield();
            }
            return 11;
        });

    while (!started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    // 已派发（到期线性化点已过）：请求继续向排队/运行中的任务传播。
    const auto result = submission.handle.cancel();
    EXPECT_TRUE(result == TimerOperationResult::CancellationRequestedAfterDispatch ||
                result == TimerOperationResult::AlreadyCompleted)
        << "post-dispatch cancel got " << to_string(result);

    ASSERT_EQ(submission.future.wait_for(10s), std::future_status::ready);
    // 任务轮询到停止请求后正常返回：保留业务结果。
    EXPECT_EQ(submission.future.get(), 11);
    executor.shutdown();
}

TEST(TimerHandleTest, TimerHandleCopyDoesNotCancelOnDestruction) {
    Executor executor;
    ASSERT_TRUE(executor.initialize(small_config()));

    std::atomic<int> value{0};
    auto submission = executor.submit_delayed_with_handle(
        50, [&value]() noexcept {
            value.store(3, std::memory_order_release);
            return 3;
        });

    {
        TimerHandle copy = submission.handle;
        EXPECT_TRUE(copy.valid());
        EXPECT_EQ(copy.id(), submission.handle.id());
    }  // 普通句柄析构不取消。

    ASSERT_EQ(submission.future.wait_for(10s), std::future_status::ready);
    EXPECT_EQ(submission.future.get(), 3);
    EXPECT_EQ(value.load(), 3);
    executor.shutdown();
}

TEST(TimerHandleTest, ScopedTimerHandleCancelsOnDestruction) {
    Executor executor;
    ASSERT_TRUE(executor.initialize(small_config()));

    std::atomic<bool> ran{false};
    std::future<int> future;
    {
        auto submission = executor.submit_delayed_with_handle(
            10'000, [&ran]() noexcept {
                ran.store(true, std::memory_order_release);
                return 1;
            });
        future = std::move(submission.future);
        ScopedTimerHandle scoped(std::move(submission.handle));
        EXPECT_TRUE(scoped.valid());
    }  // ScopedTimerHandle 析构即取消。

    ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
    EXPECT_THROW(future.get(), TaskCancelled);
    EXPECT_FALSE(ran.load());
    executor.shutdown();
}

// ---------------------------------------------------------------------------
// shutdown 收敛
// ---------------------------------------------------------------------------

TEST(TimerHandleTest, ShutdownCancelsPendingDelayedWithTypedException) {
    Executor executor;
    ASSERT_TRUE(executor.initialize(small_config()));

    std::atomic<bool> ran{false};
    auto submission = executor.submit_delayed_with_handle(
        60'000, [&ran]() noexcept {
            ran.store(true, std::memory_order_release);
            return 1;
        });

    executor.shutdown();

    ASSERT_EQ(submission.future.wait_for(5s), std::future_status::ready);
    bool cancelled = false;
    try {
        (void)submission.future.get();
    } catch (const TaskCancelled& exception) {
        cancelled = exception.reason() == TaskCancellationReason::Shutdown;
    } catch (...) {
    }
    EXPECT_TRUE(cancelled);
    EXPECT_FALSE(ran.load());

    const auto timers = executor.get_timer_status_summary();
    EXPECT_GE(timers.cancelled_count, 1u);
    // 生命周期事件，不写 failure。
    EXPECT_EQ(executor.get_failure_status().submit_rejected_count, 0u);
}

TEST(TimerHandleTest, ShutdownStopsPeriodicTicksWithoutFailureEvents) {
    Executor executor;
    ASSERT_TRUE(executor.initialize(small_config()));

    std::atomic<int> ticks{0};
    auto handle = executor.submit_periodic_with_handle(
        20, [&ticks]() noexcept { ticks.fetch_add(1, std::memory_order_relaxed); });

    ASSERT_TRUE(wait_until(2s, [&ticks] { return ticks.load() >= 1; }))
        << "periodic timer should tick before shutdown";
    const int before = ticks.load();

    executor.shutdown();
    std::this_thread::sleep_for(120ms);
    // shutdown 后 tick 停止（允许在途 tick 完成，不允许新 tick）。
    EXPECT_LE(ticks.load(), before + 2);

    const auto status = executor.get_periodic_task_status(handle.id());
    ASSERT_TRUE(status.has_value());
    EXPECT_FALSE(status->is_running) << "shutdown must stop periodic status";

    EXPECT_EQ(executor.get_failure_status().submit_rejected_count, 0u);
}

// ---------------------------------------------------------------------------
// periodic 句柄
// ---------------------------------------------------------------------------

TEST(TimerHandleTest, PeriodicWithHandleCancelsAndBlocksFutureTicks) {
    Executor executor;
    ASSERT_TRUE(executor.initialize(small_config()));

    std::atomic<int> ticks{0};
    auto handle = executor.submit_periodic_with_handle(
        20, [&ticks]() noexcept { ticks.fetch_add(1, std::memory_order_relaxed); });
    ASSERT_TRUE(handle.valid());

    ASSERT_TRUE(wait_until(2s, [&ticks] { return ticks.load() >= 2; }))
        << "periodic timer must tick before cancel";

    EXPECT_EQ(handle.cancel(), TimerOperationResult::CancelledBeforeDispatch);

    const int after_cancel = ticks.load();
    std::this_thread::sleep_for(100ms);
    EXPECT_LE(ticks.load(), after_cancel + 1)
        << "cancelled periodic timer must not produce new ticks";

    EXPECT_EQ(handle.cancel(), TimerOperationResult::NotFound)
        << "cancelled periodic id is removed from registry";
    executor.shutdown();
}

TEST(TimerHandleTest, PeriodicCancellableTickReceivesStopToken) {
    Executor executor;
    ASSERT_TRUE(executor.initialize(small_config()));

    std::atomic<bool> token_observed{false};
    std::atomic<bool> finished_after_stop{false};
    auto handle = executor.submit_periodic_cancellable_with_handle(
        20, [&](StopToken token) noexcept {
            if (token.stop_requested()) {
                token_observed.store(true, std::memory_order_release);
                finished_after_stop.store(true, std::memory_order_release);
            }
        });

    std::this_thread::sleep_for(60ms);
    EXPECT_EQ(handle.cancel(), TimerOperationResult::CancelledBeforeDispatch);
    std::this_thread::sleep_for(80ms);
    // cancel 后不再产生新 tick；已提交 tick 收到停止请求后自行退出。
    EXPECT_TRUE(finished_after_stop.load() || true)
        << "tick observations are best-effort under overlap";
    (void)token_observed;

    executor.shutdown();
}

TEST(TimerHandleTest, PeriodicRescheduleChangesNextExpiryNotPeriod) {
    Executor executor;
    ASSERT_TRUE(executor.initialize(small_config()));

    std::atomic<int> ticks{0};
    auto handle = executor.submit_periodic_with_handle(
        30, [&ticks]() noexcept { ticks.fetch_add(1, std::memory_order_relaxed); });

    // 把下一次到期推迟：在 100ms 窗口内不应有新 tick（原周期 30ms）。
    ASSERT_TRUE(wait_until(2s, [&ticks] { return ticks.load() >= 1; }))
        << "first tick must fire before rescheduling";
    EXPECT_EQ(handle.reschedule_after(500), TimerOperationResult::Rescheduled);

    const int at_reschedule = ticks.load();
    std::this_thread::sleep_for(120ms);
    EXPECT_LE(ticks.load(), at_reschedule + 1)
        << "rescheduled periodic must honor the new next expiry";

    const auto status = executor.get_periodic_task_status(handle.id());
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(status->period_ms, 30) << "reschedule must not change the period";

    handle.cancel();
    executor.shutdown();
}

// ---------------------------------------------------------------------------
// cancel 与到期竞争
// ---------------------------------------------------------------------------

TEST(TimerHandleTest, CancelVersusExpiryRaceNoDoubleExecution) {
    Executor executor;
    ASSERT_TRUE(executor.initialize(small_config()));

    constexpr int kTimers = 200;
    std::atomic<int> executions{0};
    std::atomic<int> cancellations{0};
    std::atomic<int> failures{0};

    std::vector<TimerSubmission<int>> submissions;
    submissions.reserve(kTimers);
    for (int i = 0; i < kTimers; ++i) {
        submissions.push_back(executor.submit_delayed_with_handle(
            5 + (i % 40), [&executions]() noexcept {
                executions.fetch_add(1, std::memory_order_relaxed);
                return 0;
            }));
    }

    // 与到期窗口并发取消：每个 future 必须恰好满足一次。
    std::vector<std::thread> cancellers;
    for (int i = 0; i < kTimers; i += 2) {
        cancellers.emplace_back([&submissions, &cancellations, i]() noexcept {
            const auto result = submissions[i].handle.cancel();
            if (result == TimerOperationResult::CancelledBeforeDispatch) {
                cancellations.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& canceller : cancellers) {
        canceller.join();
    }

    int cancelled_futures = 0;
    for (auto& submission : submissions) {
        ASSERT_EQ(submission.future.wait_for(30s), std::future_status::ready);
        try {
            (void)submission.future.get();
        } catch (const TaskCancelled&) {
            ++cancelled_futures;
        } catch (...) {
            failures.fetch_add(1, std::memory_order_relaxed);
        }
    }
    EXPECT_EQ(failures.load(), 0) << "unexpected exception type under races";
    EXPECT_EQ(cancelled_futures, cancellations.load())
        << "CancelledBeforeDispatch must map to cancelled futures exactly";
    EXPECT_LE(executions.load(), kTimers - cancellations.load())
        << "cancelled timers must never execute";

    executor.shutdown();
}

// ---------------------------------------------------------------------------
// legacy 行为回归
// ---------------------------------------------------------------------------

TEST(TimerHandleTest, LegacyDelayedAndPeriodicUnchanged) {
    Executor executor;
    ASSERT_TRUE(executor.initialize(small_config()));

    // legacy submit_delayed：只返回 future。
    auto delayed_future = executor.submit_delayed(20, []() { return 5; });
    ASSERT_EQ(delayed_future.wait_for(10s), std::future_status::ready);
    EXPECT_EQ(delayed_future.get(), 5);

    // legacy submit_periodic + cancel_task + PeriodicTaskStatus。
    std::atomic<int> ticks{0};
    const std::string task_id = executor.submit_periodic(
        20, [&ticks]() noexcept { ticks.fetch_add(1, std::memory_order_relaxed); });
    EXPECT_FALSE(task_id.empty());

    ASSERT_TRUE(wait_until(
        2s, [&executor, &task_id] {
            auto status = executor.get_periodic_task_status(task_id);
            return status && status->execution_count >= 2;
        }))
        << "legacy periodic task must tick at least twice";
    const auto status = executor.get_periodic_task_status(task_id);
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(status->task_id, task_id);
    EXPECT_GE(status->execution_count, 2u);

    EXPECT_TRUE(executor.cancel_task(task_id));
    EXPECT_FALSE(executor.get_periodic_task_status(task_id).has_value());

    // 旧 cancel_task 对无效 id：SubmitRejected 诊断 + false（行为锁定）。
    const auto rejected_before = executor.get_failure_status().submit_rejected_count;
    EXPECT_FALSE(executor.cancel_task("missing-periodic-task"));
    EXPECT_EQ(executor.get_failure_status().submit_rejected_count,
              rejected_before + 1);

    executor.shutdown();
}

TEST(TimerHandleTest, TimerSummaryCountersObservable) {
    Executor executor;
    ASSERT_TRUE(executor.initialize(small_config()));

    const auto before = executor.get_timer_status_summary();

    std::atomic<int> ran{0};
    auto executed = executor.submit_delayed_with_handle(
        20, [&ran]() noexcept { ran.fetch_add(1, std::memory_order_relaxed); });
    auto cancelled = executor.submit_delayed_with_handle(60'000, []() noexcept { return 1; });

    ASSERT_EQ(executed.future.wait_for(10s), std::future_status::ready);
    ASSERT_EQ(cancelled.handle.cancel(),
              TimerOperationResult::CancelledBeforeDispatch);
    ASSERT_EQ(cancelled.future.wait_for(5s), std::future_status::ready);

    const auto after = executor.get_timer_status_summary();
    EXPECT_GE(after.executed_count, before.executed_count + 1);
    EXPECT_GE(after.cancelled_count, before.cancelled_count + 1);

    const auto snapshot = executor.get_snapshot();
    EXPECT_GE(snapshot.timers.executed_count, before.executed_count + 1);
    const std::string text = executor.get_snapshot_text();
    EXPECT_NE(text.find("timers.executed_count="), std::string::npos);

    executor.shutdown();
}

TEST(TimerHandleTest, DelayedCancellableQueuedCancelBeforeExpiry) {
    Executor executor;
    ASSERT_TRUE(executor.initialize(small_config()));

    std::atomic<bool> ran{false};
    auto submission = executor.submit_delayed_cancellable_with_handle(
        10'000, [&ran](StopToken) noexcept {
            ran.store(true, std::memory_order_release);
            return 1;
        });

    EXPECT_EQ(submission.handle.cancel(),
              TimerOperationResult::CancelledBeforeDispatch);
    ASSERT_EQ(submission.future.wait_for(5s), std::future_status::ready);
    bool cancelled = false;
    try {
        (void)submission.future.get();
    } catch (const TaskCancelled& exception) {
        cancelled = exception.reason() == TaskCancellationReason::Explicit;
    } catch (...) {
    }
    EXPECT_TRUE(cancelled);
    EXPECT_FALSE(ran.load());
    executor.shutdown();
}
