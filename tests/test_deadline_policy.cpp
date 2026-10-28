// 0.7.1 §3.2 deadline policy 契约测试
// (docs/design/roadmap_v0.7.md §3.2；设计语义见
// docs/design/scheduling_runtime.md §11)。
//
// 覆盖：
//  A. 边界不漂移：严格已过的提交时点准入拒绝（DeadlineExpired）对三种
//     策略一致；恰好相等不算错过（evaluate 边界 <=）；无 deadline 时
//     policy 无效（零观测、零处置）。
//  B. RunAnyway（默认 + 显式）：错过仍执行、返回值穿透 future、
//     deadline_missed_count=1、failure 事件 DeadlineMissed、不丢弃。
//  C. DropIfLate：错过不执行用户代码、future 以
//     TaskCancelled(DeadlineMissed) 就绪、missed=1 + dropped=1、遵循
//     取消类约定（不进 failure 统计）；未错过照常执行。
//  D. CancelIfLate：错过仍启动且 this_task::get_stop_token() 可观测
//     stop_requested；未错过令牌可观测但未请求；协作退出的任务快速
//     完成；非任务上下文返回空令牌。
//  E. 依赖传播矩阵（submit_after TaskBuilder 重载）：
//     上游 DropIfLate 丢弃 → 下游 DependencyCancelled；
//     上游 RunAnyway 错过 → 下游正常执行；
//     上游 CancelIfLate 协作完成后 → 下游正常执行。
//  F. 指标守恒：missed 只在严格错过时计一次，dropped 只归 DropIfLate。

#include <gtest/gtest.h>

#include <kairo/executor.hpp>
#include <kairo/scheduler.hpp>
#include <kairo/scheduling.hpp>
#include <kairo/task_cancellation.hpp>
#include <kairo/task_options.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <thread>

using namespace kairo;

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

namespace {

void init_executor(Executor& executor, size_t threads) {
    ExecutorConfig config;
    config.min_threads = threads;
    config.max_threads = threads;
    ASSERT_TRUE(executor.initialize(config));
}

// 强制错过的确定性构造：单 worker 先被门闩任务占住，待提交的目标任务
// 在队列中熬过自己的 deadline，再放行门闩——目标任务开始执行时必然
// 严格晚于 deadline。
//
// setup 返回前必须等到门闩已真实起跑（gate_running 握手）：若在 worker
// 尚未取走门闩时提交目标任务，两者会同队列竞争，出队次序取决于 EDF 键
//（Task::operator<：同优先级按 deadline 升序）而非提交顺序。握手保证
// 用例提交目标任务时门闩已占住 worker，构造对门闩起跑延迟不敏感。
//
// 门闩 deadline 必须是实际不可错过的远期时点（与下方"不应错过"用例的
// 写法一致），不能给临近值（如 10ms）：deadline_missed 在任务开始执行
// 时判定（evaluate_deadline_policy，每任务至多一次），runner 高负载时
// 门闩自身起跑延迟可能超过临近 deadline，被额外计一次错过，污染各用例
// 针对目标任务的 deadline_missed 计数（PR #224 的 flake 根因）。
struct LatenessFixture {
    Executor executor;
    std::atomic<bool> gate{false};
    std::atomic<bool> gate_running{false};
    std::future<void> blocker;

    void setup(size_t threads = 1) {
        init_executor(executor, threads);
        blocker = executor.submit_auto(
            kairo::task([this] {
                gate_running.store(true, std::memory_order_release);
                while (!gate.load(std::memory_order_acquire)) {
                    std::this_thread::sleep_for(milliseconds(1));
                }
            })
                .deadline(Clock::now() + std::chrono::hours(1)));
        while (!gate_running.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(milliseconds(1));
        }
    }

    void release() {
        gate.store(true, std::memory_order_release);
        blocker.get();
    }

    // 未走到 release() 的用例也必须放行门闩，否则 ~Executor 排空时
    // 会死等仍在自旋的 blocker。
    ~LatenessFixture() {
        gate.store(true, std::memory_order_release);
        if (blocker.valid()) {
            blocker.wait();
        }
    }

