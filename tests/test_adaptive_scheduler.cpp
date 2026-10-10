// 0.7.0 M3 AdaptiveScheduler 行为测试
// (docs/design/roadmap_v0.7.md §2.4；独立验证代理编写，不修改库代码)。
//
// 覆盖：
//  A. 配置归一：各标量夹取边界与 config() 返回生效值（含提升窗口与降载
//     窗口的耦合夹取、负目标 → 0、默认目标数组）、D1 契约（merge_interval=0
//     时每次 route()/snapshot() 都重合并）、自定义桶界真实进入 p99 评估。
//  B. histogram_quantile_ns 公开辅助：空/退化分位、桶内线性插值、
//     多桶累计秩、溢出桶返回最后有限上界。
//  C. CPU/GPU 历史选择（纯调度器级，合成样本 + 伪造能力快照）：
//     冷启动回退启发式、双侧收敛翻转到 GPU/CPU、滞回边际内"维持"、
//     交替负载不逐条翻转、无 GPU 可提交时不产生 AdaptiveHistory、
//     RequireRequestedBackend 显式约束不做历史翻转、超长 GPU 名截断后
//     仍可命中历史、detail 携带两侧 EWMA 数值。
//  D. QoS 感知降载（窗口差分语义）：开闸窗口数、结构化拒绝 shape、
//     类边界（严格更低才拒绝）、目标 0 的类永不参与、关闸对称滞回、
//     降载关闭开关、无证据 fail-open、流量停止后闸门必然解除（原缺陷
//     回归守卫）、GPU 目的地决策不受降载影响。
//  E. QoS→priority 有界提升：promotion_windows 才开（早于它不开）、
//     封顶 CRITICAL、显式 priority 原样、未提升类原样、恢复滞回、
//     提升关闭开关、流量停止后提升必然解除。
//  F. 可解释性：format_state_text 行内容、wants_feedback 恒真、
//     DefaultScheduler 的 effective_priority_for 原样返回（0.6.1 回归）。
//  G. Executor 集成：route_task 0.6.0 风格归一化（LoadShedding 不设
//     status → Rejected + 计数）、AdaptiveHistory 计数、CpuOrGpu 的
//     AdaptiveHistory CPU 豁免与启发式 CPU 拒绝对照、真实 AdaptiveScheduler
//     注入后 wants_feedback 生效（TaskBuilder / CpuGpuTask 两侧样本流入、
//     异常任务 success=false）、提升态下 priority_promoted_count 增长。
//  H. 并发冒烟：4 route 线程（持续喂样维持窗口证据）+ 2 on_task_completed
//     线程无崩溃无死锁，状态机在并发下正常开闸。
//
// 窗口语义（2026-10-10 缺陷修复后的差分语义，独立验证复核确认）：
// AdaptiveSchedulerConfig::aggregator 已下发内嵌聚合器（merge_interval/
// 桶界/alpha/max_keys 生效），State::evaluate 按两次评估间的**样本增量**
// 计算 window p99 与证据。因此本文件中"一个评估窗口" = （喂足
// min_window_samples 条新样本）+ 恰好一次 route()；merge_interval=0 下
// 每次 route() 都恰好合并+评估一次——advance_window() 是唯一的窗口推进
// 入口，不带喂样的 route() 是空窗口（必然 fail-open），状态机测试中
// 所有 route() 都必须经由 advance_window 并在窗口前喂样。

#include <gtest/gtest.h>

#include <kairo/adaptive_scheduler.hpp>
#include <kairo/executor.hpp>
#include <kairo/feedback_aggregator.hpp>
#include <kairo/scheduler.hpp>
#include <kairo/scheduling.hpp>
#include <kairo/task_options.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace kairo;
namespace sched = kairo::scheduling;

