// test_task_cancellation.cpp
// C1 任务级协作取消回归测试（docs/design/task_cancellation_and_timers.md §10.1）。
//
// 覆盖：
//   - 排队取消（含依赖未满足）：任务不执行，future 以 TaskCancelled(Explicit)
//     就绪，不产生 failure 事件；
//   - 运行中协作取消：request_task_cancel 只置位 StopToken；任务轮询退出，
//     抛 TaskCancelled 按取消归类，正常返回保留业务结果；
//   - 重复/过期/未知句柄：AlreadyRequested / AlreadyCompleted / NotFound；
//   - 依赖取消传播：依赖方以 TaskCancelled(DependencyCancelled) 终止，
//     when_all 聚合传播；
//   - 取消计数进入独立 CancellationStatus（非 ExecutorFailureStatus）；
//   - registry 容量耗尽的明确拒绝；
//   - cancel 与开始执行/完成并发竞争下 future 恰好满足一次。

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

ExecutorConfig one_thread_config() {
    ExecutorConfig config;
    config.min_threads = 1;
    config.max_threads = 1;
    return config;
}

ExecutorConfig small_config() {
    ExecutorConfig config;
    config.min_threads = 2;
    config.max_threads = 4;
    return config;
}

bool wait_until_ready(std::future<void>& future, std::chrono::milliseconds timeout) {
    return future.wait_for(timeout) == std::future_status::ready;
}

// future 就绪早于 worker 线程到达 record_task_exception 是合法时序；
// 计数断言必须轮询等待而非立即读取。
bool wait_for_counter(std::chrono::milliseconds timeout,
                      const std::function<uint64_t()>& read) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (read() >= 1u) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return read() >= 1u;
}

// 排队任务的辅助：先占满唯一 worker，再提交被测任务，保证其处于 Queued。
class OccupiedPool {
public:
    explicit OccupiedPool(Executor& executor)
        : gate_(std::make_shared<std::promise<void>>())
        , entered_(std::make_shared<std::promise<void>>()) {
        occupied_ = executor.submit([state = gate_, entered = entered_]() {
            entered->set_value();
            state->get_future().wait();
        });
        // 等占位任务真正进入执行再返回。此前构造只等 submit 返回——任务
        // 可能仍在队列中，随后提交的高优先级被测任务会先被调度出队
        // （PriorityScheduler 按优先级取，CRITICAL 先于排队的 NORMAL 占位
        // 任务），取消到达时被测任务已在运行，RequestedBeforeStart 的
        // 断言随之失败。
        entered_->get_future().wait();
    }

    void release() {
        if (released_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        gate_->set_value();
        if (occupied_.valid()) {
            occupied_.wait();
        }
    }

    ~OccupiedPool() {
        release();
    }

private:
    std::shared_ptr<std::promise<void>> gate_;
    std::shared_ptr<std::promise<void>> entered_;
    std::future<void> occupied_;
    std::atomic<bool> released_{false};
};

}  // namespace

// ---------------------------------------------------------------------------
// 排队取消
// ---------------------------------------------------------------------------

TEST(TaskCancellationTest, QueuedTaskCancelSkipsExecutionAndSatisfiesFuture) {
    Executor executor;
    ASSERT_TRUE(executor.initialize(one_thread_config()));

    OccupiedPool occupied(executor);

    std::atomic<bool> ran{false};
    auto submission = executor.submit_with_handle([&ran]() noexcept {
        ran.store(true, std::memory_order_release);
        return 7;
    });
    ASSERT_TRUE(submission.handle.valid());

    const auto before_failures = executor.get_failure_status();
    const auto before_cancel = executor.get_cancellation_status();

    const auto response = executor.request_task_cancel(submission.handle);
    EXPECT_EQ(response.result, TaskCancellationResult::RequestedBeforeStart);
    EXPECT_TRUE(response.accepted());

    occupied.release();

    ASSERT_EQ(submission.future.wait_for(5s), std::future_status::ready);
    bool cancelled = false;
    try {
        (void)submission.future.get();
    } catch (const TaskCancelled& exception) {
        cancelled = exception.reason() == TaskCancellationReason::Explicit;
    } catch (...) {
    }
    EXPECT_TRUE(cancelled) << "queued cancel must satisfy future with TaskCancelled(Explicit)";
    EXPECT_FALSE(ran.load()) << "cancelled queued task must not execute";

    const auto after_failures = executor.get_failure_status();
    EXPECT_EQ(after_failures.total_count, before_failures.total_count)
        << "successful cancellation must not produce failure events";

    const auto after_cancel = executor.get_cancellation_status();
    EXPECT_EQ(after_cancel.request_count, before_cancel.request_count + 1);
    EXPECT_EQ(after_cancel.queued_cancelled_count,
              before_cancel.queued_cancelled_count + 1);

    executor.shutdown();
}

