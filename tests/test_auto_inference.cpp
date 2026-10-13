// 0.7.1 §3.0.2 / §3.0.3 任务特征自动推断 + 执行器自动选择契约测试
// (docs/design/roadmap_v0.7.md；设计语义见
// docs/design/scheduling_runtime.md §13)。
//
// 覆盖：
//  A. opt-in 边界：默认关闭时模式命中的任务也不产建议、计数恒零。
//  B. 推断注解（建议优先于切换）：blocking/low-latency 名称模式命中 →
//     AutoInferredIntent 位 + inferred_reason + detail 建议；路由不被
//     改变（任务仍投默认池）；auto_inferred_count 同步。
//  C. 显式 intent 优先：推断有话可说但用户显式设置 → 不注解，
//     auto_inference_rejected_count 同步。
//  D. 运行期开关：set_auto_inference_enabled(false) 后回到纯意图路由。
//  E. TaskInferenceTable 单元（合成时间戳，确定性）：EWMA、periodic
//     稳定/不规则判定、键空间上界（满后丢弃新名称）。
//  F. AdaptiveScheduler::select_bounded_executor 单元（伪造能力快照，
//     确定性）：最小负载当选、并列取名字序、未运行/已满过滤、
//     RealtimeQueue 与空候选返回空。
//  G. dispatch_auto 集成：注入 AdaptiveScheduler 后未指定
//     preferred_executor 的 LowLatency 提交被自动选择
//     （AutoExecutorSelection 位 + auto_executor_selected_count）；
//     DefaultScheduler 保持既有结构化拒绝（回归守卫）。

#include <gtest/gtest.h>

#include <kairo/adaptive_scheduler.hpp>
#include <kairo/executor.hpp>
#include <kairo/scheduler.hpp>
#include <kairo/task_inference.hpp>

#include <atomic>
#include <chrono>
#include <memory>
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

void init_executor_with_inference(Executor& executor, size_t threads) {
    ExecutorConfig config;
    config.min_threads = threads;
    config.max_threads = threads;
    config.enable_auto_inference = true;
    ASSERT_TRUE(executor.initialize(config));
}

RoutingDecision last_decision(Executor& executor) {
    auto decisions = executor.get_recent_routing_decisions(1);
    return decisions.empty() ? RoutingDecision{} : decisions.back();
}

}  // namespace

// ---------------------------------------------------------------------------
// A/B/C/D. submit_auto 推断注解
// ---------------------------------------------------------------------------

TEST(AutoInferenceTest, DisabledByDefaultProducesNoSuggestions) {
    Executor executor;
    init_executor(executor, 2);
    (void)executor.submit_auto(kairo::task([] {}).name("sensor_io_read"));
    const auto& decision = last_decision(executor);
    EXPECT_EQ(decision.diagnostics & RoutingDiagnostics::AutoInferredIntent, 0u);
    EXPECT_TRUE(decision.inferred_reason.empty());
    const auto metrics = executor.get_scheduling_metrics();
    EXPECT_EQ(metrics.auto_inferred_count, 0u);
    EXPECT_EQ(metrics.auto_inference_rejected_count, 0u);
}

TEST(AutoInferenceTest, BlockingIoPatternAnnotatesSuggestion) {
    Executor executor;
    init_executor_with_inference(executor, 2);
    (void)executor.submit_auto(kairo::task([] {}).name("sensor_io_read"));

    const auto& decision = last_decision(executor);
    EXPECT_NE(decision.diagnostics & RoutingDiagnostics::AutoInferredIntent, 0u);
    EXPECT_EQ(decision.inferred_reason, "blocking_io_pattern");
    EXPECT_NE(decision.detail.find("start_worker"), std::string::npos);
    // 建议优先于切换：路由仍指向默认异步池。
    EXPECT_EQ(decision.selected_backend, ExecutionBackend::DefaultAsync);
    EXPECT_EQ(executor.get_scheduling_metrics().auto_inferred_count, 1u);
}

TEST(AutoInferenceTest, LowLatencyPatternAnnotatesSuggestion) {
    Executor executor;
    init_executor_with_inference(executor, 2);
    (void)executor.submit_auto(kairo::task([] {}).name("ctrl_control_loop"));

    const auto& decision = last_decision(executor);
    EXPECT_NE(decision.diagnostics & RoutingDiagnostics::AutoInferredIntent, 0u);
    EXPECT_EQ(decision.inferred_reason, "low_latency_pattern");
    EXPECT_NE(decision.detail.find("dispatch_auto(LowLatency)"),
              std::string::npos);
}