    static std::chrono::steady_clock::time_point imminent_deadline(
        milliseconds margin) {
        return Clock::now() + margin;
    }
};

bool threw_deadline_missed(std::future<int>& future) {
    try {
        future.get();
        return false;
    } catch (const TaskCancelled& cancelled) {
        return cancelled.reason() == TaskCancellationReason::DeadlineMissed;
    }
}

bool void_threw_deadline_missed(std::future<void>& future) {
    try {
        future.get();
        return false;
    } catch (const TaskCancelled& cancelled) {
        return cancelled.reason() == TaskCancellationReason::DeadlineMissed;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// A. 边界不漂移
// ---------------------------------------------------------------------------

TEST(DeadlinePolicyTest, AdmissionRejectionUnchangedAcrossPolicies) {
    // 提交时点已严格过期 → 三种策略同样被准入拒绝（DeadlineExpired），
    // policy 不改变准入语义。
    for (const auto policy : {DeadlinePolicy::RunAnyway,
                              DeadlinePolicy::DropIfLate,
                              DeadlinePolicy::CancelIfLate}) {
        Executor executor;
        init_executor(executor, 2);
        auto future = executor.submit_auto(
            kairo::task([] { return 1; })
                .deadline(Clock::now() - milliseconds(50))
                .deadline_policy(policy));
        EXPECT_THROW(future.get(), std::runtime_error);
        EXPECT_EQ(executor.get_scheduling_metrics().deadline_rejected_count, 1u);
        EXPECT_EQ(executor.get_scheduling_metrics().deadline_missed_count, 0u);
        EXPECT_EQ(executor.get_scheduling_metrics().deadline_dropped_count, 0u);
    }
}

TEST(DeadlinePolicyTest, FutureDeadlineNeverTriggers) {
    LatenessFixture fixture;
    fixture.setup(2);
    auto future = fixture.executor.submit_auto(
        kairo::task([] { return 7; })
            .deadline(Clock::now() + std::chrono::hours(1))
            .deadline_policy(DeadlinePolicy::DropIfLate));
    EXPECT_EQ(future.get(), 7);
    const auto metrics = fixture.executor.get_scheduling_metrics();
    EXPECT_EQ(metrics.deadline_missed_count, 0u);
    EXPECT_EQ(metrics.deadline_dropped_count, 0u);
}

TEST(DeadlinePolicyTest, PolicyWithoutDeadlineIsInert) {
    LatenessFixture fixture;
    fixture.setup(2);
    auto future = fixture.executor.submit_auto(
        kairo::task([] { return 3; })
            .deadline_policy(DeadlinePolicy::DropIfLate));
    EXPECT_EQ(future.get(), 3);
    const auto metrics = fixture.executor.get_scheduling_metrics();
    EXPECT_EQ(metrics.deadline_missed_count, 0u);
    EXPECT_EQ(metrics.deadline_dropped_count, 0u);
}

// ---------------------------------------------------------------------------
// B. RunAnyway（默认 = 0.6.x）
// ---------------------------------------------------------------------------

TEST(DeadlinePolicyTest, RunAnywayExecutesAfterMiss) {
    LatenessFixture fixture;
    fixture.setup(1);  // 单 worker：门闩占住 → 目标任务必然晚执行

    std::atomic<bool> executed{false};
    auto late = Clock::now() + milliseconds(30);
    auto future = fixture.executor.submit_auto(
        kairo::task([&executed] {
            executed.store(true, std::memory_order_release);
            return 42;
        }).deadline(late));
    std::this_thread::sleep_for(milliseconds(80));
    fixture.release();

    EXPECT_EQ(future.get(), 42);
    EXPECT_TRUE(executed.load());

    const auto metrics = fixture.executor.get_scheduling_metrics();
    EXPECT_EQ(metrics.deadline_missed_count, 1u);
    EXPECT_EQ(metrics.deadline_dropped_count, 0u);
    EXPECT_EQ(fixture.executor.get_failure_status().deadline_missed_count, 1u);
}

// ---------------------------------------------------------------------------
// C. DropIfLate
// ---------------------------------------------------------------------------

TEST(DeadlinePolicyTest, DropIfLateSkipsExecutionAndSettlesTerminal) {
    LatenessFixture fixture;
    fixture.setup(1);

    std::atomic<bool> executed{false};
    auto future = fixture.executor.submit_auto(
        kairo::task([&executed] {
            executed.store(true, std::memory_order_release);
            return 1;
        })
            .deadline(Clock::now() + milliseconds(30))
            .deadline_policy(DeadlinePolicy::DropIfLate));
    std::this_thread::sleep_for(milliseconds(80));
    fixture.release();

    EXPECT_FALSE(executed.load());
    EXPECT_TRUE(threw_deadline_missed(future));

    const auto metrics = fixture.executor.get_scheduling_metrics();
    EXPECT_EQ(metrics.deadline_missed_count, 1u);
    EXPECT_EQ(metrics.deadline_dropped_count, 1u);
    // 取消类约定：不进 failure 统计（failure 面板只剩 RunAnyway 的观测）。
    EXPECT_EQ(fixture.executor.get_failure_status().deadline_missed_count, 0u);
    EXPECT_EQ(fixture.executor.get_failure_status().task_exception_count, 0u);
}

TEST(DeadlinePolicyTest, DropIfLateBeforeDeadlineExecutesNormally) {
    LatenessFixture fixture;
    fixture.setup(2);
    auto future = fixture.executor.submit_auto(
        kairo::task([] { return 5; })
            .deadline(Clock::now() + std::chrono::hours(1))
            .deadline_policy(DeadlinePolicy::DropIfLate));
    EXPECT_EQ(future.get(), 5);
    EXPECT_EQ(fixture.executor.get_scheduling_metrics().deadline_dropped_count,
              0u);
}

// ---------------------------------------------------------------------------
// D. CancelIfLate
// ---------------------------------------------------------------------------

TEST(DeadlinePolicyTest, CancelIfLateRequestsCooperativeStopOnMiss) {
    LatenessFixture fixture;
    fixture.setup(1);

    std::atomic<bool> observed_stop{false};
    auto future = fixture.executor.submit_auto(
        kairo::task([&observed_stop]() -> int {
            const auto token = get_stop_token();
            // 协作轮询：错过时令牌已被请求；任务自行快速退出。
            for (int i = 0; i < 10'000 && !token.stop_requested(); ++i) {
                std::this_thread::sleep_for(milliseconds(1));
            }
            observed_stop.store(token.stop_requested(),
                                std::memory_order_release);
            return 9;
        })
            .deadline(Clock::now() + milliseconds(30))
            .deadline_policy(DeadlinePolicy::CancelIfLate));
    std::this_thread::sleep_for(milliseconds(80));
    fixture.release();

    EXPECT_EQ(future.get(), 9);  // 协作完成（非抢占，任务自行结束）
    EXPECT_TRUE(observed_stop.load());

    const auto metrics = fixture.executor.get_scheduling_metrics();
    EXPECT_EQ(metrics.deadline_missed_count, 1u);
    EXPECT_EQ(metrics.deadline_dropped_count, 0u);
}

TEST(DeadlinePolicyTest, CancelIfLateWithoutMissHasInactiveToken) {
    LatenessFixture fixture;
    fixture.setup(2);
    std::atomic<bool> possible{false};
    std::atomic<bool> requested{true};
    auto future = fixture.executor.submit_auto(
        kairo::task([&possible, &requested]() -> int {
            const auto token = get_stop_token();
            possible.store(token.stop_possible(), std::memory_order_release);
            requested.store(token.stop_requested(), std::memory_order_release);
            return 4;
        })
            .deadline(Clock::now() + std::chrono::hours(1))
            .deadline_policy(DeadlinePolicy::CancelIfLate));
    EXPECT_EQ(future.get(), 4);
    EXPECT_TRUE(possible.load());     // CancelIfLate 任务全程可观测令牌
    EXPECT_FALSE(requested.load());   // 未错过：无停止请求
}

TEST(StopTokenContextTest, OutsideTaskContextReturnsEmptyToken) {
    const auto token = get_stop_token();
    EXPECT_FALSE(token.stop_possible());
    EXPECT_FALSE(token.stop_requested());
}

// ---------------------------------------------------------------------------
// E. 依赖传播矩阵（submit_after TaskBuilder 重载）
// ---------------------------------------------------------------------------

TEST(DeadlinePolicyDependencyTest, DroppedUpstreamPropagatesDependencyCancelled) {
    LatenessFixture fixture;
    fixture.setup(1);

    std::atomic<bool> upstream_ran{false};
    auto upstream_submission = fixture.executor.submit_after_with_handle(
        std::vector<TaskHandle>{},
        kairo::task([&upstream_ran] {
            upstream_ran.store(true, std::memory_order_release);
        })
            .deadline(Clock::now() + milliseconds(30))
            .deadline_policy(DeadlinePolicy::DropIfLate));
    auto downstream_future = fixture.executor.submit_after(
        upstream_submission.handle, [] { return 1; });

    std::this_thread::sleep_for(milliseconds(80));
    fixture.release();

    // 上游：结构化丢弃终态。
    EXPECT_TRUE(void_threw_deadline_missed(upstream_submission.future));
    EXPECT_FALSE(upstream_ran.load());
    // 下游：既有归类规则 → DependencyCancelled。
    bool dependency_cancelled = false;
    try {
        downstream_future.get();
        FAIL() << "downstream must not run after dropped upstream";
    } catch (const TaskCancelled& cancelled) {
        dependency_cancelled =
            cancelled.reason() == TaskCancellationReason::DependencyCancelled;
    }
    EXPECT_TRUE(dependency_cancelled);
    EXPECT_EQ(fixture.executor.get_scheduling_metrics().deadline_dropped_count,
              1u);
}

TEST(DeadlinePolicyDependencyTest, RunAnywayUpstreamLetsDownstreamRun) {
    LatenessFixture fixture;
    fixture.setup(1);

    auto upstream_submission = fixture.executor.submit_after_with_handle(
        std::vector<TaskHandle>{},
        kairo::task([] {}).deadline(Clock::now() + milliseconds(30)));
    std::atomic<bool> downstream_ran{false};
    auto downstream_future = fixture.executor.submit_after(
        upstream_submission.handle,
        [&downstream_ran] {
            downstream_ran.store(true, std::memory_order_release);
            return 2;
        });

    std::this_thread::sleep_for(milliseconds(80));
    fixture.release();

    upstream_submission.future.get();  // void 上游正常完成
    EXPECT_EQ(downstream_future.get(), 2);
    EXPECT_TRUE(downstream_ran.load());
    EXPECT_EQ(fixture.executor.get_scheduling_metrics().deadline_missed_count,
              1u);
    EXPECT_EQ(fixture.executor.get_scheduling_metrics().deadline_dropped_count,
              0u);
}

TEST(DeadlinePolicyDependencyTest, CancelIfLateCooperativeCompletionLetsDownstreamRun) {
    LatenessFixture fixture;
    fixture.setup(1);

    auto upstream_submission = fixture.executor.submit_after_with_handle(
        std::vector<TaskHandle>{},
        kairo::task([]() -> int {
            // 协作快速退出（不抛异常，正常完成）。
            const auto token = get_stop_token();
            for (int i = 0; i < 10'000 && !token.stop_requested(); ++i) {
                std::this_thread::sleep_for(milliseconds(1));
            }
            return 3;
        })
            .deadline(Clock::now() + milliseconds(30))
            .deadline_policy(DeadlinePolicy::CancelIfLate));
    auto downstream_future = fixture.executor.submit_after(
        upstream_submission.handle, [] { return 4; });

    std::this_thread::sleep_for(milliseconds(80));
    fixture.release();

    EXPECT_EQ(upstream_submission.future.get(), 3);
    EXPECT_EQ(downstream_future.get(), 4);
    EXPECT_EQ(fixture.executor.get_scheduling_metrics().deadline_missed_count,
              1u);
    EXPECT_EQ(fixture.executor.get_scheduling_metrics().deadline_dropped_count,
              0u);
}

TEST(DeadlinePolicyDependencyTest, TrackedDropBeforeDeadlineRunsNormally) {
    LatenessFixture fixture;
    fixture.setup(2);

    auto upstream_submission = fixture.executor.submit_after_with_handle(
        std::vector<TaskHandle>{},
        kairo::task([] { return 6; })
            .deadline(Clock::now() + std::chrono::hours(1))
            .deadline_policy(DeadlinePolicy::DropIfLate));
    auto downstream_future = fixture.executor.submit_after(
        upstream_submission.handle, [] { return 7; });

    EXPECT_EQ(upstream_submission.future.get(), 6);
    EXPECT_EQ(downstream_future.get(), 7);
    EXPECT_EQ(fixture.executor.get_scheduling_metrics().deadline_dropped_count,
              0u);
}

// ---------------------------------------------------------------------------
// F. 指标守恒
// ---------------------------------------------------------------------------

TEST(DeadlinePolicyMetricsTest, MissedCountedOncePerLateStartAcrossPolicies) {
    LatenessFixture fixture;
    fixture.setup(1);

    // 三种策略各一次严格错过。
    auto run_anyway = fixture.executor.submit_auto(
        kairo::task([] { return 1; })
            .deadline(Clock::now() + milliseconds(30)));
    auto dropped = fixture.executor.submit_auto(
        kairo::task([] { return 2; })
            .deadline(Clock::now() + milliseconds(30))
            .deadline_policy(DeadlinePolicy::DropIfLate));
    auto cancelled = fixture.executor.submit_auto(
        kairo::task([] { return 3; })
            .deadline(Clock::now() + milliseconds(30))
            .deadline_policy(DeadlinePolicy::CancelIfLate));

    std::this_thread::sleep_for(milliseconds(80));
    fixture.release();

    EXPECT_EQ(run_anyway.get(), 1);
    EXPECT_TRUE(threw_deadline_missed(dropped));
    EXPECT_EQ(cancelled.get(), 3);

    const auto metrics = fixture.executor.get_scheduling_metrics();
    EXPECT_EQ(metrics.deadline_missed_count, 3u);   // 每次错过恰计一次
    EXPECT_EQ(metrics.deadline_dropped_count, 1u);  // 只归 DropIfLate
}