namespace {

using sched::FeedbackEntry;
using sched::FeedbackHistogram;
using sched::FeedbackSnapshot;
using sched::find_entry;
using sched::histogram_quantile_ns;

using std::chrono::milliseconds;

constexpr int64_t kInt64Max = std::numeric_limits<int64_t>::max();

// ---- 合成反馈 ----------------------------------------------------------

SchedulingFeedback make_feedback(ExecutionBackend backend,
                                 const std::string& executor_name,
                                 QosClass qos, int64_t queue_wait_ns,
                                 int64_t execution_ns) {
    SchedulingFeedback feedback;
    feedback.task_id = "feed";
    feedback.qos = qos;
    feedback.success = true;
    feedback.backend = backend;
    feedback.executor_name = executor_name;
    feedback.queue_wait_ns = queue_wait_ns;
    feedback.execution_duration_ns = execution_ns;
    return feedback;
}

void feed(AdaptiveScheduler& scheduler, ExecutionBackend backend,
          const std::string& executor_name, QosClass qos, size_t count,
          int64_t queue_wait_ns, int64_t execution_ns) {
    const SchedulingFeedback feedback =
        make_feedback(backend, executor_name, qos, queue_wait_ns, execution_ns);
    for (size_t i = 0; i < count; ++i) {
        scheduler.on_task_completed(feedback);
    }
}

// ---- 请求与能力快照 ----------------------------------------------------

TaskRouter::Request auto_request(QosClass qos = QosClass::Standard) {
    TaskRouter::Request request;
    request.cpu_gpu_task = false;
    request.options.name = "adaptive_probe";
    request.options.qos = qos;
    return request;
}

TaskRouter::Request cpu_gpu_request(
    QosClass qos = QosClass::Standard,
    std::optional<bool> gpu_selected = std::nullopt,
    FallbackPolicy fallback = FallbackPolicy::NoFallback,
    std::optional<std::string> preferred_executor = std::nullopt) {
    TaskRouter::Request request;
    request.cpu_gpu_task = true;
    request.options.intent = ExecutionIntent::CpuOrGpu;
    request.options.name = "adaptive_probe";
    request.options.qos = qos;
    request.options.fallback = fallback;
    request.options.preferred_executor = std::move(preferred_executor);
    request.gpu_selected = gpu_selected;
    return request;
}

std::vector<ExecutorCapability> gpu_capabilities(const std::string& name = "gpu0") {
    ExecutorCapability gpu;
    gpu.backend = ExecutionBackend::Gpu;
    gpu.name = name;
    gpu.registered = true;
    gpu.running = true;
    gpu.supports_gpu_kernel = true;
    return {gpu};
}

// 伪造"已注册但未运行"的 GPU 能力（不可提交）。
std::vector<ExecutorCapability> stopped_gpu_capabilities(const std::string& name) {
    ExecutorCapability gpu;
    gpu.backend = ExecutionBackend::Gpu;
    gpu.name = name;
    gpu.registered = true;
    gpu.running = false;
    gpu.supports_gpu_kernel = true;
    return {gpu};
}

// ---- 配置构造 ----------------------------------------------------------

// CPU/GPU 历史选择测试配置；关闭降载/提升以隔离历史决策。
AdaptiveSchedulerConfig history_config() {
    AdaptiveSchedulerConfig config;
    config.min_samples = 8;
    config.load_shedding_enabled = false;
    config.priority_promotion_enabled = false;
    config.aggregator.merge_interval = milliseconds{0};
    return config;
}

// 降载测试配置（桶界经修复后真实生效）：超阈样本 500ms 全落桶1 →
// 窗口 p99 = 1ms + (1s - 1ms) × 1.0 = 1e9 ns > 目标 1e8；恢复样本 100µs
// 全落桶0 → 窗口 p99 = 1e6 ns < 目标 × 0.5 = 5e7。
AdaptiveSchedulerConfig shed_config() {
    AdaptiveSchedulerConfig config;
    config.queue_wait_p99_target_ns = {0, 100'000'000, 100'000'000,
                                       100'000'000};
    config.shed_breach_windows = 3;
    config.shed_close_windows = 5;
    config.min_window_samples = 8;
    config.priority_promotion_enabled = false;
    config.aggregator.merge_interval = milliseconds{0};
    config.aggregator.queue_wait_bucket_bounds_ns = {1'000'000,
                                                     1'000'000'000};
    return config;
}

void feed_breach(AdaptiveScheduler& scheduler, QosClass qos, size_t count = 40) {
    feed(scheduler, ExecutionBackend::DefaultAsync, "default", qos, count,
         500'000'000, 1'000);
}

// 单个恢复窗口的喂样（差分语义下窗口内 40 条即足够 min_window_samples=8，
// 且全部落桶0 → 窗口 p99 = 1e6 ns < 5e7 恢复阈值）。
void feed_recovery(AdaptiveScheduler& scheduler, QosClass qos, size_t count = 40) {
    feed(scheduler, ExecutionBackend::DefaultAsync, "default", qos, count,
         100'000, 1'000);
}

// ---- 窗口推进 ----------------------------------------------------------

// 恰好推进一个评估窗口：merge_interval=0（已真实生效）下每次 route()
// 都合并+评估一次；调用方必须在调用前喂足本窗口的新样本，否则该窗口
// 是空窗口（差分 0 → fail-open，状态机回到宽容侧）。
RoutingDecision advance_window(AdaptiveScheduler& scheduler,
                               const TaskRouter::Request& request,
                               const std::vector<ExecutorCapability>&
                                   capabilities = {}) {
    return scheduler.route(request, capabilities);
}

// 与契约测试同款：带线程数的默认池初始化。
void init_executor(Executor& executor, size_t threads) {
    ExecutorConfig config;
    config.min_threads = threads;
    config.max_threads = threads;
    ASSERT_TRUE(executor.initialize(config));
}

// 冷启动/无 GPU 场景：AdaptiveScheduler 决策必须与 DefaultScheduler
// 逐字段一致（完全未标记的 0.6.1 基线）。
void expect_identical_to_default(const RoutingDecision& adaptive,
                                 const RoutingDecision& baseline) {
    EXPECT_EQ(adaptive.selected_backend, baseline.selected_backend);
    EXPECT_EQ(adaptive.selected_executor_name,
              baseline.selected_executor_name);
    EXPECT_EQ(adaptive.reason, baseline.reason);
    EXPECT_EQ(adaptive.status, baseline.status);
    EXPECT_EQ(adaptive.fell_back, baseline.fell_back);
    EXPECT_EQ(adaptive.diagnostics, baseline.diagnostics)
        << "fallthrough decisions must not carry adaptive diagnostics";
    EXPECT_EQ(adaptive.detail, baseline.detail);
}

// 0.6.0 风格脚本调度器：route() 原样返回预置决策（供 route_task 归一化
// 与 facade 规则的可控验证）。
class ScriptedScheduler final : public IScheduler {
public:
    RoutingDecision next;
    bool feedback = false;

    RoutingDecision route(const TaskRouter::Request&,
                          const std::vector<ExecutorCapability>&) override {
        return next;
    }

    bool wants_feedback() const noexcept override { return feedback; }
};

RoutingDecision make_scripted_decision(RoutingReason reason, uint32_t diagnostics,
                                       ExecutionBackend backend) {
    RoutingDecision decision;
    decision.task_name = "scripted";
    decision.requested_intent = ExecutionIntent::Auto;
    decision.reason = reason;
    decision.status = RoutingStatus::Accepted;
    decision.diagnostics = diagnostics;
    decision.selected_backend = backend;
    decision.selected_executor_name =
        backend == ExecutionBackend::DefaultAsync ? "default" : "gpu0";
    decision.detail = "scripted";
    return decision;
}

}  // namespace

// ---------------------------------------------------------------------------
// A. 配置归一
// ---------------------------------------------------------------------------

TEST(AdaptiveConfig, ScalarClampsLowSideAndNegativeTargets) {
    AdaptiveSchedulerConfig config;
    config.min_samples = 0;
    config.hysteresis_margin = 0.0;
    config.shed_breach_windows = 0;
    config.shed_close_windows = 0;
    config.promotion_windows = 0;
    config.promotion_close_windows = 0;
    config.min_window_samples = 0;
    config.recovery_ratio = 0.0;
    config.queue_wait_p99_target_ns = {-5, -1, 100, -1'000'000'000'000LL};
    config.aggregator.merge_interval = milliseconds{7};

    AdaptiveScheduler scheduler{config};
    const auto& effective = scheduler.config();
    EXPECT_EQ(effective.min_samples, 1u);
    EXPECT_DOUBLE_EQ(effective.hysteresis_margin, 0.05);
    EXPECT_EQ(effective.shed_breach_windows, 1u);
    EXPECT_EQ(effective.shed_close_windows, 1u);
    // promotion_windows 夹取下界是夹取后的 shed_breach_windows(=1)；
    // promotion_close_windows 下界是夹取后的 promotion_windows(=1)。
    EXPECT_EQ(effective.promotion_windows, 1u);
    EXPECT_EQ(effective.promotion_close_windows, 1u);
    EXPECT_EQ(effective.min_window_samples, 1u);
    EXPECT_DOUBLE_EQ(effective.recovery_ratio, 0.1);
    // 负目标按 0 处理（该类不评估），非负目标原样保留。
    EXPECT_EQ(effective.queue_wait_p99_target_ns[0], 0);
    EXPECT_EQ(effective.queue_wait_p99_target_ns[1], 0);
    EXPECT_EQ(effective.queue_wait_p99_target_ns[2], 100);
    EXPECT_EQ(effective.queue_wait_p99_target_ns[3], 0);
    EXPECT_EQ(effective.aggregator.merge_interval, milliseconds{7})
        << "config() must reflect the requested aggregator settings";
}

TEST(AdaptiveConfig, ScalarClampsHighSideAndPromotionCoupling) {
    AdaptiveSchedulerConfig config;
    config.min_samples = 2'000'000;
    config.hysteresis_margin = 0.99;
    config.shed_close_windows = 2'000'000;
    config.min_window_samples = 2'000'000;
    config.recovery_ratio = 1.0;
    config.shed_breach_windows = 2'000'000;  // 高位夹取先于提升耦合
    config.promotion_windows = 2;
    config.promotion_close_windows = 3;

    AdaptiveScheduler scheduler{config};
    const auto& effective = scheduler.config();
    EXPECT_EQ(effective.min_samples, 1'000'000u);
    EXPECT_DOUBLE_EQ(effective.hysteresis_margin, 0.9);
    EXPECT_EQ(effective.shed_breach_windows, 1'000'000u);
    EXPECT_EQ(effective.shed_close_windows, 1'000'000u);
    // promotion_windows = clamp(2, breach=1e6, 1e6) = 1e6；
    // promotion_close_windows = clamp(3, promotion=1e6, 1e6) = 1e6。
    EXPECT_EQ(effective.promotion_windows, 1'000'000u);
    EXPECT_EQ(effective.promotion_close_windows, 1'000'000u);
    EXPECT_EQ(effective.min_window_samples, 1'000'000u);
    EXPECT_DOUBLE_EQ(effective.recovery_ratio, 0.95);
}

TEST(AdaptiveConfig, PromotionFloorIsClampedShedBreachWindows) {
    // 提升窗口以夹取后的 shed_breach_windows 为下界，取消窗口以提升窗口
    // 为下界。
    AdaptiveSchedulerConfig config;
    config.shed_breach_windows = 5;
    config.promotion_windows = 2;
    config.promotion_close_windows = 4;
    AdaptiveScheduler scheduler{config};
    EXPECT_EQ(scheduler.config().shed_breach_windows, 5u);
    EXPECT_EQ(scheduler.config().promotion_windows, 5u)
        << "promotion_windows must be at least shed_breach_windows";
    EXPECT_EQ(scheduler.config().promotion_close_windows, 5u)
        << "promotion_close_windows must be at least promotion_windows";
}

TEST(AdaptiveConfig, DefaultTargetsMatchRoadmap) {
    AdaptiveSchedulerConfig config;
    AdaptiveScheduler scheduler{config};
    const auto& effective = scheduler.config();
    EXPECT_EQ(effective.queue_wait_p99_target_ns[0], 0);  // BestEffort 不评估
    EXPECT_EQ(effective.queue_wait_p99_target_ns[1], 100'000'000);
    EXPECT_EQ(effective.queue_wait_p99_target_ns[2], 10'000'000);
    EXPECT_EQ(effective.queue_wait_p99_target_ns[3], 2'000'000);
    EXPECT_EQ(effective.min_samples, 8u);
    EXPECT_DOUBLE_EQ(effective.hysteresis_margin, 0.25);
    EXPECT_EQ(effective.shed_breach_windows, 3u);
    EXPECT_EQ(effective.shed_close_windows, 5u);
    EXPECT_EQ(effective.promotion_windows, 6u);
    EXPECT_EQ(effective.promotion_close_windows, 10u);
    EXPECT_EQ(effective.min_window_samples, 32u);
    EXPECT_DOUBLE_EQ(effective.recovery_ratio, 0.5);
    EXPECT_TRUE(effective.load_shedding_enabled);
    EXPECT_TRUE(effective.priority_promotion_enabled);
}

TEST(AdaptiveConfig, MergeIntervalZeroAdvancesMergeCount) {
    // D1 契约（aggregator 配置真实下发的回归守卫）：merge_interval=0ms
    // 时每次 refresh 都重合并——连续 route()/feedback_snapshot() 必须推进
    // merge_count（修复前聚合器吃默认 100ms 门限，计数停滞）。
    AdaptiveSchedulerConfig config;
    config.aggregator.merge_interval = milliseconds{0};
    AdaptiveScheduler scheduler{config};

    const auto c1 = scheduler.feedback_snapshot()->merge_count;
    const auto c2 = scheduler.feedback_snapshot()->merge_count;
    EXPECT_EQ(c2, c1 + 1)
        << "merge_interval=0 must re-merge on every snapshot call";

    (void)scheduler.route(auto_request(), {});  // 恰好一次合并
    const auto c3 = scheduler.feedback_snapshot()->merge_count;
    EXPECT_EQ(c3, c2 + 2)
        << "route() merges once, the follow-up snapshot call once more";
}

TEST(AdaptiveConfig, CustomBucketBoundsReachP99Evaluation) {
    // aggregator 配置真实生效的第二守卫：自定义桶界 {1ms,1s} 下 50ms 样本
    // 的窗口 p99 = 1e9（整桶插值）> 目标 5e8 → 开闸；若仍用默认桶界，
    // p99 = 1e8 < 5e8 永不开闸。
    AdaptiveSchedulerConfig config;
    config.queue_wait_p99_target_ns = {0, 500'000'000, 0, 0};
    config.shed_breach_windows = 1;
    config.min_window_samples = 8;
    config.aggregator.merge_interval = milliseconds{0};
    config.aggregator.queue_wait_bucket_bounds_ns = {1'000'000,
                                                     1'000'000'000};
    AdaptiveScheduler scheduler{config};
    feed_breach(scheduler, QosClass::Standard, 40);
    feed(scheduler, ExecutionBackend::DefaultAsync, "default",
         QosClass::Standard, 40, 50'000'000, 1'000);
    advance_window(scheduler, auto_request(QosClass::Standard));
    EXPECT_TRUE(scheduler.load_shed_active(QosClass::Standard))
        << "custom bucket bounds must be wired into the p99 evaluation";
}

// ---------------------------------------------------------------------------
// B. histogram_quantile_ns 公开辅助
// ---------------------------------------------------------------------------

TEST(HistogramQuantile, EmptyHistogramAndDegenerateQuantiles) {
    FeedbackHistogram empty;
    EXPECT_EQ(histogram_quantile_ns(empty, 0.99), 0)
        << "no samples must read as 0 (unknown), not fast";
    FeedbackHistogram zero_count;
    zero_count.buckets.push_back({1'000, 0});
    EXPECT_EQ(histogram_quantile_ns(zero_count, 0.99), 0);
}

TEST(HistogramQuantile, WithinBucketLinearInterpolation) {
    FeedbackHistogram histogram;
    histogram.buckets.push_back({1'000, 10});
    // rank = ceil(10 * 0.9) = 9 → within = 0.9 → 900。
    EXPECT_EQ(histogram_quantile_ns(histogram, 0.9), 900);
    // rank = ceil(9.9) = 10 → within = 1.0 → 1000（上界）。
    EXPECT_EQ(histogram_quantile_ns(histogram, 0.99), 1'000);
    EXPECT_EQ(histogram_quantile_ns(histogram, 1.0), 1'000);
    // q <= 0 → 0；q >= 1 收敛为 1.0。
    EXPECT_EQ(histogram_quantile_ns(histogram, 0.0), 0);
    EXPECT_EQ(histogram_quantile_ns(histogram, -1.0), 0);
    EXPECT_EQ(histogram_quantile_ns(histogram, 1.5), 1'000);
}

TEST(HistogramQuantile, MultiBucketCumulativeRanking) {
    FeedbackHistogram histogram;
    histogram.buckets.push_back({100, 2});
    histogram.buckets.push_back({1'000, 2});
    // total=4：q=0.5 → rank=2 → 桶0 within=1.0 → 100。
    EXPECT_EQ(histogram_quantile_ns(histogram, 0.5), 100);
    // q=0.75 → rank=3 → 桶1 within=(3-2)/2=0.5 → 100+(900)*0.5=550。
    EXPECT_EQ(histogram_quantile_ns(histogram, 0.75), 550);
    // q=1.0 → rank=4 → 桶1 within=1.0 → 1000。
    EXPECT_EQ(histogram_quantile_ns(histogram, 1.0), 1'000);
}

TEST(HistogramQuantile, OverflowBucketReturnsLastFiniteBound) {
    FeedbackHistogram histogram;
    histogram.buckets.push_back({100, 1});
    histogram.buckets.push_back({kInt64Max, 5});
    // 溢出桶命中时返回最后有限上界（保守下界估计）。
    EXPECT_EQ(histogram_quantile_ns(histogram, 0.99), 100);
    EXPECT_EQ(histogram_quantile_ns(histogram, 1.0), 100);
}

// ---------------------------------------------------------------------------
// C. CPU/GPU 历史选择
// ---------------------------------------------------------------------------

TEST(AdaptiveCpuGpuHistory, ColdStartFallsBackToHeuristicUnmarked) {
    AdaptiveScheduler scheduler{history_config()};
    DefaultScheduler baseline_scheduler;
    const auto capabilities = gpu_capabilities();

    // 完全无样本：决策与 DefaultScheduler 逐字段一致。
    auto request = cpu_gpu_request(QosClass::Standard, std::nullopt);
    expect_identical_to_default(scheduler.route(request, capabilities),
                                baseline_scheduler.route(request, capabilities));

    // GPU 侧样本充足（8 ≥ min_samples）、CPU 侧不足（3 < 8）：
    // 任一侧不足即回退，不标记。
    feed(scheduler, ExecutionBackend::Gpu, "gpu0", QosClass::Standard, 8,
         1'000, 2'000);
    feed(scheduler, ExecutionBackend::DefaultAsync, "default",
         QosClass::Standard, 3, 5'000'000, 10'000'000);
    request = cpu_gpu_request(QosClass::Standard, false);  // 启发式选 CPU
    const RoutingDecision adaptive = advance_window(scheduler, request,
                                                    capabilities);
    expect_identical_to_default(adaptive,
                                baseline_scheduler.route(request, capabilities));
    EXPECT_EQ(adaptive.selected_backend, ExecutionBackend::DefaultAsync);
}

TEST(AdaptiveCpuGpuHistory, ConvergesToGpuBeyondHysteresisMargin) {
    AdaptiveScheduler scheduler{history_config()};
    const auto capabilities = gpu_capabilities();

    // 启发式基线（gpu_selected=false）选 CPU；GPU 侧端到端显著更快。
    // 每侧 8 条（= min_samples ≤ 2×min_samples）即翻转：
    //   CPU EWMA ≈ 15ms × 0.656 ≈ 9.8ms；GPU EWMA ≈ 0.3ms × 0.656 ≈ 0.2ms；
    //   0.2 < 9.8 × 0.75 = 7.4 → 超出 25% 滞回边际。
    feed(scheduler, ExecutionBackend::DefaultAsync, "default",
         QosClass::Standard, 8, 5'000'000, 10'000'000);
    feed(scheduler, ExecutionBackend::Gpu, "gpu0", QosClass::Standard, 8,
         100'000, 200'000);

    const RoutingDecision decision = advance_window(
        scheduler, cpu_gpu_request(QosClass::Standard, false), capabilities);
    EXPECT_EQ(decision.selected_backend, ExecutionBackend::Gpu);
    EXPECT_EQ(decision.selected_executor_name, "gpu0");
    EXPECT_EQ(decision.reason, RoutingReason::AdaptiveHistory);
    EXPECT_EQ(decision.status, RoutingStatus::Accepted);
    EXPECT_FALSE(decision.fell_back)
        << "adaptive choice is a normal in-intent outcome, not a fallback";
    EXPECT_NE(decision.diagnostics & RoutingDiagnostics::AdaptiveHistory, 0u);
    EXPECT_NE(decision.detail.find("beyond hysteresis margin"),
              std::string::npos);
}

TEST(AdaptiveCpuGpuHistory, ConvergesToCpuAndFlipsBackSymmetrically) {
    AdaptiveScheduler scheduler{history_config()};
    const auto capabilities = gpu_capabilities();

    // 阶段一：启发式基线（无 gpu_selected）选 GPU；CPU 侧显著更快 → 翻到 CPU。
    feed(scheduler, ExecutionBackend::Gpu, "gpu0", QosClass::Standard, 12,
         5'000'000, 10'000'000);
    feed(scheduler, ExecutionBackend::DefaultAsync, "default",
         QosClass::Standard, 12, 100'000, 200'000);
    RoutingDecision decision = advance_window(
        scheduler, cpu_gpu_request(QosClass::Standard), capabilities);
    EXPECT_EQ(decision.selected_backend, ExecutionBackend::DefaultAsync);
    EXPECT_EQ(decision.selected_executor_name, "default");
    EXPECT_EQ(decision.reason, RoutingReason::AdaptiveHistory);
    EXPECT_NE(decision.diagnostics & RoutingDiagnostics::AdaptiveHistory, 0u);
    EXPECT_NE(decision.detail.find("beyond hysteresis margin"),
              std::string::npos);

    // 阶段二：负载反转（GPU 慢、CPU 快互换）→ 翻回 GPU（对称性）。
    feed(scheduler, ExecutionBackend::DefaultAsync, "default",
         QosClass::Standard, 40, 5'000'000, 10'000'000);
    feed(scheduler, ExecutionBackend::Gpu, "gpu0", QosClass::Standard, 40,
         100'000, 200'000);
    decision = advance_window(scheduler, cpu_gpu_request(QosClass::Standard),
                              capabilities);
    EXPECT_EQ(decision.selected_backend, ExecutionBackend::Gpu);
    EXPECT_EQ(decision.reason, RoutingReason::AdaptiveHistory);
    EXPECT_NE(decision.diagnostics & RoutingDiagnostics::AdaptiveHistory, 0u);
}

TEST(AdaptiveCpuGpuHistory, WithinMarginMaintainsCurrentSideButMarksHistory) {
    AdaptiveScheduler scheduler{history_config()};
    const auto capabilities = gpu_capabilities();

    // 两侧 EWMA 完全相同的喂样序列（交替交错喂同值）→ 差值 0，边际内。
    for (size_t i = 0; i < 12; ++i) {
        scheduler.on_task_completed(make_feedback(
            ExecutionBackend::DefaultAsync, "default", QosClass::Standard,
            100'000, 200'000));
        scheduler.on_task_completed(make_feedback(
            ExecutionBackend::Gpu, "gpu0", QosClass::Standard, 100'000,
            200'000));
    }

    // 在用侧 CPU（gpu_selected=false）：边际内维持 CPU，但决策由历史驱动。
    RoutingDecision decision = advance_window(
        scheduler, cpu_gpu_request(QosClass::Standard, false), capabilities);
    EXPECT_EQ(decision.selected_backend, ExecutionBackend::DefaultAsync);
    EXPECT_EQ(decision.reason, RoutingReason::AdaptiveHistory);
    EXPECT_NE(decision.diagnostics & RoutingDiagnostics::AdaptiveHistory, 0u);
    EXPECT_NE(decision.detail.find("within hysteresis margin"),
              std::string::npos);

    // 在用侧 GPU（启发式默认）：同样边际内维持 GPU 并标记。
    decision = advance_window(scheduler, cpu_gpu_request(QosClass::Standard),
                              capabilities);
    EXPECT_EQ(decision.selected_backend, ExecutionBackend::Gpu);
    EXPECT_EQ(decision.selected_executor_name, "gpu0");
    EXPECT_EQ(decision.reason, RoutingReason::AdaptiveHistory);
    EXPECT_NE(decision.detail.find("within hysteresis margin"),
              std::string::npos);
}

TEST(AdaptiveCpuGpuHistory, AlternatingLoadDoesNotFlipPerDecision) {
    AdaptiveScheduler scheduler{history_config()};
    const auto capabilities = gpu_capabilities();

    // 快慢交替的负载：GPU 侧与 CPU 侧各自经历相同的快/慢序列 →
    // EWMA 收敛到同一水平，任何一侧都无法拉开 25% 边际。
    for (int phase = 0; phase < 5; ++phase) {
        const int64_t value = (phase % 2 == 0) ? 200'000 : 8'000'000;
        for (int i = 0; i < 12; ++i) {
            scheduler.on_task_completed(make_feedback(
                ExecutionBackend::DefaultAsync, "default",
                QosClass::Standard, value, 0));
            scheduler.on_task_completed(make_feedback(
                ExecutionBackend::Gpu, "gpu0", QosClass::Standard, value, 0));
        }
        const RoutingDecision decision = advance_window(
            scheduler, cpu_gpu_request(QosClass::Standard, false),
            capabilities);
        EXPECT_EQ(decision.selected_backend, ExecutionBackend::DefaultAsync)
            << "phase " << phase
            << ": balanced alternating load must not flip the decision";
        EXPECT_EQ(decision.reason, RoutingReason::AdaptiveHistory);
    }
}

TEST(AdaptiveCpuGpuHistory, DetailCarriesBothSideEwmaValues) {
    AdaptiveScheduler scheduler{history_config()};
    const auto capabilities = gpu_capabilities();
    feed(scheduler, ExecutionBackend::DefaultAsync, "default",
         QosClass::Standard, 12, 5'000'000, 10'000'000);
    feed(scheduler, ExecutionBackend::Gpu, "gpu0", QosClass::Standard, 12,
         100'000, 200'000);

    const RoutingDecision decision = advance_window(
        scheduler, cpu_gpu_request(QosClass::Standard, false), capabilities);
    ASSERT_EQ(decision.reason, RoutingReason::AdaptiveHistory);

    // detail 中的数值必须能从内嵌聚合器快照复原（可解释性契约）。
    const auto snapshot = scheduler.feedback_snapshot();
    const auto* cpu_entry = find_entry(*snapshot, ExecutionBackend::DefaultAsync,
                                       QosClass::Standard, "default");
    const auto* gpu_entry = find_entry(*snapshot, ExecutionBackend::Gpu,
                                       QosClass::Standard, "gpu0");
    ASSERT_NE(cpu_entry, nullptr);
    ASSERT_NE(gpu_entry, nullptr);
    const int64_t cpu_total = cpu_entry->ewma_queue_wait_ns +
                              cpu_entry->ewma_execution_duration_ns;
    const int64_t gpu_total = gpu_entry->ewma_queue_wait_ns +
                              gpu_entry->ewma_execution_duration_ns;
    EXPECT_NE(decision.detail.find("cpu ewma " + std::to_string(cpu_total) +
                                   "ns"),
              std::string::npos)
        << decision.detail;
    EXPECT_NE(decision.detail.find("gpu ewma " + std::to_string(gpu_total) +
                                   "ns"),
              std::string::npos)
        << decision.detail;
}

TEST(AdaptiveCpuGpuHistory, NoSubmittableGpuNeverMarksAdaptiveHistory) {
    AdaptiveScheduler scheduler{history_config()};
    DefaultScheduler baseline_scheduler;

    // 无任何 GPU 能力：即便双侧历史都达到 min_samples 也无从切换。
    feed(scheduler, ExecutionBackend::DefaultAsync, "default",
         QosClass::Standard, 12, 100'000, 200'000);
    feed(scheduler, ExecutionBackend::Gpu, "gpu0", QosClass::Standard, 12,
         100'000, 200'000);

    auto request = cpu_gpu_request(QosClass::Standard, std::nullopt,
                                   FallbackPolicy::AllowCpu);
    const RoutingDecision adaptive = advance_window(scheduler, request, {});
    expect_identical_to_default(adaptive,
                                baseline_scheduler.route(request, {}));
    EXPECT_EQ(adaptive.selected_backend, ExecutionBackend::DefaultAsync);
    EXPECT_TRUE(adaptive.fell_back);

    // GPU 能力存在但未运行（不可提交）且带 preferred_executor：同样不标记。
    AdaptiveScheduler stopped{history_config()};
    feed(stopped, ExecutionBackend::DefaultAsync, "default",
         QosClass::Standard, 12, 100'000, 200'000);
    feed(stopped, ExecutionBackend::Gpu, "gpu0", QosClass::Standard, 12,
         100'000, 200'000);
    auto stopped_request = cpu_gpu_request(
        QosClass::Standard, std::nullopt, FallbackPolicy::AllowCpu,
        std::optional<std::string>{"gpu0"});
    const RoutingDecision stopped_decision =
        advance_window(stopped, stopped_request,
                       stopped_gpu_capabilities("gpu0"));
    expect_identical_to_default(
        stopped_decision,
        baseline_scheduler.route(stopped_request,
                                 stopped_gpu_capabilities("gpu0")));
    EXPECT_EQ(stopped_decision.selected_backend, ExecutionBackend::DefaultAsync);
}

TEST(AdaptiveCpuGpuHistory, RequireRequestedBackendNeverFlipsToCpu) {
    // 显式用户约束优先于历史（缺陷 3 修复的回归守卫）：历史强烈偏 CPU 时
    // RequireRequestedBackend 的请求也不得翻向 CPU，且不标 AdaptiveHistory。
    AdaptiveScheduler scheduler{history_config()};
    DefaultScheduler baseline_scheduler;
    const auto capabilities = gpu_capabilities();

    // CPU 侧 EWMA ≈ 0.2ms ≪ GPU 侧 ≈ 10ms：无守卫时会翻向 CPU。
    feed(scheduler, ExecutionBackend::DefaultAsync, "default",
         QosClass::Standard, 12, 100'000, 200'000);
    feed(scheduler, ExecutionBackend::Gpu, "gpu0", QosClass::Standard, 12,
         5'000'000, 10'000'000);

    // 对照组：同一段历史下 NoFallback + 无偏好 → 历史翻到 CPU（守卫前的
    // 行为基线，证明历史确实强烈偏 CPU）。
    const RoutingDecision flip = advance_window(
        scheduler, cpu_gpu_request(QosClass::Standard), capabilities);
    EXPECT_EQ(flip.selected_backend, ExecutionBackend::DefaultAsync);
    EXPECT_EQ(flip.reason, RoutingReason::AdaptiveHistory);

    // RequireRequestedBackend（须带 preferred_executor）：决策保持 GPU，
    // 与 DefaultScheduler 逐字段一致、无 AdaptiveHistory 标记。
    auto constrained = cpu_gpu_request(
        QosClass::Standard, std::nullopt, FallbackPolicy::RequireRequestedBackend,
        std::optional<std::string>{"gpu0"});
    const RoutingDecision decision = advance_window(scheduler, constrained,
                                                    capabilities);
    expect_identical_to_default(
        decision, baseline_scheduler.route(constrained, capabilities));
    EXPECT_EQ(decision.selected_backend, ExecutionBackend::Gpu);
    EXPECT_EQ(decision.selected_executor_name, "gpu0");
    EXPECT_EQ(decision.reason, RoutingReason::PreferredExecutor);
    EXPECT_EQ(decision.diagnostics & RoutingDiagnostics::AdaptiveHistory, 0u);
}

TEST(AdaptiveCpuGpuHistory, OverlongGpuNameMatchesTruncatedHistoryKey) {
    AdaptiveScheduler scheduler{history_config()};
    // 60 字节 GPU 名：聚合器按 47 字节容量截断存键；查找端同样截断才能命中。
    const std::string long_name(60, 'g');
    const auto capabilities = gpu_capabilities(long_name);
    feed(scheduler, ExecutionBackend::DefaultAsync, "default",
         QosClass::Standard, 12, 5'000'000, 10'000'000);
    feed(scheduler, ExecutionBackend::Gpu, long_name, QosClass::Standard, 12,
         100'000, 200'000);

    const RoutingDecision decision = advance_window(
        scheduler, cpu_gpu_request(QosClass::Standard), capabilities);
    EXPECT_EQ(decision.reason, RoutingReason::AdaptiveHistory)
        << "truncated key must still match the fed history";
    // 决策携带的执行器名是能力快照原名（不截断）。
    EXPECT_EQ(decision.selected_executor_name, long_name);
}

// ---------------------------------------------------------------------------
// D. QoS 感知降载（窗口差分语义）
// ---------------------------------------------------------------------------

TEST(AdaptiveLoadShedding, EngagesAfterBreachWindowsAndRejectsLowerQos) {
    AdaptiveScheduler scheduler{shed_config()};

    // 窗口 1、2（各喂 40 条超阈样本）：未达 shed_breach_windows(3) → 不拒。
    feed_breach(scheduler, QosClass::Standard);
    RoutingDecision decision =
        advance_window(scheduler, auto_request(QosClass::BestEffort));
    EXPECT_EQ(decision.status, RoutingStatus::Accepted);
    EXPECT_EQ(decision.reason, RoutingReason::DefaultPolicy);
    EXPECT_FALSE(scheduler.load_shed_active(QosClass::Standard));
    feed_breach(scheduler, QosClass::Standard);
    decision = advance_window(scheduler, auto_request(QosClass::BestEffort));
    EXPECT_EQ(decision.status, RoutingStatus::Accepted);
    EXPECT_FALSE(scheduler.load_shed_active(QosClass::Standard));

    // 窗口 3：开闸，本条提交即被结构化拒绝。
    feed_breach(scheduler, QosClass::Standard);
    decision = advance_window(scheduler, auto_request(QosClass::BestEffort));
    EXPECT_EQ(decision.status, RoutingStatus::Rejected);
    EXPECT_EQ(decision.reason, RoutingReason::LoadShedding);
    EXPECT_NE(decision.diagnostics & RoutingDiagnostics::LoadShedding, 0u);
    EXPECT_EQ(decision.diagnostics & RoutingDiagnostics::AdaptiveHistory, 0u);
    EXPECT_EQ(decision.selected_backend, ExecutionBackend::DefaultAsync);
    EXPECT_EQ(decision.selected_executor_name, "default");
    // detail 可复原：超阈类、目标值、被拒类。
    EXPECT_NE(decision.detail.find("Standard"), std::string::npos);
    EXPECT_NE(decision.detail.find("100000000"), std::string::npos);
    EXPECT_NE(decision.detail.find("BestEffort"), std::string::npos);
    EXPECT_TRUE(scheduler.load_shed_active(QosClass::Standard));
}

TEST(AdaptiveLoadShedding, ClassBoundariesStrictlyLowerOnly) {
    AdaptiveScheduler scheduler{shed_config()};
    // 每个探针 = 一个带喂样的评估窗口（喂样维持 Standard 超阈证据，探针
    // 路由自身的评估在 maybe_shed 之前完成，闸门保持开）。前两个窗口
    // 未达 shed_breach_windows(3)，闸门未开：任何类都不被拒。
    feed_breach(scheduler, QosClass::Standard);
    EXPECT_EQ(advance_window(scheduler, auto_request(QosClass::BestEffort))
                  .status,
              RoutingStatus::Accepted)
        << "valve not open before breach_windows";
    feed_breach(scheduler, QosClass::Standard);
    EXPECT_EQ(advance_window(scheduler, auto_request(QosClass::BestEffort))
                  .status,
              RoutingStatus::Accepted)
        << "valve not open before breach_windows";
    // 窗口 3 起：开闸。同类与更高类永不互为降载对象。
    feed_breach(scheduler, QosClass::Standard);
    EXPECT_EQ(scheduler.route(auto_request(QosClass::Standard), {}).status,
              RoutingStatus::Accepted)
        << "same class is never shed";
    feed_breach(scheduler, QosClass::Standard);
    EXPECT_EQ(scheduler.route(auto_request(QosClass::Interactive), {}).status,
              RoutingStatus::Accepted)
        << "higher class is never shed";
    feed_breach(scheduler, QosClass::Standard);
    EXPECT_EQ(scheduler.route(auto_request(QosClass::Critical), {}).status,
              RoutingStatus::Accepted)
        << "higher class is never shed";
    feed_breach(scheduler, QosClass::Standard);
    EXPECT_EQ(scheduler.route(auto_request(QosClass::BestEffort), {}).status,
              RoutingStatus::Rejected)
        << "strictly lower class is shed";
    EXPECT_TRUE(scheduler.load_shed_active(QosClass::Standard));
}

TEST(AdaptiveLoadShedding, TargetZeroClassNeverParticipates) {
    AdaptiveScheduler scheduler{shed_config()};  // BestEffort 目标为 0
    // 大量 BestEffort 超阈样本：目标 0 → 该类永不评估。
    for (int i = 0; i < 3; ++i) {
        feed_breach(scheduler, QosClass::BestEffort);
        advance_window(scheduler, auto_request(QosClass::BestEffort));
    }
    EXPECT_FALSE(scheduler.load_shed_active(QosClass::BestEffort));

    // 独立调度器：只有 BestEffort 有样本时，没有任何类会被保护性拒绝
    //（BestEffort 是最低类，其超阈不构成对任何类的降载依据）。
    AdaptiveScheduler isolated{shed_config()};
    for (int i = 0; i < 3; ++i) {
        feed_breach(isolated, QosClass::BestEffort);
        const RoutingDecision decision = advance_window(
            isolated, auto_request(QosClass::Standard));
        EXPECT_EQ(decision.status, RoutingStatus::Accepted);
    }
}

TEST(AdaptiveLoadShedding, RecoversOnlyAfterCloseWindowsSymmetricHysteresis) {
    AdaptiveSchedulerConfig config = shed_config();
    config.shed_breach_windows = 2;
    config.shed_close_windows = 3;
    AdaptiveScheduler scheduler{config};

    feed_breach(scheduler, QosClass::Standard);
    advance_window(scheduler, auto_request(QosClass::BestEffort));
    feed_breach(scheduler, QosClass::Standard);
    advance_window(scheduler, auto_request(QosClass::BestEffort));
    ASSERT_TRUE(scheduler.load_shed_active(QosClass::Standard));

    // 恢复窗口（窗口内全部为快样本，p99 = 1e6 < 目标 × 0.5），但恢复窗口
    // 数不足 close_windows(3)。
    feed_recovery(scheduler, QosClass::Standard);
    advance_window(scheduler, auto_request(QosClass::BestEffort));
    EXPECT_TRUE(scheduler.load_shed_active(QosClass::Standard))
        << "one recovery window must not close the valve (close_windows=3)";
    feed_recovery(scheduler, QosClass::Standard);
    advance_window(scheduler, auto_request(QosClass::BestEffort));
    EXPECT_TRUE(scheduler.load_shed_active(QosClass::Standard))
        << "two recovery windows must not close the valve";

    // 第 3 个恢复窗口：关闸，恢复接受。
    feed_recovery(scheduler, QosClass::Standard);
    advance_window(scheduler, auto_request(QosClass::BestEffort));
    EXPECT_FALSE(scheduler.load_shed_active(QosClass::Standard));
    EXPECT_EQ(scheduler.route(auto_request(QosClass::BestEffort), {}).status,
              RoutingStatus::Accepted);
}

TEST(AdaptiveLoadShedding, TrafficStopsMustCloseValveFailOpen) {
    // 原缺陷回归守卫（缺陷 2 修复验证）：流量完全停止后，任意多的评估
    // 窗口（空差分 → 证据不足 → fail-open）必须把闸门解除，不能让陈旧
    // p99 无限期维持开闸。
    AdaptiveSchedulerConfig config = shed_config();
    config.shed_breach_windows = 2;
    AdaptiveScheduler scheduler{config};

    feed_breach(scheduler, QosClass::Standard);
    advance_window(scheduler, auto_request(QosClass::BestEffort));
    feed_breach(scheduler, QosClass::Standard);
    ASSERT_EQ(advance_window(scheduler, auto_request(QosClass::BestEffort))
                  .status,
              RoutingStatus::Rejected);
    ASSERT_TRUE(scheduler.load_shed_active(QosClass::Standard));

    // 流量停止：空差分窗口立即 fail-open。
    advance_window(scheduler, auto_request(QosClass::BestEffort));
    EXPECT_FALSE(scheduler.load_shed_active(QosClass::Standard))
        << "stale evidence must not keep the valve open once traffic stops";
    // 任意多窗口后仍为 false。
    for (int i = 0; i < 3; ++i) {
        advance_window(scheduler, auto_request(QosClass::BestEffort));
    }
    EXPECT_FALSE(scheduler.load_shed_active(QosClass::Standard));
    EXPECT_EQ(scheduler.route(auto_request(QosClass::BestEffort), {}).status,
              RoutingStatus::Accepted);

    // 样本恢复后状态机按连续证据重新进入。
    feed_breach(scheduler, QosClass::Standard);
    advance_window(scheduler, auto_request(QosClass::BestEffort));
    feed_breach(scheduler, QosClass::Standard);
    EXPECT_EQ(advance_window(scheduler, auto_request(QosClass::BestEffort))
                  .status,
              RoutingStatus::Rejected)
        << "state machine must re-engage on fresh consecutive evidence";
    EXPECT_TRUE(scheduler.load_shed_active(QosClass::Standard));
}

TEST(AdaptiveLoadShedding, DisabledSwitchNeverEngages) {
    AdaptiveSchedulerConfig config = shed_config();
    config.load_shedding_enabled = false;
    AdaptiveScheduler scheduler{config};
    for (int i = 0; i < 6; ++i) {
        feed_breach(scheduler, QosClass::Standard);
        const RoutingDecision decision = advance_window(
            scheduler, auto_request(QosClass::BestEffort));
        EXPECT_EQ(decision.status, RoutingStatus::Accepted);
    }
    EXPECT_FALSE(scheduler.load_shed_active(QosClass::Standard));
}

TEST(AdaptiveLoadShedding, NoEvidenceFailOpen) {
    AdaptiveScheduler scheduler{shed_config()};
    // 零样本：无证据 → 不开闸、不提升（宽容侧）。
    for (const QosClass qos : {QosClass::BestEffort, QosClass::Standard,
                               QosClass::Interactive, QosClass::Critical}) {
        EXPECT_FALSE(scheduler.load_shed_active(qos));
        EXPECT_FALSE(scheduler.priority_promoted(qos));
        EXPECT_EQ(scheduler.route(auto_request(qos), {}).status,
                  RoutingStatus::Accepted);
    }
}

TEST(AdaptiveLoadShedding, SkipsGpuDestinationDecisions) {
    AdaptiveScheduler scheduler{shed_config()};
    // 三个带喂样的窗口开闸（第 3 个窗口的 BestEffort 探针本身被拒）。
    for (int i = 0; i < 2; ++i) {
        feed_breach(scheduler, QosClass::Standard);
        advance_window(scheduler, auto_request(QosClass::BestEffort));
    }
    feed_breach(scheduler, QosClass::Standard);
    EXPECT_EQ(advance_window(scheduler, auto_request(QosClass::BestEffort))
                  .status,
              RoutingStatus::Rejected);
    ASSERT_TRUE(scheduler.load_shed_active(QosClass::Standard));

    // CpuOrGpu 启发式选 GPU 的决策目的地不是默认池，不受默认池降载影响
    //（带喂样窗口维持开闸态，探针只读路由结果）。
    feed_breach(scheduler, QosClass::Standard);
    const RoutingDecision decision = scheduler.route(
        cpu_gpu_request(QosClass::BestEffort), gpu_capabilities());
    EXPECT_EQ(decision.selected_backend, ExecutionBackend::Gpu);
    EXPECT_EQ(decision.status, RoutingStatus::Accepted);
    EXPECT_EQ(decision.reason, RoutingReason::GpuHeuristic);
}

// ---------------------------------------------------------------------------
// E. QoS→priority 有界提升（窗口差分语义）
// ---------------------------------------------------------------------------

TEST(AdaptivePriorityPromotion, EngagesOnlyAfterPromotionWindows) {
    AdaptiveSchedulerConfig config = shed_config();
    config.shed_breach_windows = 2;
    config.promotion_windows = 4;
    config.promotion_close_windows = 1'000'000;  // 测试轮内不取消
    config.priority_promotion_enabled = true;
    config.queue_wait_p99_target_ns = {0, 0, 100'000'000, 100'000'000};
    AdaptiveScheduler scheduler{config};

    TaskOptions interactive;
    interactive.qos = QosClass::Interactive;

    // 窗口 1、2：未达 promotion_windows → 不提升（窗口 2 时降载已开：
    // 降载与提升的窗口数区分度契约）。
    feed_breach(scheduler, QosClass::Interactive);
    advance_window(scheduler, auto_request(QosClass::Interactive));
    EXPECT_FALSE(scheduler.priority_promoted(QosClass::Interactive));
    feed_breach(scheduler, QosClass::Interactive);
    advance_window(scheduler, auto_request(QosClass::Interactive));
    EXPECT_FALSE(scheduler.priority_promoted(QosClass::Interactive));
    EXPECT_TRUE(scheduler.load_shed_active(QosClass::Interactive))
        << "shed engages at breach_windows(2) before promotion_windows(4)";

    // 窗口 3、4：达到 promotion_windows → 提升。
    feed_breach(scheduler, QosClass::Interactive);
    advance_window(scheduler, auto_request(QosClass::Interactive));
    feed_breach(scheduler, QosClass::Interactive);
    advance_window(scheduler, auto_request(QosClass::Interactive));
    EXPECT_TRUE(scheduler.priority_promoted(QosClass::Interactive));

    // 有界提升：Interactive 默认 HIGH(2) → 3；显式 priority 原样；
    // 未提升类原样。
    EXPECT_EQ(scheduler.effective_priority_for(interactive, 2), 3);
    TaskOptions explicit_priority = interactive;
    explicit_priority.priority = TaskPriority::LOW;
    explicit_priority.priority_set = true;
    EXPECT_EQ(scheduler.effective_priority_for(explicit_priority, 0), 0)
        << "explicit priority must never be overridden";
    TaskOptions standard;
    standard.qos = QosClass::Standard;
    EXPECT_EQ(scheduler.effective_priority_for(standard, 1), 1);
}

TEST(AdaptivePriorityPromotion, CriticalClassCappedAtCritical) {
    AdaptiveSchedulerConfig config = shed_config();
    config.shed_breach_windows = 1;
    config.promotion_windows = 1;
    config.promotion_close_windows = 1'000'000;
    config.priority_promotion_enabled = true;
    AdaptiveScheduler scheduler{config};

    feed_breach(scheduler, QosClass::Critical);
    advance_window(scheduler, auto_request(QosClass::Critical));
    ASSERT_TRUE(scheduler.priority_promoted(QosClass::Critical));

    TaskOptions critical;
    critical.qos = QosClass::Critical;
    // 封顶 CRITICAL：min(3+1, 3) = 3。
    EXPECT_EQ(scheduler.effective_priority_for(critical, 3),
              static_cast<int>(TaskPriority::CRITICAL));
}

TEST(AdaptivePriorityPromotion, RecoversOnlyAfterPromotionCloseWindows) {
    AdaptiveSchedulerConfig config = shed_config();
    config.shed_breach_windows = 2;
    config.promotion_windows = 2;
    config.promotion_close_windows = 4;
    config.priority_promotion_enabled = true;
    AdaptiveScheduler scheduler{config};

    feed_breach(scheduler, QosClass::Interactive);
    advance_window(scheduler, auto_request(QosClass::Interactive));
    feed_breach(scheduler, QosClass::Interactive);
    advance_window(scheduler, auto_request(QosClass::Interactive));
    ASSERT_TRUE(scheduler.priority_promoted(QosClass::Interactive));

    // 恢复窗口证据充足，但恢复窗口数不足 promotion_close_windows(4)。
    for (int i = 0; i < 3; ++i) {
        feed_recovery(scheduler, QosClass::Interactive);
        advance_window(scheduler, auto_request(QosClass::Interactive));
        EXPECT_TRUE(scheduler.priority_promoted(QosClass::Interactive))
            << "recovery window #" << i + 1
            << " must not cancel promotion (close=4)";
    }

    feed_recovery(scheduler, QosClass::Interactive);
    advance_window(scheduler, auto_request(QosClass::Interactive));
    EXPECT_FALSE(scheduler.priority_promoted(QosClass::Interactive));
    TaskOptions interactive;
    interactive.qos = QosClass::Interactive;
    EXPECT_EQ(scheduler.effective_priority_for(interactive, 2), 2);
}

TEST(AdaptivePriorityPromotion, TrafficStopsMustCancelPromotionFailOpen) {
    // 缺陷 2 回归守卫（提升侧）：流量停止后任意多评估窗口内提升必须解除。
    AdaptiveSchedulerConfig config = shed_config();
    config.shed_breach_windows = 1;
    config.promotion_windows = 1;
    config.promotion_close_windows = 1'000'000;
    config.priority_promotion_enabled = true;
    AdaptiveScheduler scheduler{config};

    feed_breach(scheduler, QosClass::Interactive);
    advance_window(scheduler, auto_request(QosClass::Interactive));
    ASSERT_TRUE(scheduler.priority_promoted(QosClass::Interactive));

    advance_window(scheduler, auto_request(QosClass::Interactive));  // 空窗口
    EXPECT_FALSE(scheduler.priority_promoted(QosClass::Interactive))
        << "stale evidence must not keep promotion once traffic stops";
    for (int i = 0; i < 3; ++i) {
        advance_window(scheduler, auto_request(QosClass::Interactive));
    }
    EXPECT_FALSE(scheduler.priority_promoted(QosClass::Interactive));
    TaskOptions interactive;
    interactive.qos = QosClass::Interactive;
    EXPECT_EQ(scheduler.effective_priority_for(interactive, 2), 2);
}

TEST(AdaptivePriorityPromotion, DisabledSwitchNeverPromotes) {
    AdaptiveSchedulerConfig config = shed_config();
    config.priority_promotion_enabled = false;
    AdaptiveScheduler scheduler{config};
    for (int i = 0; i < 6; ++i) {
        feed_breach(scheduler, QosClass::Interactive);
        advance_window(scheduler, auto_request(QosClass::Interactive));
    }
    EXPECT_FALSE(scheduler.priority_promoted(QosClass::Interactive));
    TaskOptions interactive;
    interactive.qos = QosClass::Interactive;
    EXPECT_EQ(scheduler.effective_priority_for(interactive, 2), 2);
}

// ---------------------------------------------------------------------------
// F. 可解释性与 0.6.1 回归
// ---------------------------------------------------------------------------

TEST(AdaptiveExplainability, FormatStateTextListsActiveClasses) {
    AdaptiveScheduler scheduler{shed_config()};
    for (int i = 0; i < 3; ++i) {
        feed_breach(scheduler, QosClass::Standard);
        advance_window(scheduler, auto_request(QosClass::BestEffort));
    }
    ASSERT_TRUE(scheduler.load_shed_active(QosClass::Standard));

    const std::string text = scheduler.format_state_text();
    EXPECT_NE(text.find("shed=[Standard]"), std::string::npos) << text;
    EXPECT_NE(text.find("promoted=[]"), std::string::npos) << text;
    EXPECT_NE(text.find("merges="), std::string::npos) << text;
}

TEST(AdaptiveExplainability, WantsFeedbackAlwaysTrue) {
    AdaptiveScheduler scheduler{history_config()};
    EXPECT_TRUE(scheduler.wants_feedback());
    EXPECT_TRUE(scheduler.feedback_snapshot() != nullptr);
}

TEST(AdaptiveExplainability, DefaultSchedulerPriorityPassthrough) {
    // 0.6.1 回归：DefaultScheduler 不覆写优先级、不需要反馈。
    DefaultScheduler scheduler;
    TaskOptions options;
    EXPECT_FALSE(options.priority_set);
    EXPECT_EQ(scheduler.effective_priority_for(options, 7), 7);
    EXPECT_EQ(scheduler.effective_priority_for(options, 0), 0);
    options.priority_set = true;
    options.priority = TaskPriority::CRITICAL;
    EXPECT_EQ(scheduler.effective_priority_for(options, 2), 2)
        << "default scheduler must ignore options entirely";
    EXPECT_FALSE(scheduler.wants_feedback());
}

// ---------------------------------------------------------------------------
// G. Executor 集成
// ---------------------------------------------------------------------------

TEST(AdaptiveExecutorIntegration, RouteTaskNormalizesLegacyLoadSheddingReason) {
    // 0.6.0 风格调度器只设 reason=LoadShedding 不设 status → route_task
    // 统一升级为 Rejected，并计入 load_shedding_rejected_count。
    Executor executor;
    init_executor(executor, 2);
    auto scripted = std::make_unique<ScriptedScheduler>();
    scripted->next = make_scripted_decision(RoutingReason::LoadShedding,
                                            RoutingDiagnostics::LoadShedding,
                                            ExecutionBackend::DefaultAsync);
    executor.set_scheduler(std::move(scripted));

    auto future = executor.submit_auto(kairo::task([] { return 42; }));
    EXPECT_THROW(future.get(), std::runtime_error);

    const auto decision = executor.get_last_routing_decision();
    ASSERT_TRUE(decision.has_value());
    EXPECT_EQ(decision->status, RoutingStatus::Rejected);
    EXPECT_EQ(decision->reason, RoutingReason::LoadShedding);

    const auto metrics = executor.get_scheduling_metrics();
    EXPECT_EQ(metrics.rejected_count, 1u);
    EXPECT_EQ(metrics.load_shedding_rejected_count, 1u);
    EXPECT_EQ(metrics.accepted_count, 0u);
    EXPECT_EQ(metrics.adaptive_history_count, 0u);
}

TEST(AdaptiveExecutorIntegration, AdaptiveHistoryDecisionCounted) {
    Executor executor;
    init_executor(executor, 2);
    auto scripted = std::make_unique<ScriptedScheduler>();
    scripted->next = make_scripted_decision(
        RoutingReason::AdaptiveHistory, RoutingDiagnostics::AdaptiveHistory,
        ExecutionBackend::DefaultAsync);
    executor.set_scheduler(std::move(scripted));

    auto future = executor.submit_auto(kairo::task([] { return 42; }));
    EXPECT_EQ(future.get(), 42);

    const auto metrics = executor.get_scheduling_metrics();
    EXPECT_EQ(metrics.accepted_count, 1u);
    EXPECT_EQ(metrics.rejected_count, 0u);
    EXPECT_EQ(metrics.adaptive_history_count, 1u);
    EXPECT_EQ(metrics.load_shedding_rejected_count, 0u);
}

TEST(AdaptiveExecutorIntegration, CpuOrGpuAdaptiveHistoryCpuChoiceExemptFromHeuristicRejection) {
    // 豁免规则：AdaptiveHistory（reason + diagnostics 位同时置位）驱动的
    // CPU 选择在 fallback != AllowCpu 时也不拒绝。
    Executor executor;
    init_executor(executor, 2);
    auto scripted = std::make_unique<ScriptedScheduler>();
    scripted->next = make_scripted_decision(
        RoutingReason::AdaptiveHistory, RoutingDiagnostics::AdaptiveHistory,
        ExecutionBackend::DefaultAsync);
    executor.set_scheduler(std::move(scripted));

    std::atomic<bool> ran_on_cpu{false};
    auto future = executor.submit_auto(
        cpu_gpu_task([&ran_on_cpu] { ran_on_cpu.store(true); },
                     [](void*) { FAIL() << "GPU path must not run"; }));
    ASSERT_NO_FATAL_FAILURE(future.get());
    EXPECT_TRUE(ran_on_cpu.load());

    const auto decision = executor.get_last_routing_decision();
    ASSERT_TRUE(decision.has_value());
    EXPECT_EQ(decision->status, RoutingStatus::Accepted);
    EXPECT_EQ(decision->reason, RoutingReason::AdaptiveHistory);
}

TEST(AdaptiveExecutorIntegration, CpuOrGpuHeuristicCpuStillRejectedWithoutAllowCpu) {
    // 对照：启发式 CPU 选择 + NoFallback → 0.6.1 拒绝规则逐位不变。
    Executor executor;
    init_executor(executor, 2);
    auto scripted = std::make_unique<ScriptedScheduler>();
    scripted->next = make_scripted_decision(RoutingReason::GpuHeuristic, 0u,
                                            ExecutionBackend::DefaultAsync);
    executor.set_scheduler(std::move(scripted));

    auto future = executor.submit_auto(
        cpu_gpu_task([] {}, [](void*) {}));
    EXPECT_THROW(future.get(), std::runtime_error);
    const auto decision = executor.get_last_routing_decision();
    ASSERT_TRUE(decision.has_value());
    EXPECT_EQ(decision->status, RoutingStatus::Rejected);

    // reason 单独为 AdaptiveHistory 而缺 diagnostics 位 → 不豁免（双条件）。
    Executor strict;
    init_executor(strict, 2);
    auto scripted2 = std::make_unique<ScriptedScheduler>();
    scripted2->next = make_scripted_decision(RoutingReason::AdaptiveHistory,
                                             0u, ExecutionBackend::DefaultAsync);
    strict.set_scheduler(std::move(scripted2));
    auto strict_future = strict.submit_auto(cpu_gpu_task([] {}, [](void*) {}));
    EXPECT_THROW(strict_future.get(), std::runtime_error)
        << "exemption requires reason AND diagnostics bit";
}

TEST(AdaptiveExecutorIntegration, InjectedAdaptiveSchedulerReceivesTaskBuilderSamples) {
    Executor executor;
    init_executor(executor, 4);
    auto scheduler = std::make_unique<AdaptiveScheduler>(history_config());
    auto* adaptive = scheduler.get();
    executor.set_scheduler(std::move(scheduler));
    EXPECT_TRUE(adaptive->wants_feedback());

    for (int i = 0; i < 6; ++i) {
        executor.submit_auto(kairo::task([] {}).name("ok"));
    }
    for (int i = 0; i < 2; ++i) {
        executor.submit_auto(kairo::task([] {
            throw std::runtime_error("boom");
        }).name("bad"));
    }
    ASSERT_TRUE(executor.wait_for_completion_for(std::chrono::seconds(10)));

    // merge_interval=0 已真实生效：feedback_snapshot() 立即重合并可见样本。
    const auto snapshot = adaptive->feedback_snapshot();
    const auto* entry = find_entry(*snapshot, ExecutionBackend::DefaultAsync,
                                   QosClass::Standard, "default");
    ASSERT_NE(entry, nullptr)
        << "TaskBuilder samples must flow into the embedded aggregator";
    EXPECT_EQ(entry->attempts, 8u);
    EXPECT_EQ(entry->failures, 2u)
        << "throwing tasks must report success=false";
    EXPECT_EQ(entry->deadline_misses, 0u);
    // 两侧时延测量都应有真实观测值（queue_wait = 提交 → 开始执行）。
    EXPECT_GT(entry->ewma_queue_wait_ns, 0);
    EXPECT_GT(entry->ewma_execution_duration_ns, 0);
    EXPECT_EQ(entry->queue_wait.total(), 8u);
    EXPECT_EQ(entry->execution_duration.total(), 8u);

    const auto metrics = executor.get_scheduling_metrics();
    EXPECT_EQ(metrics.feedback_reported_count, 8u);
    EXPECT_EQ(snapshot->total_attempts, 8u);
    EXPECT_EQ(snapshot->total_failures, 2u);
}

TEST(AdaptiveExecutorIntegration, CpuGpuTaskCpuPathProducesDefaultAsyncSamples) {
    Executor executor;
    init_executor(executor, 4);
    auto scheduler = std::make_unique<AdaptiveScheduler>(history_config());
    auto* adaptive = scheduler.get();
    executor.set_scheduler(std::move(scheduler));

    // 无 GPU 注册 + fallback=AllowCpu → 启发式回退 CPU（fell_back），
    // CPU 侧包装上报 (DefaultAsync, "default", qos)。
    executor.submit_auto(
        cpu_gpu_task([] {}, [](void*) {})
            .name("cg-ok")
            .qos(QosClass::Interactive)
            .fallback(FallbackPolicy::AllowCpu))
        .get();
    try {
        executor.submit_auto(
            cpu_gpu_task([] { throw std::runtime_error("cpu boom"); },
                         [](void*) {})
                .name("cg-bad")
                .fallback(FallbackPolicy::AllowCpu))
            .get();
        FAIL() << "throwing CPU callable must propagate";
    } catch (const std::runtime_error&) {
    }
    ASSERT_TRUE(executor.wait_for_completion_for(std::chrono::seconds(10)));

    const auto snapshot = adaptive->feedback_snapshot();
    const auto* interactive = find_entry(*snapshot, ExecutionBackend::DefaultAsync,
                                         QosClass::Interactive, "default");
    ASSERT_NE(interactive, nullptr);
    EXPECT_EQ(interactive->attempts, 1u);
    EXPECT_EQ(interactive->failures, 0u);
    const auto* standard = find_entry(*snapshot, ExecutionBackend::DefaultAsync,
                                      QosClass::Standard, "default");
    ASSERT_NE(standard, nullptr);
    EXPECT_EQ(standard->attempts, 1u);
    EXPECT_EQ(standard->failures, 1u)
        << "CpuGpuTask CPU-side exception must report success=false";
    EXPECT_GT(standard->ewma_queue_wait_ns, 0);
    EXPECT_GT(standard->ewma_execution_duration_ns, 0);

    const auto metrics = executor.get_scheduling_metrics();
    EXPECT_EQ(metrics.feedback_reported_count, 2u);
}

TEST(AdaptiveExecutorIntegration, PriorityPromotedCountGrowsInPromotedState) {
    Executor executor;
    init_executor(executor, 4);
    AdaptiveSchedulerConfig config = history_config();
    config.priority_promotion_enabled = true;
    config.shed_breach_windows = 1;
    config.promotion_windows = 1;
    config.queue_wait_p99_target_ns = {0, 0, 100'000'000, 100'000'000};
    config.min_window_samples = 8;
    auto scheduler = std::make_unique<AdaptiveScheduler>(config);
    auto* adaptive = scheduler.get();
    executor.set_scheduler(std::move(scheduler));

    // 直接向内嵌聚合器喂 Interactive 超阈历史，并推进一个评估窗口。
    feed_breach(*adaptive, QosClass::Interactive);
    advance_window(*adaptive, auto_request(QosClass::Interactive));
    ASSERT_TRUE(adaptive->priority_promoted(QosClass::Interactive));

    const auto metrics_before = executor.get_scheduling_metrics();
    ASSERT_EQ(metrics_before.priority_promoted_count, 0u);

    // 提升态下的默认优先级提交：Interactive HIGH(2) → CRITICAL(3)，
    // 被调整的提交计数 +1。（facade 先咨询 effective_priority_for 再
    // route_task；后续提交触发的空差分窗口会 fail-open 解除提升，但不
    // 影响本断言——计数发生在咨询时点。）
    executor.submit_auto(kairo::task([] {}).name("promoted")
                             .qos(QosClass::Interactive))
        .get();
    auto metrics = executor.get_scheduling_metrics();
    EXPECT_EQ(metrics.priority_promoted_count,
              metrics_before.priority_promoted_count + 1);

    // 显式 priority 的提交不计入（永不被覆盖）。
    executor.submit_auto(kairo::task([] {}).name("explicit")
                             .qos(QosClass::Interactive)
                             .priority(TaskPriority::LOW))
        .get();
    metrics = executor.get_scheduling_metrics();
    EXPECT_EQ(metrics.priority_promoted_count,
              metrics_before.priority_promoted_count + 1);

    // 未提升类（Standard）不计入。
    executor.submit_auto(kairo::task([] {}).name("plain").qos(QosClass::Standard))
        .get();
    metrics = executor.get_scheduling_metrics();
    EXPECT_EQ(metrics.priority_promoted_count,
              metrics_before.priority_promoted_count + 1);
    EXPECT_EQ(metrics.feedback_reported_count, 3u);
}

// ---------------------------------------------------------------------------
// H. 并发冒烟
// ---------------------------------------------------------------------------

TEST(AdaptiveConcurrency, RoutesAndFeedbackConcurrentlySmoke) {
    AdaptiveSchedulerConfig config = history_config();
    config.load_shedding_enabled = true;
    config.priority_promotion_enabled = true;
    AdaptiveScheduler scheduler{config};
    const auto capabilities = gpu_capabilities();

    std::atomic<int> route_failures{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < 2'000; ++i) {
                const QosClass qos = static_cast<QosClass>((t + i) % 4);
                RoutingDecision decision =
                    (i % 2 == 0)
                        ? scheduler.route(auto_request(qos), {})
                        : scheduler.route(
                              cpu_gpu_request(qos, (i % 4 == 1)
                                                       ? std::optional<bool>{true}
                                                       : std::nullopt),
                              capabilities);
                if (decision.detail.empty()) {
                    route_failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (int t = 0; t < 2; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < 4'000; ++i) {
                scheduler.on_task_completed(make_feedback(
                    t == 0 ? ExecutionBackend::DefaultAsync
                           : ExecutionBackend::Gpu,
                    t == 0 ? "default" : "gpu0",
                    static_cast<QosClass>(i % 4),
                    (i % 2 == 0) ? 100'000 : 500'000'000, 1'000));
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(route_failures.load(), 0);
    // 收尾自检：终态仍可产出一致决策与诊断文本。
    const RoutingDecision decision = scheduler.route(auto_request(), {});
    EXPECT_FALSE(decision.detail.empty());
    EXPECT_FALSE(scheduler.format_state_text().empty());
    EXPECT_TRUE(scheduler.feedback_snapshot() != nullptr);
}

TEST(AdaptiveConcurrency, ConcurrentRoutesDriveEvaluationStateMachine) {
    // 并发 route() 反复触发"合并 + 互斥锁内重评估"路径；路由线程同时
    // 持续喂样维持窗口证据（差分语义下空窗口会 fail-open），末期降载
    // 必须处于激活态（状态机在并发下不丢窗口、不崩溃）。
    AdaptiveScheduler scheduler{shed_config()};
    const auto capabilities = gpu_capabilities();

    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&scheduler, &capabilities, t] {
            for (int i = 0; i < 400; ++i) {
                // 每轮喂 10 条超阈样本（≥ min_window_samples=8）保证该
                // 线程视角的窗口有证据；跨线程合并后证据只多不少。
                feed_breach(scheduler, QosClass::Standard, 10);
                if (t % 2 == 0) {
                    (void)scheduler.route(auto_request(QosClass::BestEffort),
                                          {});
                } else {
                    (void)scheduler.route(
                        cpu_gpu_request(QosClass::Interactive), capabilities);
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    EXPECT_TRUE(scheduler.load_shed_active(QosClass::Standard))
        << "1600 evidence-carrying concurrent windows must engage the shed "
           "state machine";
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