TEST(AutoInferenceTest, ExplicitIntentTakesPrecedence) {
    Executor executor;
    init_executor_with_inference(executor, 2);
    (void)executor.submit_auto(
        kairo::task([] {}).name("sensor_io_read").intent(ExecutionIntent::GeneralCpu));

    const auto& decision = last_decision(executor);
    EXPECT_EQ(decision.diagnostics & RoutingDiagnostics::AutoInferredIntent, 0u);
    EXPECT_TRUE(decision.inferred_reason.empty());
    EXPECT_EQ(executor.get_scheduling_metrics().auto_inference_rejected_count, 1u);
    EXPECT_EQ(executor.get_scheduling_metrics().auto_inferred_count, 0u);
}

TEST(AutoInferenceTest, RuntimeToggleDisablesInference) {
    Executor executor;
    init_executor_with_inference(executor, 2);
    EXPECT_TRUE(executor.auto_inference_enabled());
    executor.set_auto_inference_enabled(false);
    EXPECT_FALSE(executor.auto_inference_enabled());

    (void)executor.submit_auto(kairo::task([] {}).name("sensor_io_read"));
    EXPECT_EQ(last_decision(executor).diagnostics &
                  RoutingDiagnostics::AutoInferredIntent,
              0u);
    EXPECT_EQ(executor.get_scheduling_metrics().auto_inferred_count, 0u);
}

// ---------------------------------------------------------------------------
// E. TaskInferenceTable 单元
// ---------------------------------------------------------------------------

TEST(TaskInferenceTableTest, DurationEwmaAndSamples) {
    detail::TaskInferenceTable table;
    SchedulingFeedback feedback;
    feedback.task_id = "worker_io_read";
    int64_t now = 1'000'000'000;  // 1s
    for (int i = 0; i < 4; ++i) {
        feedback.execution_duration_ns = 1'000'000;  // 1ms
        table.record(feedback, now);
        now += 100'000'000;
    }
    const auto characteristics = table.analyze("worker_io_read", now);
    EXPECT_EQ(characteristics.samples, 4u);
    EXPECT_EQ(characteristics.estimated_duration_ns, 1'000'000u);
    EXPECT_TRUE(characteristics.is_blocking_io);  // 名称模式
    EXPECT_FALSE(characteristics.is_periodic);    // 无间隔样本
}

TEST(TaskInferenceTableTest, PeriodicDetectionRequiresRegularIntervals) {
    detail::TaskInferenceTable table;
    SchedulingFeedback feedback;
    feedback.task_id = "tick";
    feedback.execution_duration_ns = 100;

    // 稳定 10ms 间隔 × 5 → periodic。
    int64_t now = 0;
    for (int i = 0; i < 6; ++i) {
        table.record(feedback, now);
        now += 10'000'000;
    }
    EXPECT_TRUE(table.analyze("tick", now).is_periodic);

    // 不规则间隔（抖动 50%）→ 非 periodic。
    detail::TaskInferenceTable jittered;
    now = 0;
    for (int i = 0; i < 8; ++i) {
        jittered.record(feedback, now);
        now += (i % 2 == 0) ? 5'000'000 : 15'000'000;
    }
    EXPECT_FALSE(jittered.analyze("tick", now).is_periodic);
}

TEST(TaskInferenceTableTest, BoundedKeyspaceDropsNewestNames) {
    detail::TaskInferenceTable table;
    SchedulingFeedback feedback;
    feedback.execution_duration_ns = 1;
    int64_t now = 0;
    for (size_t i = 0; i <= detail::TaskInferenceTable::kMaxTrackedNames; ++i) {
        feedback.task_id = "task-" + std::to_string(i);
        table.record(feedback, now);
        now += 1'000'000;
    }
    EXPECT_EQ(table.dropped_names(), 1u);
    // 首个名称仍在；最后（被丢弃的）名称无样本。
    EXPECT_TRUE(table.knows("task-0"));
    EXPECT_FALSE(table.knows("task-" +
                             std::to_string(detail::TaskInferenceTable::
                                                kMaxTrackedNames)));
}

// ---------------------------------------------------------------------------
// F. AdaptiveScheduler::select_bounded_executor 单元
// ---------------------------------------------------------------------------