TEST(TaskCancellationTest, QueuedCancelBeatsQueuedSoftTimeoutArbitration) {
    Executor executor;
    ExecutorConfig config = one_thread_config();
    config.task_timeout_ms = 60;
    ASSERT_TRUE(executor.initialize(config));

    OccupiedPool occupied(executor);

    auto submission = executor.submit_with_handle([]() noexcept { return 1; });

    // 立即取消（排队时长远未到 soft timeout）：必须是取消语义。
    const auto response = executor.request_task_cancel(submission.handle);
    ASSERT_EQ(response.result, TaskCancellationResult::RequestedBeforeStart);

    occupied.release();

    ASSERT_EQ(submission.future.wait_for(5s), std::future_status::ready);
    bool cancelled = false;
    try {
        (void)submission.future.get();
    } catch (const TaskCancelled&) {
        cancelled = true;
    } catch (...) {
    }
    EXPECT_TRUE(cancelled) << "cancel before timeout must win the queued arbitration";
    executor.shutdown();
}

// ---------------------------------------------------------------------------
// 运行中协作取消
// ---------------------------------------------------------------------------

TEST(TaskCancellationTest, RunningTaskCancelRequestsStopToken) {
    Executor executor;
    ASSERT_TRUE(executor.initialize(small_config()));

    std::atomic<bool> started{false};
    auto submission = executor.submit_cancellable([&started](StopToken token) noexcept {
        started.store(true, std::memory_order_release);
        while (!token.stop_requested()) {
            std::this_thread::yield();
        }
        return 42;
    });
    ASSERT_TRUE(submission.handle.valid());

    while (!started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    const auto response = executor.request_task_cancel(submission.handle);
    EXPECT_EQ(response.result, TaskCancellationResult::RequestedRunning);
    EXPECT_TRUE(response.accepted());

    // 任务收到停止请求后正常返回：future 保留业务结果。
    ASSERT_EQ(submission.future.wait_for(10s), std::future_status::ready);
    EXPECT_EQ(submission.future.get(), 42);

    const auto status = executor.get_cancellation_status();
    EXPECT_EQ(status.running_request_count, 1u);
    EXPECT_EQ(status.completed_after_request_count, 1u);

    executor.shutdown();
}

TEST(TaskCancellationTest, RunningTaskThrowingTaskCancelledIsClassifiedAsCancel) {
    Executor executor;
    ASSERT_TRUE(executor.initialize(small_config()));

    std::atomic<bool> started{false};
    auto submission = executor.submit_cancellable([&started](StopToken token) {
        started.store(true, std::memory_order_release);
        while (!token.stop_requested()) {
            std::this_thread::yield();
        }
        throw TaskCancelled(TaskCancellationReason::Explicit,
                            "task observed stop request");
    });

    while (!started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    ASSERT_EQ(executor.request_task_cancel(submission.handle).result,
              TaskCancellationResult::RequestedRunning);

    ASSERT_EQ(submission.future.wait_for(10s), std::future_status::ready);
    bool cancelled = false;
    try {
        submission.future.get();
    } catch (const TaskCancelled& exception) {
        cancelled = exception.reason() == TaskCancellationReason::Explicit;
    } catch (...) {
    }
    EXPECT_TRUE(cancelled);

    // 协作取消是生命周期事件：不进入 failure 统计。
    EXPECT_EQ(executor.get_failure_status().task_exception_count, 0u);
    EXPECT_EQ(executor.get_failure_status().total_count, 0u);

    const auto status = executor.get_cancellation_status();
    EXPECT_EQ(status.request_count, 1u);
    EXPECT_EQ(status.running_request_count, 1u);
    EXPECT_EQ(status.queued_cancelled_count, 0u);

    executor.shutdown();
}

TEST(TaskCancellationTest, TaskCancelledWithoutRequestStillCountsAsFailure) {
    Executor executor;
    ASSERT_TRUE(executor.initialize(small_config()));

    auto submission = executor.submit_cancellable([](StopToken) {
        throw TaskCancelled(TaskCancellationReason::Explicit,
                            "user threw cancellation without a request");
    });

    ASSERT_EQ(submission.future.wait_for(10s), std::future_status::ready);
    EXPECT_THROW(submission.future.get(), TaskCancelled);

    // 无取消请求时主动抛 TaskCancelled：不得绕过 failure 统计。
    // （计数在 worker 线程上记录，可能晚于 future 就绪，有界等待。）
    EXPECT_TRUE(wait_for_counter(
        std::chrono::seconds(2),
        [&executor]() {
            return executor.get_failure_status().task_exception_count;
        }))
        << "unrequested TaskCancelled must be counted as a task failure";
    executor.shutdown();
}

TEST(TaskCancellationTest, CancelOfTokenlessRunningTaskOnlyRecordsRequest) {
    Executor executor;
    ASSERT_TRUE(executor.initialize(one_thread_config()));

    std::atomic<bool> started{false};
    // 保持门替代固定 sleep：断言 RequestedRunning 期间任务不得自然完成
    // （固定 100ms 在高负载下可被主线程调度延迟耗尽，任务终态后取消得
    // AlreadyCompleted）。主线程放行后任务才完成。
    std::atomic<bool> release{false};
    auto submission = executor.submit_with_handle([&started, &release]() noexcept {
        started.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        return 5;
    });

    while (!started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    const auto response = executor.request_task_cancel(submission.handle);
    EXPECT_EQ(response.result, TaskCancellationResult::RequestedRunning);

    // 不接收 token 的任务只能记录"已请求"，任务本身照常完成。
    release.store(true, std::memory_order_release);
    ASSERT_EQ(submission.future.wait_for(10s), std::future_status::ready);
    EXPECT_EQ(submission.future.get(), 5);
    executor.shutdown();
}

// ---------------------------------------------------------------------------
// 幂等与过期句柄
// ---------------------------------------------------------------------------

TEST(TaskCancellationTest, RepeatAndStaleHandlesAreIdempotent) {
    Executor executor;
    ASSERT_TRUE(executor.initialize(one_thread_config()));

    OccupiedPool occupied(executor);

    auto submission = executor.submit_with_handle([]() noexcept { return 1; });

    ASSERT_EQ(executor.request_task_cancel(submission.handle).result,
              TaskCancellationResult::RequestedBeforeStart);
    // 排队取消后句柄进入终态：重复请求幂等返回，不再计数。
    const auto second = executor.request_task_cancel(submission.handle);
    EXPECT_TRUE(second.result == TaskCancellationResult::AlreadyCompleted ||
                second.result == TaskCancellationResult::NotFound)
        << "repeat cancel after terminal state, got "
        << to_string(second.result);

    occupied.release();
    ASSERT_EQ(submission.future.wait_for(5s), std::future_status::ready);

    EXPECT_EQ(executor.get_cancellation_status().request_count, 1u)
        << "repeat cancels must not re-count first requests";

    // 过期句柄（任务已完成）与未知句柄。
    auto done = executor.submit_with_handle([]() noexcept { return 2; });
    ASSERT_EQ(done.future.wait_for(10s), std::future_status::ready);
    EXPECT_EQ(executor.request_task_cancel(done.handle).result,
              TaskCancellationResult::AlreadyCompleted);

    const TaskHandle unknown("task_does_not_exist");
    EXPECT_EQ(executor.request_task_cancel(unknown).result,
              TaskCancellationResult::NotFound);
    EXPECT_EQ(executor.request_task_cancel(TaskHandle{}).result,
              TaskCancellationResult::NotFound);

    executor.shutdown();
}

TEST(TaskCancellationTest, RepeatRunningCancelReturnsAlreadyRequested) {
    Executor executor;
    ASSERT_TRUE(executor.initialize(small_config()));

    std::atomic<bool> started{false};
    // 第二道保持门：任务观察到 stop_requested 后不得立即返回，否则任务
    // 收尾（phase 终态 + registry finalize）与主线程的第二次 cancel 赛跑
    // （CI 高负载下主线程被抢占时第二次请求得 AlreadyCompleted）。
    // 门保证两次断言期间任务仍处 Running。
    std::atomic<bool> release{false};
    auto submission = executor.submit_cancellable(
        [&started, &release](StopToken token) noexcept {
            started.store(true, std::memory_order_release);
            while (!token.stop_requested()) {
                std::this_thread::yield();
            }
            while (!release.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            return 1;
        });

    while (!started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    EXPECT_EQ(executor.request_task_cancel(submission.handle).result,
              TaskCancellationResult::RequestedRunning);
    EXPECT_EQ(executor.request_task_cancel(submission.handle).result,
              TaskCancellationResult::AlreadyRequested);
    EXPECT_EQ(executor.get_cancellation_status().request_count, 1u);

    // 断言全部完成后才放行任务：future 以业务结果就绪。
    release.store(true, std::memory_order_release);
    ASSERT_EQ(submission.future.wait_for(10s), std::future_status::ready);
    executor.shutdown();
}

// ---------------------------------------------------------------------------
// 依赖图交互
// ---------------------------------------------------------------------------

TEST(TaskCancellationTest, CancelledDependencyPropagatesToDependents) {
    Executor executor;
    ASSERT_TRUE(executor.initialize(one_thread_config()));

    OccupiedPool occupied(executor);

    auto dependency = executor.submit_with_handle([]() noexcept { return 1; });
    auto dependent =
        executor.submit_after_with_handle(dependency.handle, []() noexcept { return 2; });

    // 取消依赖：依赖方不执行，依赖它的任务以 DependencyCancelled 终止。
    ASSERT_EQ(executor.request_task_cancel(dependency.handle).result,
              TaskCancellationResult::RequestedBeforeStart);

    occupied.release();

    ASSERT_EQ(dependency.future.wait_for(5s), std::future_status::ready);
    ASSERT_EQ(dependent.future.wait_for(5s), std::future_status::ready);
    bool dependency_cancelled = false;
    bool dependent_cancelled = false;
    try {
        (void)dependency.future.get();
    } catch (const TaskCancelled& exception) {
        dependency_cancelled =
            exception.reason() == TaskCancellationReason::Explicit;
    } catch (...) {
    }
    try {
        (void)dependent.future.get();
    } catch (const TaskCancelled& exception) {
        dependent_cancelled =
            exception.reason() == TaskCancellationReason::DependencyCancelled;
    } catch (...) {
    }
    EXPECT_TRUE(dependency_cancelled);
    EXPECT_TRUE(dependent_cancelled);

    executor.shutdown();
}

TEST(TaskCancellationTest, CancelDependencyBlockedTaskFreesWorker) {
    Executor executor;
    ASSERT_TRUE(executor.initialize(small_config()));

    // 依赖长期占用唯一入口：dependency 阻塞在 worker 上等待 gate。
    auto gate = std::make_shared<std::promise<void>>();
    auto dependency = executor.submit_with_handle([gate]() noexcept {
        gate->get_future().wait();
        return 1;
    });

    auto dependent =
        executor.submit_after_with_handle(dependency.handle, []() noexcept { return 2; });

    // 取消依赖阻塞中的 dependent 自身：不等待依赖完成即终态。
    ASSERT_EQ(executor.request_task_cancel(dependent.handle).result,
              TaskCancellationResult::RequestedBeforeStart);

    ASSERT_EQ(dependent.future.wait_for(5s), std::future_status::ready);
    bool cancelled = false;
    try {
        (void)dependent.future.get();
    } catch (const TaskCancelled& exception) {
        cancelled = exception.reason() == TaskCancellationReason::Explicit;
    } catch (...) {
    }
    EXPECT_TRUE(cancelled);

    gate->set_value();
    ASSERT_EQ(dependency.future.wait_for(5s), std::future_status::ready);
    executor.shutdown();
}

TEST(TaskCancellationTest, WhenAllPropagatesDependencyCancellation) {
    Executor executor;
    ASSERT_TRUE(executor.initialize(one_thread_config()));

    OccupiedPool occupied(executor);

    auto dependency = executor.submit_with_handle([]() noexcept { return 1; });
    TaskHandle combined = executor.when_all({dependency.handle});
    auto chained = executor.submit_after_with_handle(combined, []() noexcept { return 3; });

    ASSERT_EQ(executor.request_task_cancel(dependency.handle).result,
              TaskCancellationResult::RequestedBeforeStart);

    occupied.release();

    ASSERT_EQ(chained.future.wait_for(5s), std::future_status::ready);
    bool cancelled = false;
    try {
        (void)chained.future.get();
    } catch (const TaskCancelled& exception) {
        cancelled =
            exception.reason() == TaskCancellationReason::DependencyCancelled;
    } catch (...) {
    }
    EXPECT_TRUE(cancelled) << "when_all aggregation must propagate cancellation";
    executor.shutdown();
}

TEST(TaskCancellationTest, CancellableAfterAcceptsTokenAndCancels) {
    Executor executor;
    ASSERT_TRUE(executor.initialize(one_thread_config()));

    OccupiedPool occupied(executor);

    auto first = executor.submit_with_handle([]() noexcept { return 1; });
    auto dependent = executor.submit_cancellable_after(
        first.handle, [](StopToken) noexcept { return 9; });

    ASSERT_EQ(executor.request_task_cancel(first.handle).result,
              TaskCancellationResult::RequestedBeforeStart);

    occupied.release();

    ASSERT_EQ(dependent.future.wait_for(5s), std::future_status::ready);
    bool cancelled = false;
    try {
        (void)dependent.future.get();
    } catch (const TaskCancelled& exception) {
        cancelled =
            exception.reason() == TaskCancellationReason::DependencyCancelled;
    } catch (...) {
    }
    EXPECT_TRUE(cancelled);
    executor.shutdown();
}

// ---------------------------------------------------------------------------
// 优先级与 registry 容量
// ---------------------------------------------------------------------------

TEST(TaskCancellationTest, CancellablePriorityTaskSupportsQueuedCancel) {
    Executor executor;
    ASSERT_TRUE(executor.initialize(one_thread_config()));

    OccupiedPool occupied(executor);

    std::atomic<bool> ran{false};
    auto submission = executor.submit_cancellable_priority(
        3, [&ran](StopToken) noexcept {
            ran.store(true, std::memory_order_release);
            return 1;
        });

    ASSERT_EQ(executor.request_task_cancel(submission.handle).result,
              TaskCancellationResult::RequestedBeforeStart);
    occupied.release();

    ASSERT_EQ(submission.future.wait_for(5s), std::future_status::ready);
    EXPECT_THROW(submission.future.get(), TaskCancelled);
    EXPECT_FALSE(ran.load());
    executor.shutdown();
}

TEST(TaskCancellationTest, RegistryExhaustionRejectsObservable) {
    Executor executor;
    ASSERT_TRUE(executor.initialize(small_config()));
    executor.set_cancellation_registry_capacity(2);
    ASSERT_EQ(executor.cancellation_registry_capacity(), 2u);

    // 两个长期占用 registry 的任务（future 只能取一次，先取出再共享）。
    auto gate = std::make_shared<std::promise<void>>();
    auto gate_open = std::make_shared<std::future<void>>(gate->get_future());
    auto holder1 = executor.submit_with_handle([gate_open]() noexcept {
        gate_open->wait();
        return 1;
    });
    auto holder2 = executor.submit_with_handle([gate_open]() noexcept {
        gate_open->wait();
        return 2;
    });

    std::atomic<bool> ran{false};
    auto rejected = executor.submit_with_handle([&ran]() noexcept {
        ran.store(true, std::memory_order_release);
        return 3;
    });

    ASSERT_EQ(rejected.future.wait_for(5s), std::future_status::ready);
    bool rejected_with_error = false;
    try {
        (void)rejected.future.get();
    } catch (const std::runtime_error&) {
        rejected_with_error = true;
    } catch (...) {
    }
    EXPECT_TRUE(rejected_with_error)
        << "registry exhaustion must reject with an exception future, not "
           "silently lose cancellability";
    EXPECT_FALSE(ran.load());

    EXPECT_GE(executor.get_failure_status().submit_rejected_count, 1u);

    gate->set_value();
    EXPECT_EQ(holder1.future.wait_for(10s), std::future_status::ready);
    EXPECT_EQ(holder2.future.wait_for(10s), std::future_status::ready);
    executor.shutdown();
}

// ---------------------------------------------------------------------------
// 并发竞争：cancel vs 执行 vs 完成
// ---------------------------------------------------------------------------

TEST(TaskCancellationTest, ConcurrentCancelVersusExecutionAllFuturesResolve) {
    Executor executor;
    ASSERT_TRUE(executor.initialize(small_config()));

    constexpr int kTasks = 200;
    std::vector<decltype(executor.submit_with_handle([]() noexcept { return 0; }))>
        submissions;
    submissions.reserve(kTasks);

    for (int i = 0; i < kTasks; ++i) {
        submissions.push_back(executor.submit_with_handle(
            [i]() noexcept {
                if (i % 3 == 0) {
                    std::this_thread::sleep_for(1ms);
                }
                return i;
            }));
    }

    // 并发取消一半任务：每个 future 必须恰好满足一次（值或 TaskCancelled）。
    std::vector<std::thread> cancellers;
    for (int i = 0; i < kTasks; i += 2) {
        cancellers.emplace_back([&executor, &submissions, i]() noexcept {
            (void)executor.request_task_cancel(submissions[i].handle);
        });
    }
    for (auto& canceller : cancellers) {
        canceller.join();
    }

    for (int i = 0; i < kTasks; ++i) {
        ASSERT_EQ(submissions[i].future.wait_for(30s), std::future_status::ready)
            << "future " << i << " must resolve exactly once under cancel races";
        if (i % 2 == 1) {
            EXPECT_EQ(submissions[i].future.get(), i);
        } else {
            bool resolved = false;
            try {
                EXPECT_EQ(submissions[i].future.get(), i);
                resolved = true;
            } catch (const TaskCancelled&) {
                resolved = true;  // 取消赢得仲裁：合法终态
            } catch (...) {
                FAIL() << "unexpected exception type for future " << i;
            }
            EXPECT_TRUE(resolved);
        }
    }

    executor.shutdown();
}

TEST(TaskCancellationTest, SnapshotCarriesCancellationLifecycleFields) {
    // 单线程 + 占位：保证被取消任务处于排队态（多线程下第二个 worker
    // 可能先执行完任务，取消只能观察到 AlreadyCompleted）。
    Executor executor;
    ASSERT_TRUE(executor.initialize(one_thread_config()));

    OccupiedPool occupied(executor);
    auto submission = executor.submit_with_handle([]() noexcept { return 1; });
    ASSERT_EQ(executor.request_task_cancel(submission.handle).result,
              TaskCancellationResult::RequestedBeforeStart);
    occupied.release();
    ASSERT_EQ(submission.future.wait_for(5s), std::future_status::ready);

    const auto snapshot = executor.get_snapshot();
    EXPECT_EQ(snapshot.schema_version, 3u);
    EXPECT_EQ(snapshot.cancellation.request_count, 1u);
    EXPECT_EQ(snapshot.cancellation.queued_cancelled_count, 1u);

    const std::string text = executor.get_snapshot_text();
    EXPECT_NE(text.find("cancellation.request_count=1"), std::string::npos);

    executor.shutdown();
}

TEST(TaskCancellationTest, PlainSubmitBehaviorUnchanged) {
    // 回归锁定：普通 submit()/submit_priority() 返回类型与行为不变。
    Executor executor;
    ASSERT_TRUE(executor.initialize(small_config()));

    auto future = executor.submit([]() { return std::string("ok"); });
    EXPECT_EQ(future.get(), "ok");

    auto priority_future = executor.submit_priority(2, []() { return 7; });
    EXPECT_EQ(priority_future.get(), 7);

    auto throwing = executor.submit([]() { throw std::runtime_error("boom"); });
    EXPECT_THROW(throwing.get(), std::runtime_error);
    // future 就绪早于 worker 线程到达 record_task_exception 是合法时序
    // （见 wait_for_counter 注释）：计数断言必须轮询等待而非立即读取。
    EXPECT_TRUE(wait_for_counter(
        std::chrono::seconds(2),
        [&executor]() {
            return executor.get_failure_status().task_exception_count;
        }))
        << "throwing submit must be counted as a task failure";

    executor.shutdown();
}