TEST(SelectBoundedExecutorTest, PicksMinLoadLockfreeDeterministically) {
    AdaptiveScheduler scheduler;

    auto make = [](const char* name, bool registered, bool running,
                   size_t pending, size_t capacity) {
        ExecutorCapability capability;
        capability.backend = ExecutionBackend::LockFree;
        capability.name = name;
        capability.registered = registered;
        capability.running = running;
        capability.pending_work = pending;
        capability.capacity_hint = capacity;
        return capability;
    };

    std::vector<ExecutorCapability> candidates{
        make("a", true, true, 5, 10),  // 50%
        make("b", true, true, 2, 10),  // 20% ← 最小
        make("c", true, false, 0, 10), // 未运行 → 过滤
        make("d", false, true, 0, 10), // 未注册 → 过滤
        make("e", true, true, 10, 10), // 已满 → 过滤
    };
    EXPECT_EQ(scheduler.select_bounded_executor(ExecutionIntent::LowLatency,
                                                candidates)
                  .value_or(""),
              "b");

    // 绝对负载未知容量（capacity_hint == 0）按 pending_work 比较。
    std::vector<ExecutorCapability> no_capacity{
        make("x", true, true, 7, 0), make("y", true, true, 3, 0)};
    EXPECT_EQ(scheduler.select_bounded_executor(ExecutionIntent::LowLatency,
                                                no_capacity)
                  .value_or(""),
              "y");

    // 并列取名字序。
    std::vector<ExecutorCapability> tied{make("z", true, true, 1, 10),
                                         make("m", true, true, 1, 10)};
    EXPECT_EQ(scheduler.select_bounded_executor(ExecutionIntent::LowLatency,
                                                tied)
                  .value_or(""),
              "m");

    // RealtimeQueue 不做负载选择；空候选返回空。
    EXPECT_FALSE(scheduler.select_bounded_executor(
        ExecutionIntent::RealtimeQueue, candidates));
    EXPECT_FALSE(scheduler.select_bounded_executor(ExecutionIntent::LowLatency,
                                                   {}));
}

// ---------------------------------------------------------------------------
// G. dispatch_auto 自动选择集成
// ---------------------------------------------------------------------------

TEST(DispatchAutoSelectionTest, AdaptiveSchedulerSelectsRunningExecutor) {
    Executor executor;
    init_executor(executor, 2);
    ASSERT_TRUE(executor.register_lockfree_executor(
        "lf-a", std::make_unique<LockFreeTaskExecutor>(16)));
    ASSERT_TRUE(executor.register_lockfree_executor(
        "lf-b", std::make_unique<LockFreeTaskExecutor>(16)));
    ASSERT_TRUE(executor.start_lockfree_executor("lf-a"));
    ASSERT_TRUE(executor.start_lockfree_executor("lf-b"));
    executor.set_scheduler(std::make_unique<AdaptiveScheduler>());

    TaskOptions options;
    options.intent = ExecutionIntent::LowLatency;  // 未指定 preferred_executor
    std::atomic<int> executions{0};
    auto result = executor.dispatch_auto(options, [&executions] { ++executions; });

    EXPECT_TRUE(result.accepted);
    EXPECT_NE(result.decision.diagnostics & RoutingDiagnostics::AutoExecutorSelection,
              0u);
    // 并列最小负载 → 名字序确定性。
    EXPECT_EQ(result.decision.selected_executor_name, "lf-a");
    // 执行器自动选择计入指标（决策记录点统计）。
    EXPECT_GE(executor.get_scheduling_metrics().auto_executor_selected_count, 1u);
    executor.stop_lockfree_executor(result.decision.selected_executor_name);
    EXPECT_EQ(executions.load(), 1);
}

TEST(DispatchAutoSelectionTest, DefaultSchedulerKeepsPolicyRejection) {
    Executor executor;
    init_executor(executor, 2);
    ASSERT_TRUE(executor.register_lockfree_executor(
        "lf-a", std::make_unique<LockFreeTaskExecutor>(8)));
    ASSERT_TRUE(executor.start_lockfree_executor("lf-a"));

    TaskOptions options;
    options.intent = ExecutionIntent::LowLatency;  // 未指定 preferred_executor
    auto result = executor.dispatch_auto(options, [] {});

    // DefaultScheduler 不做自动选择：保持既有结构化拒绝（回归守卫）。
    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.decision.reason, RoutingReason::Rejected);
    EXPECT_EQ(result.decision.diagnostics & RoutingDiagnostics::AutoExecutorSelection,
              0u);
    EXPECT_EQ(executor.get_scheduling_metrics().auto_executor_selected_count, 0u);
}
