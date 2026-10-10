// M1 DefaultScheduler route() pipeline 化 行为等价测试
// (docs/design/roadmap_v0.7.md §2.2；独立验证代理编写，不修改库代码)。
//
// 目标：证明新四阶段 pipeline（约束过滤 → 候选生成 → 评分/排序 → 选择）
// 与 0.6.1 行为逐项一致。覆盖：
//  A. deadline 约束过滤语义（经 DefaultScheduler / SchedulingPipeline::
//     route 观察）：过期/未来/确定性锚点、约束拒绝字段形态、过滤器顺序。
//  B. GPU 资源约束过滤语义：设备不符/内存超量/未知总量/无能力放行/
//     非 CpuGpu 相关放行/preferred 选择核对目标。
//  C. ConstraintFilterChain 模板机制（短路顺序、计数、显式构造——
//     测试内过滤器与库内过滤器各验一遍；空链恒等见 I 组）。
//  D. kairo::detail 能力快照查找辅助（find_capability / gpu_submittable /
//     find_gpu_capability，0.6.1 语义冻结）。
//  E. IntentCandidateGenerator 决策树等价矩阵（经 TaskRouter::route ——
//     即 generate + select_first 的组合——与 0.6.1 预期逐项断言
//     status/reason/diagnostics/fell_back/task_name/selected_executor_name/
//     detail；拒绝字段形态 facade_submit_auto 兜底 + executor "default"）。
//  F. CandidatePlan / IdentityScoring（头内组件：惰性拒绝存储、take_refusal、
//     rank 不改变计划）。
//  G. affinity advisory（经 DefaultScheduler::route 观察：降级叠加、
//     reason 保留规则、拒绝不加位、只核对首个匹配后端能力）。
//  H. DefaultScheduler 端到端组合（契约测试之外的叠加场景）+
//     SchedulingPipeline<IdentityScoring> 与 DefaultScheduler 全矩阵等价 +
//     TaskRouter::route 参考路径（无过滤/无 advisory，过期 deadline 不拒绝
//     的 0.6.1 既有行为用断言锁定）。
//  I. 阶段组件直测（修复后公开阶段函数有可链接外联定义，可直接调用）：
//     DeadlineConstraintFilter / GpuResourceConstraintFilter 拒绝字段形态、
//     IntentCandidateGenerator::generate（primary 直写 + plan.count +
//     refusal 形态）、select_first（count=0 移出 / count=1 不动）、
//     apply_affinity_advisory 直测、含库内过滤器的链短路 + 空链恒等（F2
//     修复后 ConstraintFilterChain<> 可默认构造）。

#include <gtest/gtest.h>

#include <kairo/scheduler.hpp>
#include <kairo/scheduling_pipeline.hpp>
#include <kairo/task_options.hpp>
#include <kairo/task_router.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace kairo;
namespace sched = kairo::scheduling;

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;
using std::chrono::nanoseconds;

ExecutorCapability make_cpu_capability() {
    ExecutorCapability capability;
    capability.backend = ExecutionBackend::DefaultAsync;
    capability.name = "default";
    capability.registered = true;
    capability.running = true;
    capability.supports_future_submission = true;
    return capability;
}

ExecutorCapability make_gpu_capability(int device,
                                       size_t total_bytes = 0,
                                       size_t free_bytes = 0) {
    ExecutorCapability capability;
    capability.backend = ExecutionBackend::Gpu;
    capability.name = "gpu" + std::to_string(device);
    capability.registered = true;
    capability.running = true;
    capability.supports_gpu_kernel = true;
    capability.gpu_device = device;
    capability.gpu_memory_total_bytes = total_bytes;
    capability.gpu_memory_free_bytes = free_bytes;
    return capability;
}

TaskRouter::Request make_request(TaskOptions options, bool cpu_gpu_task) {
    return TaskRouter::Request{std::move(options), cpu_gpu_task, std::nullopt};
}

TaskRouter::Request make_request(TaskOptions options, bool cpu_gpu_task,
                                 std::optional<bool> gpu_selected) {
    return TaskRouter::Request{std::move(options), cpu_gpu_task,
                               gpu_selected};
}

TaskOptions make_options(const std::string& name, ExecutionIntent intent) {
    TaskOptions options;
    options.name = name;
    options.intent = intent;
    return options;
}

// 确定性 deadline 锚点（避开"恰好等于 now"的固有竞态，与契约测试同思路）：
// steady_clock 纪元 + 1ns 必然早已过去；time_point::max() 必然未到。
Clock::time_point definitely_expired_deadline() {
    return Clock::time_point{} + nanoseconds(1);
}

Clock::time_point far_future_deadline() {
    return Clock::time_point::max();
}

// 逐字段比较两条决策（timestamp 为观测元数据，不参与语义比较）。
void expect_decision_equal(const RoutingDecision& a, const RoutingDecision& b,
                           const std::string& what) {
    EXPECT_EQ(a.task_name, b.task_name) << what;
    EXPECT_EQ(a.requested_intent, b.requested_intent) << what;
    EXPECT_EQ(a.selected_backend, b.selected_backend) << what;
    EXPECT_EQ(a.selected_executor_name, b.selected_executor_name) << what;
    EXPECT_EQ(a.reason, b.reason) << what;
    EXPECT_EQ(a.status, b.status) << what;
    EXPECT_EQ(a.diagnostics, b.diagnostics) << what;
    EXPECT_EQ(a.fell_back, b.fell_back) << what;
    EXPECT_EQ(a.detail, b.detail) << what;
}

// 计数过滤器：验证链的短路顺序（首个拒绝后不得执行后续过滤器）。
class CountingFilter {
public:
    explicit CountingFilter(unsigned* counter) : counter_(counter) {}

    bool apply(const TaskRouter::Request&,
               const std::vector<ExecutorCapability>&,
               RoutingDecision&) const {
        ++*counter_;
        return true;
    }

private:
    unsigned* counter_;
};

// 固定拒绝过滤器（测试 TU 内定义，头内链模板可正常实例化调用）。
class RejectingFilter {
public:
    bool apply(const TaskRouter::Request&,
               const std::vector<ExecutorCapability>&,
               RoutingDecision& rejection) const {
        rejection.reason = RoutingReason::AdaptiveHistory;
        rejection.status = RoutingStatus::Rejected;
        rejection.detail = "rejecting filter fired";
        return false;
    }
};

// 常用能力快照集合。
std::vector<ExecutorCapability> caps_cpu_only() {
    return {make_cpu_capability()};
}

std::vector<ExecutorCapability> caps_healthy_gpu() {
    return {make_cpu_capability(), make_gpu_capability(0, 4096, 4096)};
}

std::vector<ExecutorCapability> caps_gpu_unregistered() {
    auto gpu = make_gpu_capability(0);
    gpu.registered = false;
    return {make_cpu_capability(), gpu};
}

std::vector<ExecutorCapability> caps_gpu_not_running() {
    auto gpu = make_gpu_capability(0);
    gpu.running = false;
    return {make_cpu_capability(), gpu};
}

std::vector<ExecutorCapability> caps_gpu_at_capacity() {
    auto gpu = make_gpu_capability(0, 4096, 4096);
    gpu.capacity_hint = 2;
    gpu.pending_work = 2;
    return {make_cpu_capability(), gpu};
}

std::vector<ExecutorCapability> caps_two_gpus() {
    return {make_cpu_capability(), make_gpu_capability(0, 4096, 4096),
            make_gpu_capability(1, 4096, 4096)};
}

}  // namespace

// ---------------------------------------------------------------------------
// A. deadline 约束过滤（经 DefaultScheduler / pipeline route 观察）
// ---------------------------------------------------------------------------

TEST(PipelineDeadlineStage, ExpiredPastEpochRejectedDeterministically) {
    DefaultScheduler scheduler;
    TaskOptions options = make_options("expired", ExecutionIntent::Auto);
    options.deadline = definitely_expired_deadline();

    const auto rejection = scheduler.route(make_request(options, false), {});
    EXPECT_EQ(rejection.status, RoutingStatus::Rejected);
    EXPECT_EQ(rejection.reason, RoutingReason::DeadlineExpired);
    EXPECT_EQ(rejection.diagnostics, RoutingDiagnostics::None);
    EXPECT_EQ(rejection.selected_backend, ExecutionBackend::DefaultAsync);
    EXPECT_EQ(rejection.detail,
              "scheduling: deadline already missed at submission");
}

TEST(PipelineDeadlineStage, FarFutureAcceptedCleanly) {
    DefaultScheduler scheduler;
    TaskOptions options = make_options("future", ExecutionIntent::Auto);
    options.deadline = far_future_deadline();

    const auto decision = scheduler.route(make_request(options, false), {});
    EXPECT_EQ(decision.status, RoutingStatus::Accepted);
    EXPECT_EQ(decision.reason, RoutingReason::DefaultPolicy);
    EXPECT_EQ(decision.diagnostics, RoutingDiagnostics::None);
    EXPECT_EQ(decision.detail, "default async policy");
}

TEST(PipelineDeadlineStage, NoDeadlineAccepted) {
    DefaultScheduler scheduler;
    const auto decision = scheduler.route(
        make_request(make_options("no-deadline", ExecutionIntent::Auto), false),
        {});
    EXPECT_EQ(decision.status, RoutingStatus::Accepted);
    EXPECT_NE(decision.reason, RoutingReason::DeadlineExpired);
}

// "now > deadline 才拒"的确定性锚点：now-1ms 必拒、now+50ms 必受。
// 恰好相等的用例在过滤内部会再次读时钟，结果由测量耗时决定，属固有
// 竞态，与契约测试一致地用两个锚点代替边界本身。
TEST(PipelineDeadlineStage, BoundaryAnchorsStrictlyPastOnly) {
    DefaultScheduler scheduler;

    TaskOptions in_time = make_options("anchor-in", ExecutionIntent::Auto);
    in_time.deadline = Clock::now() + milliseconds(50);
    const auto accepted = scheduler.route(make_request(in_time, false), {});
    EXPECT_EQ(accepted.status, RoutingStatus::Accepted);
    EXPECT_NE(accepted.reason, RoutingReason::DeadlineExpired);

    TaskOptions expired = make_options("anchor-out", ExecutionIntent::Auto);
    expired.deadline = Clock::now() - milliseconds(1);
    const auto rejected = scheduler.route(make_request(expired, false), {});
    EXPECT_EQ(rejected.status, RoutingStatus::Rejected);
    EXPECT_EQ(rejected.reason, RoutingReason::DeadlineExpired);
}

// 约束拒绝的字段形态：task_name 用 options.name 原文（无
// "facade_submit_auto" 兜底）、selected_executor_name 为空（尚无投递目标）。
TEST(PipelineDeadlineStage, RejectionShapeOriginalNameEmptyExecutor) {
    DefaultScheduler scheduler;
    TaskOptions named = make_options("original-name", ExecutionIntent::GeneralCpu);
    named.deadline = definitely_expired_deadline();
    const auto rejection = scheduler.route(make_request(named, false), {});
    EXPECT_EQ(rejection.task_name, "original-name");
    EXPECT_EQ(rejection.selected_executor_name, "");
    EXPECT_EQ(rejection.requested_intent, ExecutionIntent::GeneralCpu);
    EXPECT_EQ(rejection.selected_backend, ExecutionBackend::DefaultAsync);
    EXPECT_EQ(rejection.fell_back, false);

    TaskOptions unnamed = make_options("", ExecutionIntent::Auto);
    unnamed.deadline = definitely_expired_deadline();
    const auto unnamed_rejection = scheduler.route(make_request(unnamed, false), {});
    EXPECT_EQ(unnamed_rejection.task_name, "")
        << "constraint rejections must not fabricate facade_submit_auto";
    EXPECT_EQ(unnamed_rejection.selected_executor_name, "");
}

// 过期 deadline 与 GPU 设备不符同时存在：约束过滤按声明顺序短路，
// DeadlineExpired 先行（资源过滤器不再执行）。
TEST(PipelineDeadlineStage, DeadlineFilterWinsOverGpuMismatch) {
    DefaultScheduler scheduler;
    TaskOptions options = make_options("deadline-vs-gpu", ExecutionIntent::CpuOrGpu);
    options.deadline = definitely_expired_deadline();
    options.resources.gpu_device = 1;

    const auto decision = scheduler.route(make_request(options, true),
                                          caps_healthy_gpu());
    EXPECT_EQ(decision.status, RoutingStatus::Rejected);
    EXPECT_EQ(decision.reason, RoutingReason::DeadlineExpired)
        << "deadline filter runs before the GPU resource filter";
    EXPECT_EQ(decision.detail, "scheduling: deadline already missed at submission");
    EXPECT_EQ(decision.diagnostics, RoutingDiagnostics::None);
    EXPECT_EQ(decision.task_name, "deadline-vs-gpu");
    EXPECT_EQ(decision.selected_executor_name, "");
}

// ---------------------------------------------------------------------------
// B. GPU 资源约束过滤（经 DefaultScheduler / pipeline route 观察）
// ---------------------------------------------------------------------------

TEST(PipelineGpuResourceStage, DeviceMismatchRejectedWithResourceBit) {
    DefaultScheduler scheduler;
    TaskOptions options = make_options("wrong-device", ExecutionIntent::CpuOrGpu);
    options.resources.gpu_device = 1;

    const auto rejection = scheduler.route(make_request(options, true),
                                           caps_healthy_gpu());
    EXPECT_EQ(rejection.status, RoutingStatus::Rejected);
    EXPECT_EQ(rejection.reason, RoutingReason::BackendUnavailable);
    EXPECT_NE(rejection.diagnostics & RoutingDiagnostics::ResourceInfeasible, 0u);
    EXPECT_EQ(rejection.detail,
              "scheduling: requested gpu_device 1 but executor 'gpu0' runs on "
              "device 0");
}

TEST(PipelineGpuResourceStage, DeviceMatchAccepted) {
    DefaultScheduler scheduler;
    TaskOptions options = make_options("right-device", ExecutionIntent::CpuOrGpu);
    options.resources.gpu_device = 0;
    options.preferred_executor = "gpu0";

    const auto decision = scheduler.route(make_request(options, true),
                                          caps_healthy_gpu());
    EXPECT_EQ(decision.status, RoutingStatus::Accepted);
    EXPECT_EQ(decision.reason, RoutingReason::PreferredExecutor);
    EXPECT_EQ(decision.selected_backend, ExecutionBackend::Gpu);
    EXPECT_EQ(decision.diagnostics, RoutingDiagnostics::None);
}

TEST(PipelineGpuResourceStage, UnknownDeviceSkipsDeviceCheckButStillChecksMemory) {
    DefaultScheduler scheduler;
    // 与契约测试同口径：total 4096 / free 1024。
    const std::vector<ExecutorCapability> caps{
        make_cpu_capability(), make_gpu_capability(0, /*total=*/4096, /*free=*/1024)};

    TaskOptions over = make_options("unknown-device", ExecutionIntent::CpuOrGpu);
    over.resources.gpu_device = -1;
    over.resources.memory_bytes = 2048;

    // gpu_device < 0 → 设备核对跳过；内存 2048 > free 1024 仍要核对。
    const auto rejected = scheduler.route(make_request(over, true), caps);
    EXPECT_EQ(rejected.status, RoutingStatus::Rejected);
    EXPECT_EQ(rejected.reason, RoutingReason::CapacityPressure);

    TaskOptions fitting = make_options("unknown-device-fit", ExecutionIntent::CpuOrGpu);
    fitting.resources.gpu_device = -1;
    fitting.resources.memory_bytes = 1024;  // == free
    const auto accepted = scheduler.route(make_request(fitting, true), caps);
    EXPECT_EQ(accepted.status, RoutingStatus::Accepted);
    EXPECT_EQ(accepted.diagnostics, RoutingDiagnostics::None);
}

TEST(PipelineGpuResourceStage, MemoryOverAvailabilityRejectedWithResourceBit) {
    DefaultScheduler scheduler;
    const std::vector<ExecutorCapability> caps{
        make_cpu_capability(), make_gpu_capability(0, /*total=*/4096, /*free=*/1024)};
    TaskOptions options = make_options("memory-over", ExecutionIntent::CpuOrGpu);
    options.resources.memory_bytes = 2048;  // > free 1024

    const auto rejection = scheduler.route(make_request(options, true), caps);
    EXPECT_EQ(rejection.status, RoutingStatus::Rejected);
    EXPECT_EQ(rejection.reason, RoutingReason::CapacityPressure);
    EXPECT_NE(rejection.diagnostics & RoutingDiagnostics::ResourceInfeasible, 0u);
    EXPECT_EQ(rejection.detail,
              "scheduling: requested 2048 bytes exceeds GPU 'gpu0' availability");
    EXPECT_EQ(rejection.task_name, "memory-over");
    EXPECT_EQ(rejection.selected_executor_name, "");
}

TEST(PipelineGpuResourceStage, MemoryEqualAvailabilityAccepted) {
    DefaultScheduler scheduler;
    const std::vector<ExecutorCapability> caps{
        make_cpu_capability(), make_gpu_capability(0, /*total=*/4096, /*free=*/1024)};
    TaskOptions options = make_options("memory-exact", ExecutionIntent::CpuOrGpu);
    options.resources.memory_bytes = 1024;  // == free
    const auto decision = scheduler.route(make_request(options, true), caps);
    EXPECT_EQ(decision.status, RoutingStatus::Accepted);
}

TEST(PipelineGpuResourceStage, UnknownTotalIsPermissive) {
    DefaultScheduler scheduler;
    TaskOptions options = make_options("memory-unknown-total", ExecutionIntent::CpuOrGpu);
    options.resources.memory_bytes = size_t{1} << 30;  // 巨额声明；总量 0 = 未知

    auto caps = caps_healthy_gpu();
    caps[1].gpu_memory_total_bytes = 0;
    caps[1].gpu_memory_free_bytes = 0;
    const auto decision = scheduler.route(make_request(options, true), caps);
    EXPECT_EQ(decision.status, RoutingStatus::Accepted);
    EXPECT_EQ(decision.diagnostics, RoutingDiagnostics::None);
}

TEST(PipelineGpuResourceStage, ZeroFreeFallsBackToTotal) {
    DefaultScheduler scheduler;
    auto caps = caps_healthy_gpu();
    caps[1].gpu_memory_free_bytes = 0;  // available 回退为 total 4096

    TaskOptions over = make_options("free-zero-over", ExecutionIntent::CpuOrGpu);
    over.resources.memory_bytes = 4097;
    const auto rejected = scheduler.route(make_request(over, true), caps);
    EXPECT_EQ(rejected.status, RoutingStatus::Rejected);
    EXPECT_EQ(rejected.reason, RoutingReason::CapacityPressure);

    TaskOptions exact = make_options("free-zero-exact", ExecutionIntent::CpuOrGpu);
    exact.resources.memory_bytes = 4096;
    const auto accepted = scheduler.route(make_request(exact, true), caps);
    EXPECT_EQ(accepted.status, RoutingStatus::Accepted);
}

TEST(PipelineGpuResourceStage, NoMatchingGpuCapabilityPassesToIntentRouting) {
    DefaultScheduler scheduler;
    TaskOptions options = make_options("no-gpu-caps", ExecutionIntent::CpuOrGpu);
    options.resources.gpu_device = 0;
    options.resources.memory_bytes = 2048;

    // 无能力时放行，交给候选生成阶段：无 GPU 条目 + NoFallback → 意图路由
    // 的既有 BackendUnavailable 语义（而不是资源过滤的设备不符语义）。
    const auto decision = scheduler.route(make_request(options, true), {});
    EXPECT_EQ(decision.status, RoutingStatus::Rejected);
    EXPECT_EQ(decision.reason, RoutingReason::BackendUnavailable);
    EXPECT_EQ(decision.detail, "no GPU executor is available for CpuOrGpu task");
    EXPECT_EQ(decision.diagnostics, RoutingDiagnostics::None)
        << "intent-routed unavailability carries no ResourceInfeasible bit";
}

TEST(PipelineGpuResourceStage, NotCpuGpuRelevantPassesEvenWithDeclarations) {
    DefaultScheduler scheduler;
    // cpu_gpu_task = false 且 intent != CpuOrGpu：声明被忽略（放行）。
    TaskOptions auto_task = make_options("auto-with-gpu-decl", ExecutionIntent::Auto);
    auto_task.resources.gpu_device = 3;
    auto_task.resources.memory_bytes = size_t{1} << 20;
    const auto decision = scheduler.route(make_request(auto_task, false),
                                          caps_healthy_gpu());
    EXPECT_EQ(decision.status, RoutingStatus::Accepted);
    EXPECT_EQ(decision.reason, RoutingReason::DefaultPolicy);

    TaskOptions cpu_task = make_options("cpu-with-gpu-decl", ExecutionIntent::GeneralCpu);
    cpu_task.resources.memory_bytes = size_t{1} << 20;
    const auto cpu_decision = scheduler.route(make_request(cpu_task, false),
                                              caps_healthy_gpu());
    EXPECT_EQ(cpu_decision.status, RoutingStatus::Accepted);
    EXPECT_EQ(cpu_decision.reason, RoutingReason::ExplicitIntent);
}

TEST(PipelineGpuResourceStage, CpuOrGpuIntentWithoutFlagStillChecked) {
    DefaultScheduler scheduler;
    // cpu_gpu_task = false 但 intent == CpuOrGpu：gpu_relevant 判真。
    TaskOptions options = make_options("intent-only", ExecutionIntent::CpuOrGpu);
    options.resources.gpu_device = 1;
    const auto decision = scheduler.route(make_request(options, false),
                                          caps_healthy_gpu());
    EXPECT_EQ(decision.status, RoutingStatus::Rejected);
    EXPECT_EQ(decision.reason, RoutingReason::BackendUnavailable);
    EXPECT_NE(decision.diagnostics & RoutingDiagnostics::ResourceInfeasible, 0u);
}

TEST(PipelineGpuResourceStage, PreferredExecutorSelectsCheckedCapability) {
    DefaultScheduler scheduler;
    TaskOptions match = make_options("preferred-gpu", ExecutionIntent::CpuOrGpu);
    match.preferred_executor = "gpu1";
    match.resources.gpu_device = 1;  // 与 gpu1（device 1）匹配
    const auto accepted = scheduler.route(make_request(match, true),
                                          caps_two_gpus());
    EXPECT_EQ(accepted.status, RoutingStatus::Accepted);
    EXPECT_EQ(accepted.selected_executor_name, "gpu1");

    TaskOptions mismatch = make_options("preferred-gpu-mismatch", ExecutionIntent::CpuOrGpu);
    mismatch.preferred_executor = "gpu0";  // device 0
    mismatch.resources.gpu_device = 1;
    const auto rejected = scheduler.route(make_request(mismatch, true),
                                          caps_two_gpus());
    EXPECT_EQ(rejected.status, RoutingStatus::Rejected);
    EXPECT_NE(rejected.detail.find("executor 'gpu0' runs on device 0"),
              std::string::npos)
        << "detail: " << rejected.detail;
}

TEST(PipelineGpuResourceStage, ConstraintRejectionShapeOriginalNameEmptyExecutor) {
    DefaultScheduler scheduler;
    TaskOptions options = make_options("resource-shape", ExecutionIntent::CpuOrGpu);
    options.resources.gpu_device = 1;
    const auto rejection = scheduler.route(make_request(options, true),
                                           caps_healthy_gpu());
    EXPECT_EQ(rejection.task_name, "resource-shape");
    EXPECT_EQ(rejection.selected_executor_name, "");
    EXPECT_EQ(rejection.requested_intent, ExecutionIntent::CpuOrGpu);
    EXPECT_EQ(rejection.selected_backend, ExecutionBackend::DefaultAsync);
    EXPECT_EQ(rejection.fell_back, false);

    TaskOptions unnamed = make_options("", ExecutionIntent::CpuOrGpu);
    unnamed.resources.gpu_device = 1;
    const auto unnamed_rejection = scheduler.route(make_request(unnamed, true),
                                                   caps_healthy_gpu());
    EXPECT_EQ(unnamed_rejection.task_name, "");
    EXPECT_EQ(unnamed_rejection.selected_executor_name, "");
}

// ---------------------------------------------------------------------------
// C. ConstraintFilterChain 模板机制（测试内过滤器；验证头内折叠与短路）
// ---------------------------------------------------------------------------

TEST(PipelineFilterChain, ShortCircuitsAfterFirstRejection) {
    unsigned counter = 0;
    const sched::ConstraintFilterChain<RejectingFilter, CountingFilter> chain{
        RejectingFilter{}, CountingFilter{&counter}};

    RoutingDecision rejection;
    const bool ok = chain.apply(
        make_request(make_options("chain-short", ExecutionIntent::Auto), false),
        {}, rejection);
    EXPECT_FALSE(ok);
    EXPECT_EQ(counter, 0u)
        << "second filter must not run after the first one rejects";
    EXPECT_EQ(rejection.reason, RoutingReason::AdaptiveHistory);
    EXPECT_EQ(rejection.detail, "rejecting filter fired");
}

TEST(PipelineFilterChain, PassingChainRunsEveryFilterInOrder) {
    unsigned counter = 0;
    const sched::ConstraintFilterChain<CountingFilter, CountingFilter> chain{
        CountingFilter{&counter}, CountingFilter{&counter}};
    RoutingDecision rejection;
    const bool ok = chain.apply(
        make_request(make_options("chain-all-pass", ExecutionIntent::Auto), false),
        {}, rejection);
    EXPECT_TRUE(ok);
    EXPECT_EQ(counter, 2u) << "every filter runs when nothing rejects";
}

TEST(PipelineFilterChain, ExplicitCtorMovesStatefulFilters) {
    unsigned counter = 0;
    const sched::ConstraintFilterChain<CountingFilter> chain{
        CountingFilter{&counter}};
    RoutingDecision rejection;
    EXPECT_TRUE(chain.apply(
        make_request(make_options("stateful", ExecutionIntent::Auto), false),
        {}, rejection));
    EXPECT_EQ(counter, 1u);
}

// ---------------------------------------------------------------------------
// D. kairo::detail 能力快照查找辅助（0.6.1 语义冻结）
// ---------------------------------------------------------------------------

TEST(PipelineDetailHelpers, FindCapabilityMatchesBackendAndOptionalName) {
    const auto caps = caps_healthy_gpu();
    EXPECT_EQ(kairo::detail::find_capability(caps, ExecutionBackend::Gpu),
              &caps[1]);
    EXPECT_EQ(kairo::detail::find_capability(caps, ExecutionBackend::Gpu, "gpu0"),
              &caps[1]);
    EXPECT_EQ(kairo::detail::find_capability(caps, ExecutionBackend::Gpu, "ghost"),
              nullptr);
    EXPECT_EQ(kairo::detail::find_capability(caps, ExecutionBackend::LockFree),
              nullptr);
    // 同后端多条目：无名称取第一个。
    const auto two = caps_two_gpus();
    EXPECT_EQ(kairo::detail::find_capability(two, ExecutionBackend::Gpu),
              &two[1]);
    EXPECT_EQ(kairo::detail::find_capability(two, ExecutionBackend::Gpu, "gpu1"),
              &two[2]);
}

TEST(PipelineDetailHelpers, GpuSubmittableRequiresFullReadiness) {
    EXPECT_FALSE(kairo::detail::gpu_submittable(nullptr));

    ExecutorCapability gpu = make_gpu_capability(0);
    EXPECT_TRUE(kairo::detail::gpu_submittable(&gpu));

    gpu.registered = false;
    EXPECT_FALSE(kairo::detail::gpu_submittable(&gpu));
    gpu.registered = true;
    gpu.running = false;
    EXPECT_FALSE(kairo::detail::gpu_submittable(&gpu));
    gpu.running = true;
    gpu.supports_gpu_kernel = false;
    EXPECT_FALSE(kairo::detail::gpu_submittable(&gpu));
    gpu.supports_gpu_kernel = true;

    gpu.capacity_hint = 2;
    gpu.pending_work = 1;
    EXPECT_TRUE(kairo::detail::gpu_submittable(&gpu));
    gpu.pending_work = 2;  // 满容量
    EXPECT_FALSE(kairo::detail::gpu_submittable(&gpu));
    gpu.capacity_hint = 0;  // 容量未知 = 不设限
    gpu.pending_work = 100;
    EXPECT_TRUE(kairo::detail::gpu_submittable(&gpu));
}

TEST(PipelineDetailHelpers, FindGpuCapabilityPreferenceAndRunningFallback) {
    const auto two = caps_two_gpus();
    // 有偏好：按名称精确匹配（不要求 running）。
    EXPECT_EQ(kairo::detail::find_gpu_capability(two, std::optional<std::string>("gpu1")),
              &two[2]);
    EXPECT_EQ(kairo::detail::find_gpu_capability(two, std::optional<std::string>("ghost")),
              nullptr);

    // 无偏好：取首个 running 的 GPU（不要求唯一/registered/kernel）。
    EXPECT_EQ(kairo::detail::find_gpu_capability(two, std::nullopt), &two[1]);

    auto none_running = caps_two_gpus();
    none_running[1].running = false;
    none_running[2].running = false;
    EXPECT_EQ(kairo::detail::find_gpu_capability(none_running, std::nullopt),
              nullptr);

    // 无偏好但未注册条目在 running 时仍可被选中（与 0.6.1 一致：只看 running）。
    auto unregistered_running = caps_two_gpus();
    unregistered_running[1].registered = false;
    EXPECT_EQ(kairo::detail::find_gpu_capability(unregistered_running, std::nullopt),
              &unregistered_running[1]);
}

// ---------------------------------------------------------------------------
// E. 意图路由决策树等价矩阵（经 TaskRouter::route —— generate +
//    select_first 的组合，无过滤/无 advisory 的 0.6.1 参考路径）
// ---------------------------------------------------------------------------

TEST(IntentRoutingTree, NonCpuGpuAutoAcceptedDefaultPolicy) {
    TaskOptions options = make_options("auto-task", ExecutionIntent::Auto);
    const auto decision = TaskRouter{}.route(make_request(options, false), {});

    EXPECT_EQ(decision.status, RoutingStatus::Accepted);
    EXPECT_EQ(decision.reason, RoutingReason::DefaultPolicy);
    EXPECT_EQ(decision.diagnostics, RoutingDiagnostics::None);
    EXPECT_EQ(decision.selected_backend, ExecutionBackend::DefaultAsync);
    EXPECT_EQ(decision.selected_executor_name, "default");
    EXPECT_EQ(decision.fell_back, false);
    EXPECT_EQ(decision.task_name, "auto-task");
    EXPECT_EQ(decision.requested_intent, ExecutionIntent::Auto);
    EXPECT_EQ(decision.detail, "default async policy");
}

TEST(IntentRoutingTree, EmptyNameUsesFacadeFallbackOnAccept) {
    const auto decision = TaskRouter{}.route(
        make_request(make_options("", ExecutionIntent::Auto), false), {});
    EXPECT_EQ(decision.status, RoutingStatus::Accepted);
    EXPECT_EQ(decision.task_name, "facade_submit_auto");
    EXPECT_EQ(decision.selected_executor_name, "default");
}

TEST(IntentRoutingTree, NonCpuGpuGeneralCpuAcceptedExplicitIntent) {
    const auto decision = TaskRouter{}.route(
        make_request(make_options("cpu-task", ExecutionIntent::GeneralCpu), false),
        {});
    EXPECT_EQ(decision.status, RoutingStatus::Accepted);
    EXPECT_EQ(decision.reason, RoutingReason::ExplicitIntent);
    EXPECT_EQ(decision.selected_backend, ExecutionBackend::DefaultAsync);
    EXPECT_EQ(decision.selected_executor_name, "default");
    EXPECT_EQ(decision.fell_back, false);
    EXPECT_EQ(decision.detail, "GeneralCpu selects default async executor");
}

TEST(IntentRoutingTree, TypedApiIntentsRejectedWithFacadeShape) {
    const ExecutionIntent intents[] = {ExecutionIntent::LowLatency,
                                       ExecutionIntent::RealtimeQueue,
                                       ExecutionIntent::BlockingWorker,
                                       ExecutionIntent::CpuOrGpu};
    for (const ExecutionIntent intent : intents) {
        const auto decision = TaskRouter{}.route(
            make_request(make_options("typed", intent), false), {});
        EXPECT_EQ(decision.status, RoutingStatus::Rejected)
            << "intent index " << static_cast<int>(intent);
        EXPECT_EQ(decision.reason, RoutingReason::Rejected) << "intent index "
                                                            << static_cast<int>(intent);
        EXPECT_EQ(decision.task_name, "typed");
        EXPECT_EQ(decision.selected_executor_name, "default");
        EXPECT_EQ(decision.selected_backend, ExecutionBackend::DefaultAsync);
        EXPECT_EQ(decision.diagnostics, RoutingDiagnostics::None);
        EXPECT_EQ(decision.fell_back, false);
        EXPECT_EQ(decision.detail, "task intent requires a typed submission API");
    }
}

TEST(IntentRoutingTree, CpuGpuNoGpuNoFallbackRejected) {
    const auto decision = TaskRouter{}.route(
        make_request(make_options("cg-nofb", ExecutionIntent::CpuOrGpu), true),
        caps_cpu_only());
    EXPECT_EQ(decision.status, RoutingStatus::Rejected);
    EXPECT_EQ(decision.reason, RoutingReason::BackendUnavailable);
    EXPECT_EQ(decision.detail, "no GPU executor is available for CpuOrGpu task");
    EXPECT_EQ(decision.selected_executor_name, "default");
    EXPECT_EQ(decision.selected_backend, ExecutionBackend::DefaultAsync);
    EXPECT_EQ(decision.fell_back, false);
    EXPECT_EQ(decision.diagnostics, RoutingDiagnostics::None);
}

TEST(IntentRoutingTree, CpuGpuNoGpuAllowCpuDegraded) {
    TaskOptions options = make_options("cg-allow", ExecutionIntent::CpuOrGpu);
    options.fallback = FallbackPolicy::AllowCpu;
    const auto decision = TaskRouter{}.route(make_request(options, true),
                                             caps_cpu_only());
    EXPECT_EQ(decision.status, RoutingStatus::AcceptedDegraded);
    EXPECT_EQ(decision.reason, RoutingReason::FallbackPolicy);
    EXPECT_EQ(decision.fell_back, true);
    EXPECT_EQ(decision.selected_backend, ExecutionBackend::DefaultAsync);
    EXPECT_EQ(decision.selected_executor_name, "default");
    EXPECT_EQ(decision.diagnostics, RoutingDiagnostics::None)
        << "fallback degradation carries no diagnostics bit in 0.6.1";
    EXPECT_EQ(decision.detail,
              "GPU unavailable; falling back to default async executor");
}

TEST(IntentRoutingTree, RequireBackendWithoutPreferredRejected) {
    TaskOptions options = make_options("cg-req-nopref", ExecutionIntent::CpuOrGpu);
    options.fallback = FallbackPolicy::RequireRequestedBackend;
    const auto decision = TaskRouter{}.route(make_request(options, true),
                                             caps_healthy_gpu());
    EXPECT_EQ(decision.status, RoutingStatus::Rejected);
    EXPECT_EQ(decision.reason, RoutingReason::Rejected);
    EXPECT_EQ(decision.detail,
              "RequireRequestedBackend requires preferred_executor");
    EXPECT_EQ(decision.task_name, "cg-req-nopref");
    EXPECT_EQ(decision.selected_executor_name, "default");
}

TEST(IntentRoutingTree, RequireBackendHealthyPreferredAccepted) {
    TaskOptions options = make_options("cg-req-ok", ExecutionIntent::CpuOrGpu);
    options.fallback = FallbackPolicy::RequireRequestedBackend;
    options.preferred_executor = "gpu0";
    const auto decision = TaskRouter{}.route(make_request(options, true),
                                             caps_healthy_gpu());
    EXPECT_EQ(decision.status, RoutingStatus::Accepted);
    EXPECT_EQ(decision.reason, RoutingReason::PreferredExecutor);
    EXPECT_EQ(decision.selected_backend, ExecutionBackend::Gpu);
    EXPECT_EQ(decision.selected_executor_name, "gpu0");
    EXPECT_EQ(decision.fell_back, false);
    EXPECT_EQ(decision.detail, "required GPU executor is available");
}

TEST(IntentRoutingTree, RequireBackendUnavailabilityReasonMatrix) {
    const std::vector<std::tuple<const char*, RoutingReason,
                                 std::vector<ExecutorCapability>>>
        states{{"unregistered", RoutingReason::BackendUnavailable,
                caps_gpu_unregistered()},
               {"not-running", RoutingReason::BackendNotRunning,
                caps_gpu_not_running()},
               {"at-capacity", RoutingReason::CapacityPressure,
                caps_gpu_at_capacity()}};
    for (const auto& [label, reason, caps] : states) {
        TaskOptions options = make_options(std::string("cg-req-") + label,
                                           ExecutionIntent::CpuOrGpu);
        options.fallback = FallbackPolicy::RequireRequestedBackend;
        options.preferred_executor = "gpu0";
        const auto decision = TaskRouter{}.route(make_request(options, true),
                                                 caps);
        EXPECT_EQ(decision.status, RoutingStatus::Rejected) << label;
        EXPECT_EQ(decision.reason, reason) << label;
        EXPECT_EQ(decision.detail,
                  "requested GPU executor is unavailable, stopped, or at capacity")
            << label;
        EXPECT_EQ(decision.selected_executor_name, "default") << label;
        EXPECT_EQ(decision.diagnostics, RoutingDiagnostics::None) << label;
    }
}

// 0.6.1 怪癖锁定：registered+running 但 supports_gpu_kernel == false 的
// 能力会落到 unavailable_reason 的最后一档（CapacityPressure）。
TEST(IntentRoutingTree, MissingGpuKernelFlagReportsCapacityPressure) {
    auto caps = caps_healthy_gpu();
    caps[1].supports_gpu_kernel = false;

    const auto decision = TaskRouter{}.route(
        make_request(make_options("cg-no-kernel", ExecutionIntent::CpuOrGpu), true),
        caps);
    EXPECT_EQ(decision.status, RoutingStatus::Rejected);
    EXPECT_EQ(decision.reason, RoutingReason::CapacityPressure);
    EXPECT_EQ(decision.detail, "no GPU executor is available for CpuOrGpu task");
}

TEST(IntentRoutingTree, NoFallbackUnavailabilityReasonMatrix) {
    using Caps = std::vector<ExecutorCapability>;
    const std::vector<std::tuple<const char*, RoutingReason, Caps>> expected{
        {"unregistered", RoutingReason::BackendUnavailable, caps_gpu_unregistered()},
        {"not-running", RoutingReason::BackendNotRunning, caps_gpu_not_running()},
        {"at-capacity", RoutingReason::CapacityPressure, caps_gpu_at_capacity()}};
    for (const auto& [label, reason, caps] : expected) {
        const auto decision = TaskRouter{}.route(
            make_request(make_options(std::string("cg-nofb-") + label,
                                      ExecutionIntent::CpuOrGpu),
                         true),
            caps);
        EXPECT_EQ(decision.status, RoutingStatus::Rejected) << label;
        EXPECT_EQ(decision.reason, reason) << label;
        EXPECT_EQ(decision.detail,
                  "no GPU executor is available for CpuOrGpu task") << label;
    }
}

TEST(IntentRoutingTree, AllowCpuDegradedAcrossUnavailableStates) {
    for (const auto& caps :
         {caps_gpu_unregistered(), caps_gpu_not_running(),
          caps_gpu_at_capacity()}) {
        TaskOptions options = make_options("cg-allow-x", ExecutionIntent::CpuOrGpu);
        options.fallback = FallbackPolicy::AllowCpu;
        const auto decision = TaskRouter{}.route(make_request(options, true),
                                                 caps);
        EXPECT_EQ(decision.status, RoutingStatus::AcceptedDegraded);
        EXPECT_EQ(decision.reason, RoutingReason::FallbackPolicy);
        EXPECT_EQ(decision.fell_back, true);
        EXPECT_EQ(decision.selected_executor_name, "default");
        EXPECT_EQ(decision.detail,
                  "GPU unavailable; falling back to default async executor");
    }
}

TEST(IntentRoutingTree, HeuristicSelectedFalseRoutesToCpuWithoutDegradation) {
    const auto decision = TaskRouter{}.route(
        make_request(make_options("cg-cpu-heur", ExecutionIntent::CpuOrGpu),
                     true, false),
        caps_healthy_gpu());
    EXPECT_EQ(decision.status, RoutingStatus::Accepted);
    EXPECT_EQ(decision.reason, RoutingReason::GpuHeuristic);
    EXPECT_EQ(decision.selected_backend, ExecutionBackend::DefaultAsync);
    EXPECT_EQ(decision.selected_executor_name, "default");
    EXPECT_EQ(decision.fell_back, false);
    EXPECT_EQ(decision.diagnostics, RoutingDiagnostics::None);
    EXPECT_EQ(decision.detail, "GPU scheduler selected CPU");
}

TEST(IntentRoutingTree, HeuristicSelectedTrueRoutesToGpu) {
    const auto decision = TaskRouter{}.route(
        make_request(make_options("cg-gpu-heur", ExecutionIntent::CpuOrGpu),
                     true, true),
        caps_healthy_gpu());
    EXPECT_EQ(decision.status, RoutingStatus::Accepted);
    EXPECT_EQ(decision.reason, RoutingReason::GpuHeuristic);
    EXPECT_EQ(decision.selected_backend, ExecutionBackend::Gpu);
    EXPECT_EQ(decision.selected_executor_name, "gpu0");
    EXPECT_EQ(decision.detail, "GPU scheduler selected GPU");
}

TEST(IntentRoutingTree, HeuristicNulloptBehavesLikeTrue) {
    const auto decision = TaskRouter{}.route(
        make_request(make_options("cg-gpu-null", ExecutionIntent::CpuOrGpu), true),
        caps_healthy_gpu());
    EXPECT_EQ(decision.status, RoutingStatus::Accepted);
    EXPECT_EQ(decision.reason, RoutingReason::GpuHeuristic);
    EXPECT_EQ(decision.selected_backend, ExecutionBackend::Gpu);
    EXPECT_EQ(decision.selected_executor_name, "gpu0");
    EXPECT_EQ(decision.detail, "GPU scheduler selected GPU");
}

TEST(IntentRoutingTree, PreferredHealthySelectedOverHeuristic) {
    TaskOptions options = make_options("cg-pref", ExecutionIntent::CpuOrGpu);
    options.preferred_executor = "gpu0";
    const auto decision = TaskRouter{}.route(
        make_request(options, true, std::nullopt), caps_healthy_gpu());
    EXPECT_EQ(decision.status, RoutingStatus::Accepted);
    EXPECT_EQ(decision.reason, RoutingReason::PreferredExecutor);
    EXPECT_EQ(decision.selected_backend, ExecutionBackend::Gpu);
    EXPECT_EQ(decision.selected_executor_name, "gpu0");
    EXPECT_EQ(decision.detail, "preferred GPU executor selected");
}

TEST(IntentRoutingTree, TwoGpusWithoutPreferredIsUnavailable) {
    TaskOptions options = make_options("cg-two-gpus", ExecutionIntent::CpuOrGpu);
    const auto rejected = TaskRouter{}.route(make_request(options, true),
                                             caps_two_gpus());
    EXPECT_EQ(rejected.status, RoutingStatus::Rejected);
    EXPECT_EQ(rejected.reason, RoutingReason::BackendUnavailable);

    options.fallback = FallbackPolicy::AllowCpu;
    const auto degraded = TaskRouter{}.route(make_request(options, true),
                                             caps_two_gpus());
    EXPECT_EQ(degraded.status, RoutingStatus::AcceptedDegraded);
    EXPECT_EQ(degraded.reason, RoutingReason::FallbackPolicy);
    EXPECT_EQ(degraded.fell_back, true);
}

TEST(IntentRoutingTree, TwoGpusWithPreferredBypassesCountCheck) {
    TaskOptions options = make_options("cg-two-pref", ExecutionIntent::CpuOrGpu);
    options.preferred_executor = "gpu1";
    const auto decision = TaskRouter{}.route(make_request(options, true),
                                             caps_two_gpus());
    EXPECT_EQ(decision.status, RoutingStatus::Accepted);
    EXPECT_EQ(decision.reason, RoutingReason::PreferredExecutor);
    EXPECT_EQ(decision.selected_executor_name, "gpu1");
}

TEST(IntentRoutingTree, PreferredNameNotFoundIsUnavailable) {
    TaskOptions options = make_options("cg-ghost", ExecutionIntent::CpuOrGpu);
    options.preferred_executor = "ghost";
    const auto nofb = TaskRouter{}.route(make_request(options, true),
                                         caps_healthy_gpu());
    EXPECT_EQ(nofb.status, RoutingStatus::Rejected);
    EXPECT_EQ(nofb.reason, RoutingReason::BackendUnavailable);

    options.fallback = FallbackPolicy::RequireRequestedBackend;
    const auto req = TaskRouter{}.route(make_request(options, true),
                                        caps_healthy_gpu());
    EXPECT_EQ(req.status, RoutingStatus::Rejected);
    EXPECT_EQ(req.reason, RoutingReason::BackendUnavailable);
    EXPECT_EQ(req.detail,
              "requested GPU executor is unavailable, stopped, or at capacity");
}

// 单 GPU 条目但未运行且无偏好：gpu_count == 1 仍能找到条目（count 检查
// 不要求 running），不可用原因能区分出 BackendNotRunning。
TEST(IntentRoutingTree, SingleNotRunningGpuWithoutPreferredReportsNotRunning) {
    auto caps = caps_gpu_not_running();
    caps.erase(caps.begin());  // 只留未运行 GPU，gpu_count == 1
    const auto decision = TaskRouter{}.route(
        make_request(make_options("cg-single-down", ExecutionIntent::CpuOrGpu), true),
        caps);
    EXPECT_EQ(decision.status, RoutingStatus::Rejected);
    EXPECT_EQ(decision.reason, RoutingReason::BackendNotRunning);
}

// intent == Auto 但 cpu_gpu_task == true：走 CpuGpu 树（无 GPU → 拒绝）。
TEST(IntentRoutingTree, CpuGpuFlagWithAutoIntentFollowsGpuTree) {
    const auto decision = TaskRouter{}.route(
        make_request(make_options("flag-auto", ExecutionIntent::Auto), true),
        caps_cpu_only());
    EXPECT_EQ(decision.status, RoutingStatus::Rejected);
    EXPECT_EQ(decision.reason, RoutingReason::BackendUnavailable);
    EXPECT_EQ(decision.detail, "no GPU executor is available for CpuOrGpu task");
    EXPECT_EQ(decision.requested_intent, ExecutionIntent::Auto);
}

// 空名称 CpuGpu 拒绝：facade_submit_auto 兜底（与约束拒绝形态相反）。
TEST(IntentRoutingTree, CpuGpuRefusalEmptyNameFacadeFallback) {
    const auto decision = TaskRouter{}.route(
        make_request(make_options("", ExecutionIntent::CpuOrGpu), true),
        caps_cpu_only());
    EXPECT_EQ(decision.task_name, "facade_submit_auto");
    EXPECT_EQ(decision.selected_executor_name, "default");
    EXPECT_EQ(decision.requested_intent, ExecutionIntent::CpuOrGpu);
    EXPECT_EQ(decision.status, RoutingStatus::Rejected);
}

// ---------------------------------------------------------------------------
// F. TaskRouter::route 参考路径 + CandidatePlan / IdentityScoring（头内组件）
// ---------------------------------------------------------------------------

// 0.6.1 既有行为锁定：TaskRouter::route 不做 deadline 过滤——过期 deadline
// 的任务在该参考路径下不被拒绝（deadline 准入是 pipeline 约束过滤阶段的
// 职责，DefaultScheduler 才有）。
TEST(TaskRouterReferencePath, ExpiredDeadlineIsNotRejectedByTaskRouter) {
    TaskOptions options = make_options("router-no-deadline-filter",
                                       ExecutionIntent::Auto);
    options.deadline = definitely_expired_deadline();
    const auto decision = TaskRouter{}.route(make_request(options, false),
                                             caps_healthy_gpu());
    EXPECT_EQ(decision.status, RoutingStatus::Accepted)
        << "TaskRouter::route has no deadline filter (0.6.1 reference path)";
    EXPECT_EQ(decision.reason, RoutingReason::DefaultPolicy);
    EXPECT_EQ(decision.task_name, "router-no-deadline-filter");
}

TEST(TaskRouterReferencePath, RouteMatchesGeneratePlusSelectCompositionShape) {
    // TaskRouter::route 的输出即"generate + select_first"骨架的产物：
    // 拒绝路径保持 facade 字段形态、接受路径带 executor "default"。
    TaskOptions options = make_options("shape-probe", ExecutionIntent::CpuOrGpu);
    const auto routed = TaskRouter{}.route(make_request(options, true),
                                           caps_cpu_only());
    EXPECT_EQ(routed.selected_backend, ExecutionBackend::DefaultAsync);
    EXPECT_EQ(routed.selected_executor_name, "default");
    EXPECT_EQ(routed.diagnostics, RoutingDiagnostics::None);
}

static_assert(!std::is_copy_constructible_v<sched::CandidatePlan>,
              "CandidatePlan must not be copyable (raw refusal storage)");
static_assert(!std::is_move_constructible_v<sched::CandidatePlan>,
              "CandidatePlan must not be movable");

TEST(CandidatePlanStorage, FreshPlanHasZeroCount) {
    sched::CandidatePlan plan;
    EXPECT_EQ(plan.count, 0);
}

TEST(CandidatePlanStorage, MakeRefusalEngagesLazilyAndTakeRefusalMovesOut) {
    sched::CandidatePlan plan;
    RoutingDecision& refusal = plan.make_refusal();
    refusal.task_name = "manual";
    refusal.status = RoutingStatus::Rejected;
    refusal.reason = RoutingReason::CapacityPressure;
    refusal.selected_executor_name = "default";
    refusal.detail = "manual refusal";
    plan.count = 0;

    RoutingDecision decision;
    decision.detail = "sentinel";
    decision = plan.take_refusal();
    EXPECT_EQ(decision.status, RoutingStatus::Rejected);
    EXPECT_EQ(decision.reason, RoutingReason::CapacityPressure);
    EXPECT_EQ(decision.task_name, "manual");
    EXPECT_EQ(decision.selected_executor_name, "default");
    EXPECT_EQ(decision.detail, "manual refusal");
}

TEST(IdentityScoringStage, ExplicitConstructionCompilesAndRankIsNoOp) {
    const sched::SchedulingPipeline<sched::IdentityScoring> pipeline{
        sched::IdentityScoring{}};
    (void)pipeline;  // 显式 scoring 构造路径可编译（SchedulingPipeline 契约）

    sched::CandidatePlan accepted;
    accepted.count = 1;
    sched::CandidatePlan refused;
    refused.make_refusal().detail = "engaged";

    sched::IdentityScoring scoring;
    const TaskRouter::Request request = make_request(
        make_options("scored", ExecutionIntent::Auto), false);
    scoring.rank(accepted, request);
    scoring.rank(refused, request);
    EXPECT_EQ(accepted.count, 1);
    EXPECT_EQ(refused.count, 0);
    EXPECT_EQ(refused.take_refusal().detail, "engaged")
        << "rank must not disturb the engaged refusal storage";
}

// ---------------------------------------------------------------------------
// G. affinity advisory（经 DefaultScheduler::route 观察）
// ---------------------------------------------------------------------------

TEST(AffinityAdvisoryViaRoute, DisjointCpusDegradedWithBitAndReason) {
    DefaultScheduler scheduler;
    ExecutorCapability cpu = make_cpu_capability();
    cpu.bound_cpus = {0, 1};
    TaskOptions options = make_options("adv-disjoint", ExecutionIntent::Auto);
    options.affinity = AffinityHint{{9999}};

    const auto decision = scheduler.route(make_request(options, false), {cpu});
    EXPECT_EQ(decision.status, RoutingStatus::AcceptedDegraded);
    EXPECT_EQ(decision.reason, RoutingReason::AffinityMismatch);
    EXPECT_NE(decision.diagnostics & RoutingDiagnostics::AffinityMismatch, 0u);
    EXPECT_NE(decision.detail.find(
                  "AffinityMismatch: requested cpus do not intersect executor "
                  "'default' bound set"),
              std::string::npos)
        << "detail: " << decision.detail;
}

TEST(AffinityAdvisoryViaRoute, IntersectingCpusUntouched) {
    DefaultScheduler scheduler;
    ExecutorCapability cpu = make_cpu_capability();
    cpu.bound_cpus = {0, 1};
    TaskOptions options = make_options("adv-intersect", ExecutionIntent::Auto);
    options.affinity = AffinityHint{{0}};

    const auto decision = scheduler.route(make_request(options, false), {cpu});
    EXPECT_EQ(decision.status, RoutingStatus::Accepted);
    EXPECT_EQ(decision.reason, RoutingReason::DefaultPolicy);
    EXPECT_EQ(decision.diagnostics, RoutingDiagnostics::None);
    EXPECT_EQ(decision.detail, "default async policy");
}

TEST(AffinityAdvisoryViaRoute, EmptyRequestCpusNoOp) {
    DefaultScheduler scheduler;
    ExecutorCapability cpu = make_cpu_capability();
    cpu.bound_cpus = {0};
    const auto decision = scheduler.route(
        make_request(make_options("adv-empty", ExecutionIntent::Auto), false),
        {cpu});
    EXPECT_EQ(decision.diagnostics, RoutingDiagnostics::None);
    EXPECT_EQ(decision.status, RoutingStatus::Accepted);
}

TEST(AffinityAdvisoryViaRoute, EmptyCapabilitiesNoOp) {
    DefaultScheduler scheduler;
    TaskOptions options = make_options("adv-no-caps", ExecutionIntent::Auto);
    options.affinity = AffinityHint{{9999}};
    const auto decision = scheduler.route(make_request(options, false), {});
    EXPECT_EQ(decision.diagnostics, RoutingDiagnostics::None);
    EXPECT_EQ(decision.status, RoutingStatus::Accepted);
}

TEST(AffinityAdvisoryViaRoute, RejectedDecisionGainsNoAffinityDiagnostics) {
    DefaultScheduler scheduler;
    ExecutorCapability cpu = make_cpu_capability();
    cpu.bound_cpus = {0, 1};
    TaskOptions options = make_options("adv-rejected", ExecutionIntent::Auto);
    options.deadline = definitely_expired_deadline();
    options.affinity = AffinityHint{{9999}};

    const auto decision = scheduler.route(make_request(options, false), {cpu});
    EXPECT_EQ(decision.status, RoutingStatus::Rejected);
    EXPECT_EQ(decision.reason, RoutingReason::DeadlineExpired);
    EXPECT_EQ(decision.diagnostics, RoutingDiagnostics::None)
        << "rejected decisions must not carry affinity diagnostics";
}

// 已降级决策（fallback）+ 不相交 affinity：诊断位叠加，但 reason 保留
// 首个降级原因 FallbackPolicy，detail 以 "; " 追加 advisory。
TEST(AffinityAdvisoryViaRoute, AlreadyDegradedKeepsFirstReasonButAddsBit) {
    DefaultScheduler scheduler;
    ExecutorCapability cpu = make_cpu_capability();
    cpu.bound_cpus = {0, 1};
    TaskOptions options = make_options("adv-degraded", ExecutionIntent::CpuOrGpu);
    options.fallback = FallbackPolicy::AllowCpu;
    options.affinity = AffinityHint{{9999}};

    const auto decision = scheduler.route(make_request(options, true), {cpu});
    EXPECT_EQ(decision.status, RoutingStatus::AcceptedDegraded);
    EXPECT_EQ(decision.reason, RoutingReason::FallbackPolicy)
        << "first degradation reason must be preserved";
    EXPECT_EQ(decision.fell_back, true);
    EXPECT_NE(decision.diagnostics & RoutingDiagnostics::AffinityMismatch, 0u);
    EXPECT_NE(decision.detail.find("falling back to default async executor"),
              std::string::npos);
    EXPECT_NE(decision.detail.find("AffinityMismatch: requested cpus do not "
                                   "intersect executor 'default' bound set"),
              std::string::npos)
        << "detail: " << decision.detail;
    EXPECT_NE(decision.detail.find("; "), std::string::npos)
        << "appended advisory must use the '; ' separator";
}

// 0.6.1 形态锁定：只核对首个"同后端且绑核非空"的能力条目。
TEST(AffinityAdvisoryViaRoute, OnlyFirstMatchingBackendCapabilityConsulted) {
    DefaultScheduler scheduler;
    ExecutorCapability first = make_cpu_capability();
    first.name = "first";
    first.bound_cpus = {5, 6};
    ExecutorCapability second = make_cpu_capability();
    second.name = "second";
    second.bound_cpus = {0};
    TaskOptions options = make_options("adv-first", ExecutionIntent::Auto);
    options.affinity = AffinityHint{{0}};

    const auto decision = scheduler.route(make_request(options, false),
                                          {first, second});
    EXPECT_EQ(decision.status, RoutingStatus::AcceptedDegraded);
    EXPECT_NE(decision.detail.find("executor 'first' bound set"), std::string::npos)
        << "0.6.1 consults only the first matching-backend capability; detail: "
        << decision.detail;
}

TEST(AffinityAdvisoryViaRoute, EmptyBoundSetSkippedNextCapabilityConsulted) {
    DefaultScheduler scheduler;
    ExecutorCapability empty_bound = make_cpu_capability();
    empty_bound.name = "empty-bound";
    empty_bound.bound_cpus = {};
    ExecutorCapability bound = make_cpu_capability();
    bound.name = "bound";
    bound.bound_cpus = {7};
    TaskOptions options = make_options("adv-skip", ExecutionIntent::Auto);
    options.affinity = AffinityHint{{0}};

    const auto decision = scheduler.route(make_request(options, false),
                                          {empty_bound, bound});
    EXPECT_EQ(decision.status, RoutingStatus::AcceptedDegraded);
    EXPECT_NE(decision.detail.find("executor 'bound' bound set"), std::string::npos)
        << "detail: " << decision.detail;
}

TEST(AffinityAdvisoryViaRoute, ConsultsSelectedBackendOnly) {
    DefaultScheduler scheduler;
    ExecutorCapability cpu = make_cpu_capability();
    cpu.name = "default";
    cpu.bound_cpus = {3};  // CPU 后端绑核 {3}
    ExecutorCapability gpu = make_gpu_capability(0);
    gpu.name = "gpu0";
    gpu.bound_cpus = {7};  // GPU 后端绑核 {7}
    TaskOptions options = make_options("adv-backend", ExecutionIntent::CpuOrGpu);
    options.preferred_executor = "gpu0";
    options.affinity = AffinityHint{{7}};  // 与 GPU 绑核相交、与 CPU 绑核不相交

    // 决策落在 GPU 后端：绑核相交 → 干净接受、零诊断。
    const auto gpu_decision = scheduler.route(make_request(options, true),
                                              {cpu, gpu});
    EXPECT_EQ(gpu_decision.status, RoutingStatus::Accepted);
    EXPECT_EQ(gpu_decision.diagnostics, RoutingDiagnostics::None);

    // 同一请求决策落在 CPU 后端（无 GPU 条目 + AllowCpu 回退）：绑核
    // 不相交 → advisory 降级。回退降级先发生，reason 保留 FallbackPolicy，
    // diagnostics 叠加 AffinityMismatch 位。
    TaskOptions cpu_options = make_options("adv-backend-cpu", ExecutionIntent::CpuOrGpu);
    cpu_options.fallback = FallbackPolicy::AllowCpu;
    cpu_options.affinity = AffinityHint{{7}};
    const auto cpu_decision = scheduler.route(make_request(cpu_options, true),
                                              {cpu});
    EXPECT_EQ(cpu_decision.status, RoutingStatus::AcceptedDegraded);
    EXPECT_EQ(cpu_decision.reason, RoutingReason::FallbackPolicy)
        << "fallback degradation happened first; affinity only adds the bit";
    EXPECT_NE(cpu_decision.diagnostics & RoutingDiagnostics::AffinityMismatch, 0u);
}

// ---------------------------------------------------------------------------
// H. DefaultScheduler 端到端组合 + pipeline / scheduler / 参考路径全矩阵等价
// ---------------------------------------------------------------------------

TEST(DefaultSchedulerPipeline, HotPathAutoEmptyCapsAccepted) {
    DefaultScheduler scheduler;
    const auto decision = scheduler.route(
        make_request(make_options("hot-auto", ExecutionIntent::Auto), false), {});
    EXPECT_EQ(decision.status, RoutingStatus::Accepted);
    EXPECT_EQ(decision.reason, RoutingReason::DefaultPolicy);
    EXPECT_EQ(decision.selected_backend, ExecutionBackend::DefaultAsync);
    EXPECT_EQ(decision.selected_executor_name, "default");
    EXPECT_EQ(decision.detail, "default async policy");
    EXPECT_EQ(decision.diagnostics, RoutingDiagnostics::None);
}

// 组合降级叠加：CpuGpu + AllowCpu + GPU 未运行（fallback 降级）+ affinity
// 不相交（advisory 降级）→ status AcceptedDegraded，reason 保留首个降级
// 原因 FallbackPolicy，diagnostics 叠加 AffinityMismatch 位。
TEST(DefaultSchedulerPipeline, AllowCpuNotRunningDisjointAffinityDoubleDegradation) {
    DefaultScheduler scheduler;
    TaskOptions options = make_options("combo-double", ExecutionIntent::CpuOrGpu);
    options.fallback = FallbackPolicy::AllowCpu;
    options.affinity = AffinityHint{{9999}};

    ExecutorCapability cpu = make_cpu_capability();
    cpu.bound_cpus = {0, 1};
    auto caps = caps_gpu_not_running();
    caps[0] = cpu;

    const auto decision = scheduler.route(make_request(options, true), caps);
    EXPECT_EQ(decision.status, RoutingStatus::AcceptedDegraded);
    EXPECT_EQ(decision.reason, RoutingReason::FallbackPolicy)
        << "fallback degradation happens first; affinity only adds a bit";
    EXPECT_EQ(decision.fell_back, true);
    EXPECT_NE(decision.diagnostics & RoutingDiagnostics::AffinityMismatch, 0u);
    EXPECT_EQ(decision.diagnostics & RoutingDiagnostics::ResourceInfeasible, 0u);
    EXPECT_NE(decision.detail.find("falling back to default async executor"),
              std::string::npos);
    EXPECT_NE(decision.detail.find("AffinityMismatch"), std::string::npos);
}

// GPU 健康时 CpuGpu + preferred + affinity 不相交：接受到 GPU + advisory
// 降级（reason 切换为 AffinityMismatch，因为它是首个降级）。
TEST(DefaultSchedulerPipeline, HealthyGpuPreferredDisjointAffinityDegradedToGpu) {
    DefaultScheduler scheduler;
    TaskOptions options = make_options("combo-gpu-affinity", ExecutionIntent::CpuOrGpu);
    options.preferred_executor = "gpu0";
    options.affinity = AffinityHint{{9999}};

    auto caps = caps_healthy_gpu();
    caps[0].bound_cpus = {0, 1};
    caps[1].bound_cpus = {2, 3};

    const auto decision = scheduler.route(make_request(options, true), caps);
    EXPECT_EQ(decision.status, RoutingStatus::AcceptedDegraded);
    EXPECT_EQ(decision.reason, RoutingReason::AffinityMismatch);
    EXPECT_EQ(decision.selected_backend, ExecutionBackend::Gpu);
    EXPECT_EQ(decision.selected_executor_name, "gpu0");
    EXPECT_NE(decision.diagnostics & RoutingDiagnostics::AffinityMismatch, 0u);
    EXPECT_NE(decision.detail.find("executor 'gpu0' bound set"), std::string::npos);
}

// 启发式选择 CPU（gpu_selected = false）+ 相交 affinity：正常接受、
// 零诊断（heuristic-CPU 不是降级）。
TEST(DefaultSchedulerPipeline, HeuristicCpuWithIntersectingAffinityCleanAccept) {
    DefaultScheduler scheduler;
    TaskOptions options = make_options("combo-heur-cpu", ExecutionIntent::CpuOrGpu);
    options.affinity = AffinityHint{{2, 3}};

    auto caps = caps_healthy_gpu();
    caps[0].bound_cpus = {2, 3};

    const auto decision = scheduler.route(make_request(options, true, false), caps);
    EXPECT_EQ(decision.status, RoutingStatus::Accepted);
    EXPECT_EQ(decision.reason, RoutingReason::GpuHeuristic);
    EXPECT_EQ(decision.diagnostics, RoutingDiagnostics::None);
    EXPECT_EQ(decision.fell_back, false);
}

// DefaultScheduler 与 SchedulingPipeline<IdentityScoring>::route 在代表性
// 矩阵上逐字段等价（DefaultScheduler 应为 pipeline 的一行委派）。
TEST(DefaultSchedulerPipeline, DefaultSchedulerEqualsPipelineAcrossMatrix) {
    DefaultScheduler scheduler;
    sched::SchedulingPipeline<sched::IdentityScoring> pipeline;

    TaskOptions auto_opts = make_options("e-auto", ExecutionIntent::Auto);
    TaskOptions general = make_options("e-general", ExecutionIntent::GeneralCpu);
    TaskOptions typed = make_options("e-typed", ExecutionIntent::BlockingWorker);
    TaskOptions cg = make_options("e-cg", ExecutionIntent::CpuOrGpu);
    TaskOptions cg_allow = cg;
    cg_allow.fallback = FallbackPolicy::AllowCpu;
    cg_allow.affinity = AffinityHint{{9999}};
    TaskOptions cg_require = cg;
    cg_require.fallback = FallbackPolicy::RequireRequestedBackend;
    cg_require.preferred_executor = "gpu0";
    TaskOptions cg_expired = cg;
    cg_expired.deadline = definitely_expired_deadline();
    TaskOptions cg_overmem = cg;
    cg_overmem.resources.memory_bytes = size_t{1} << 40;
    TaskOptions cg_pref_disjoint = cg;
    cg_pref_disjoint.preferred_executor = "gpu0";
    cg_pref_disjoint.affinity = AffinityHint{{9999}};

    const std::vector<ExecutorCapability> healthy = caps_healthy_gpu();
    const std::vector<ExecutorCapability> cpu_only = caps_cpu_only();
    const std::vector<ExecutorCapability> not_running = caps_gpu_not_running();
    std::vector<ExecutorCapability> bound = caps_healthy_gpu();
    bound[0].bound_cpus = {0, 1};
    bound[1].bound_cpus = {2, 3};

    struct Case {
        const char* label;
        TaskOptions options;
        bool cpu_gpu_task;
        std::optional<bool> gpu_selected;
        const std::vector<ExecutorCapability>* caps;
    };
    const std::vector<Case> cases{
        {"auto", auto_opts, false, std::nullopt, &cpu_only},
        {"general", general, false, std::nullopt, &healthy},
        {"typed", typed, false, std::nullopt, &healthy},
        {"cg-healthy", cg, true, std::nullopt, &healthy},
        {"cg-healthy-false", cg, true, false, &healthy},
        {"cg-allow-notrunning", cg_allow, true, std::nullopt, &not_running},
        {"cg-require", cg_require, true, std::nullopt, &healthy},
        {"cg-expired", cg_expired, true, std::nullopt, &healthy},
        {"cg-overmem", cg_overmem, true, std::nullopt, &healthy},
        {"cg-pref-disjoint", cg_pref_disjoint, true, std::nullopt, &bound},
        {"auto-affinity-bound", auto_opts, false, std::nullopt, &bound},
        {"cg-bound-intersect", cg, true, std::nullopt, &bound},
    };

    int index = 0;
    for (const Case& test_case : cases) {
        const TaskRouter::Request request{test_case.options,
                                          test_case.cpu_gpu_task,
                                          test_case.gpu_selected};
        const auto from_scheduler = scheduler.route(request, *test_case.caps);
        const auto from_pipeline = pipeline.route(request, *test_case.caps);
        expect_decision_equal(
            from_scheduler, from_pipeline,
            std::string(test_case.label) + " (index " +
                std::to_string(index++) + ")");
    }
    SUCCEED() << "compared " << cases.size() << " scheduler/pipeline cases";
}

// DefaultScheduler / SchedulingPipeline / TaskRouter 三条入口在同一矩阵上
// 的一致性：有约束输入时 pipeline 系必须与参考路径区分开（过滤生效），
// 无约束输入时三者必须完全一致（0.6.1 意图路由结果）。
TEST(DefaultSchedulerPipeline, ThreeEntryPointsConsistencyMatrix) {
    DefaultScheduler scheduler;
    sched::SchedulingPipeline<sched::IdentityScoring> pipeline;

    TaskOptions plain = make_options("x-plain", ExecutionIntent::Auto);
    TaskOptions cg = make_options("x-cg", ExecutionIntent::CpuOrGpu);
    TaskOptions cg_allow = cg;
    cg_allow.fallback = FallbackPolicy::AllowCpu;
    TaskOptions cg_require_pref = cg;
    cg_require_pref.fallback = FallbackPolicy::RequireRequestedBackend;
    cg_require_pref.preferred_executor = "gpu0";

    const std::vector<ExecutorCapability> healthy = caps_healthy_gpu();
    const std::vector<ExecutorCapability> cpu_only = caps_cpu_only();
    const std::vector<ExecutorCapability> not_running = caps_gpu_not_running();

    struct Case {
        const char* label;
        TaskOptions options;
        bool cpu_gpu_task;
        std::optional<bool> gpu_selected;
        const std::vector<ExecutorCapability>* caps;
        // 三个入口是否必须一致（有 deadline / 资源声明时参考路径不同）。
        bool must_match_reference;
    };
    const std::vector<Case> cases{
        {"plain-auto", plain, false, std::nullopt, &cpu_only, true},
        {"cg-healthy", cg, true, std::nullopt, &healthy, true},
        {"cg-healthy-false", cg, true, false, &healthy, true},
        {"cg-allow-nocaps", cg_allow, true, std::nullopt, &cpu_only, true},
        {"cg-allow-notrunning", cg_allow, true, std::nullopt, &not_running, true},
        {"cg-require-pref", cg_require_pref, true, std::nullopt, &healthy, true},
    };

    int index = 0;
    for (const Case& test_case : cases) {
        const TaskRouter::Request request{test_case.options,
                                          test_case.cpu_gpu_task,
                                          test_case.gpu_selected};
        const auto from_scheduler = scheduler.route(request, *test_case.caps);
        const auto from_pipeline = pipeline.route(request, *test_case.caps);
        expect_decision_equal(from_scheduler, from_pipeline,
                              std::string(test_case.label) + " (index " +
                                  std::to_string(index++) + ")");
        const auto from_router = TaskRouter{}.route(request, *test_case.caps);
        if (test_case.must_match_reference) {
            expect_decision_equal(from_scheduler, from_router,
                                  std::string(test_case.label) +
                                      " vs reference (index " +
                                      std::to_string(index++) + ")");
        }
    }
    SUCCEED() << "compared " << cases.size() << " three-entry cases";
}

// ---------------------------------------------------------------------------
// I. 阶段组件直测（修复后公开阶段函数有可链接外联定义，直接调用）
// ---------------------------------------------------------------------------

// ---- I.1 DeadlineConstraintFilter 直测 ----

TEST(DirectDeadlineFilter, ExpiredPastEpochRejectedDeterministically) {
    TaskOptions options = make_options("expired", ExecutionIntent::Auto);
    options.deadline = definitely_expired_deadline();

    RoutingDecision rejection;
    const bool ok = sched::DeadlineConstraintFilter{}.apply(
        make_request(options, false), {}, rejection);
    EXPECT_FALSE(ok);
    EXPECT_EQ(rejection.status, RoutingStatus::Rejected);
    EXPECT_EQ(rejection.reason, RoutingReason::DeadlineExpired);
    EXPECT_EQ(rejection.diagnostics, RoutingDiagnostics::None);
    EXPECT_EQ(rejection.selected_backend, ExecutionBackend::DefaultAsync);
    EXPECT_EQ(rejection.detail,
              "scheduling: deadline already missed at submission");
}

TEST(DirectDeadlineFilter, FarFutureAcceptedWithoutTouchingRejection) {
    TaskOptions options = make_options("future", ExecutionIntent::Auto);
    options.deadline = far_future_deadline();

    RoutingDecision rejection;
    rejection.detail = "sentinel";  // 通过路径不得写 rejection
    const bool ok = sched::DeadlineConstraintFilter{}.apply(
        make_request(options, false), {}, rejection);
    EXPECT_TRUE(ok);
    EXPECT_EQ(rejection.detail, "sentinel");
    EXPECT_EQ(rejection.status, RoutingStatus::Accepted);  // 默认值未变
}

TEST(DirectDeadlineFilter, NoDeadlineAccepted) {
    RoutingDecision rejection;
    const bool ok = sched::DeadlineConstraintFilter{}.apply(
        make_request(make_options("no-deadline", ExecutionIntent::Auto), false),
        {}, rejection);
    EXPECT_TRUE(ok);
}

TEST(DirectDeadlineFilter, BoundaryAnchorsStrictlyPastOnly) {
    TaskOptions in_time = make_options("anchor-in", ExecutionIntent::Auto);
    in_time.deadline = Clock::now() + milliseconds(50);
    RoutingDecision rejection;
    EXPECT_TRUE(sched::DeadlineConstraintFilter{}.apply(
        make_request(in_time, false), {}, rejection));

    TaskOptions expired = make_options("anchor-out", ExecutionIntent::Auto);
    expired.deadline = Clock::now() - milliseconds(1);
    EXPECT_FALSE(sched::DeadlineConstraintFilter{}.apply(
        make_request(expired, false), {}, rejection));
    EXPECT_EQ(rejection.reason, RoutingReason::DeadlineExpired);
}

TEST(DirectDeadlineFilter, RejectionShapeOriginalNameEmptyExecutor) {
    TaskOptions named = make_options("original-name", ExecutionIntent::GeneralCpu);
    named.deadline = definitely_expired_deadline();
    RoutingDecision rejection;
    EXPECT_FALSE(sched::DeadlineConstraintFilter{}.apply(
        make_request(named, false), {}, rejection));
    EXPECT_EQ(rejection.task_name, "original-name");
    EXPECT_EQ(rejection.selected_executor_name, "");
    EXPECT_EQ(rejection.requested_intent, ExecutionIntent::GeneralCpu);
    EXPECT_EQ(rejection.selected_backend, ExecutionBackend::DefaultAsync);
    EXPECT_EQ(rejection.fell_back, false);

    TaskOptions unnamed = make_options("", ExecutionIntent::Auto);
    unnamed.deadline = definitely_expired_deadline();
    RoutingDecision unnamed_rejection;
    EXPECT_FALSE(sched::DeadlineConstraintFilter{}.apply(
        make_request(unnamed, false), {}, unnamed_rejection));
    EXPECT_EQ(unnamed_rejection.task_name, "")
        << "constraint rejections must not fabricate facade_submit_auto";
    EXPECT_EQ(unnamed_rejection.selected_executor_name, "");
}

TEST(DirectDeadlineFilter, CapabilitiesIrrelevant) {
    TaskOptions options = make_options("with-caps", ExecutionIntent::Auto);
    options.deadline = far_future_deadline();
    RoutingDecision rejection;
    EXPECT_TRUE(sched::DeadlineConstraintFilter{}.apply(
        make_request(options, false), caps_healthy_gpu(), rejection));
}

// ---- I.2 GpuResourceConstraintFilter 直测 ----

TEST(DirectGpuResourceFilter, DeviceMismatchRejectedWithResourceBit) {
    TaskOptions options = make_options("wrong-device", ExecutionIntent::CpuOrGpu);
    options.resources.gpu_device = 1;

    RoutingDecision rejection;
    const bool ok = sched::GpuResourceConstraintFilter{}.apply(
        make_request(options, true), caps_healthy_gpu(), rejection);
    EXPECT_FALSE(ok);
    EXPECT_EQ(rejection.status, RoutingStatus::Rejected);
    EXPECT_EQ(rejection.reason, RoutingReason::BackendUnavailable);
    EXPECT_NE(rejection.diagnostics & RoutingDiagnostics::ResourceInfeasible, 0u);
    EXPECT_EQ(rejection.detail,
              "scheduling: requested gpu_device 1 but executor 'gpu0' runs on "
              "device 0");
    EXPECT_EQ(rejection.task_name, "wrong-device");
    EXPECT_EQ(rejection.selected_executor_name, "");
}

TEST(DirectGpuResourceFilter, DeviceMatchPassesWithoutTouchingRejection) {
    TaskOptions options = make_options("right-device", ExecutionIntent::CpuOrGpu);
    options.resources.gpu_device = 0;
    RoutingDecision rejection;
    rejection.detail = "sentinel";
    EXPECT_TRUE(sched::GpuResourceConstraintFilter{}.apply(
        make_request(options, true), caps_healthy_gpu(), rejection));
    EXPECT_EQ(rejection.detail, "sentinel");
}

TEST(DirectGpuResourceFilter, UnknownDeviceSkipsDeviceCheckButStillChecksMemory) {
    // 与契约测试同口径：total 4096 / free 1024。
    const std::vector<ExecutorCapability> caps{
        make_cpu_capability(), make_gpu_capability(0, /*total=*/4096, /*free=*/1024)};

    TaskOptions over = make_options("unknown-device", ExecutionIntent::CpuOrGpu);
    over.resources.gpu_device = -1;
    over.resources.memory_bytes = 2048;

    RoutingDecision rejection;
    EXPECT_FALSE(sched::GpuResourceConstraintFilter{}.apply(
        make_request(over, true), caps, rejection));
    EXPECT_EQ(rejection.reason, RoutingReason::CapacityPressure);

    TaskOptions fitting = make_options("unknown-device-fit", ExecutionIntent::CpuOrGpu);
    fitting.resources.gpu_device = -1;
    fitting.resources.memory_bytes = 1024;  // == free
    RoutingDecision fit_rejection;
    EXPECT_TRUE(sched::GpuResourceConstraintFilter{}.apply(
        make_request(fitting, true), caps, fit_rejection));
}

TEST(DirectGpuResourceFilter, MemoryOverAvailabilityRejectedWithResourceBit) {
    const std::vector<ExecutorCapability> caps{
        make_cpu_capability(), make_gpu_capability(0, /*total=*/4096, /*free=*/1024)};
    TaskOptions options = make_options("memory-over", ExecutionIntent::CpuOrGpu);
    options.resources.memory_bytes = 2048;  // > free 1024

    RoutingDecision rejection;
    EXPECT_FALSE(sched::GpuResourceConstraintFilter{}.apply(
        make_request(options, true), caps, rejection));
    EXPECT_EQ(rejection.status, RoutingStatus::Rejected);
    EXPECT_EQ(rejection.reason, RoutingReason::CapacityPressure);
    EXPECT_NE(rejection.diagnostics & RoutingDiagnostics::ResourceInfeasible, 0u);
    EXPECT_EQ(rejection.detail,
              "scheduling: requested 2048 bytes exceeds GPU 'gpu0' availability");
    EXPECT_EQ(rejection.task_name, "memory-over");
    EXPECT_EQ(rejection.selected_executor_name, "");
}

TEST(DirectGpuResourceFilter, MemoryEqualAvailabilityPasses) {
    const std::vector<ExecutorCapability> caps{
        make_cpu_capability(), make_gpu_capability(0, /*total=*/4096, /*free=*/1024)};
    TaskOptions options = make_options("memory-exact", ExecutionIntent::CpuOrGpu);
    options.resources.memory_bytes = 1024;  // == free
    RoutingDecision rejection;
    EXPECT_TRUE(sched::GpuResourceConstraintFilter{}.apply(
        make_request(options, true), caps, rejection));
}

TEST(DirectGpuResourceFilter, UnknownTotalIsPermissive) {
    TaskOptions options = make_options("memory-unknown-total", ExecutionIntent::CpuOrGpu);
    options.resources.memory_bytes = size_t{1} << 30;  // 巨额声明；总量 0 = 未知

    auto caps = caps_healthy_gpu();
    caps[1].gpu_memory_total_bytes = 0;
    caps[1].gpu_memory_free_bytes = 0;
    RoutingDecision rejection;
    EXPECT_TRUE(sched::GpuResourceConstraintFilter{}.apply(
        make_request(options, true), caps, rejection));
}

TEST(DirectGpuResourceFilter, ZeroFreeFallsBackToTotal) {
    auto caps = caps_healthy_gpu();
    caps[1].gpu_memory_free_bytes = 0;  // available 回退为 total 4096

    TaskOptions over = make_options("free-zero-over", ExecutionIntent::CpuOrGpu);
    over.resources.memory_bytes = 4097;
    RoutingDecision rejection;
    EXPECT_FALSE(sched::GpuResourceConstraintFilter{}.apply(
        make_request(over, true), caps, rejection));
    EXPECT_EQ(rejection.reason, RoutingReason::CapacityPressure);

    TaskOptions exact = make_options("free-zero-exact", ExecutionIntent::CpuOrGpu);
    exact.resources.memory_bytes = 4096;
    RoutingDecision exact_rejection;
    EXPECT_TRUE(sched::GpuResourceConstraintFilter{}.apply(
        make_request(exact, true), caps, exact_rejection));
}

TEST(DirectGpuResourceFilter, NoMatchingGpuCapabilityPasses) {
    TaskOptions options = make_options("no-gpu-caps", ExecutionIntent::CpuOrGpu);
    options.resources.gpu_device = 0;
    options.resources.memory_bytes = 2048;

    RoutingDecision rejection;
    EXPECT_TRUE(sched::GpuResourceConstraintFilter{}.apply(
        make_request(options, true), {}, rejection));
    EXPECT_TRUE(sched::GpuResourceConstraintFilter{}.apply(
        make_request(options, true), caps_cpu_only(), rejection));
}

TEST(DirectGpuResourceFilter, NotCpuGpuRelevantPassesEvenWithDeclarations) {
    TaskOptions auto_task = make_options("auto-with-gpu-decl", ExecutionIntent::Auto);
    auto_task.resources.gpu_device = 3;
    auto_task.resources.memory_bytes = size_t{1} << 20;
    RoutingDecision rejection;
    EXPECT_TRUE(sched::GpuResourceConstraintFilter{}.apply(
        make_request(auto_task, false), caps_healthy_gpu(), rejection));

    TaskOptions cpu_task = make_options("cpu-with-gpu-decl", ExecutionIntent::GeneralCpu);
    cpu_task.resources.memory_bytes = size_t{1} << 20;
    EXPECT_TRUE(sched::GpuResourceConstraintFilter{}.apply(
        make_request(cpu_task, false), caps_healthy_gpu(), rejection));
}

TEST(DirectGpuResourceFilter, CpuOrGpuIntentWithoutFlagStillChecked) {
    TaskOptions options = make_options("intent-only", ExecutionIntent::CpuOrGpu);
    options.resources.gpu_device = 1;
    RoutingDecision rejection;
    EXPECT_FALSE(sched::GpuResourceConstraintFilter{}.apply(
        make_request(options, false), caps_healthy_gpu(), rejection));
    EXPECT_EQ(rejection.reason, RoutingReason::BackendUnavailable);
    EXPECT_NE(rejection.diagnostics & RoutingDiagnostics::ResourceInfeasible, 0u);
}

TEST(DirectGpuResourceFilter, PreferredExecutorSelectsCheckedCapability) {
    TaskOptions match = make_options("preferred-gpu", ExecutionIntent::CpuOrGpu);
    match.preferred_executor = "gpu1";
    match.resources.gpu_device = 1;  // 与 gpu1（device 1）匹配
    RoutingDecision rejection;
    EXPECT_TRUE(sched::GpuResourceConstraintFilter{}.apply(
        make_request(match, true), caps_two_gpus(), rejection));

    TaskOptions mismatch = make_options("preferred-gpu-mismatch", ExecutionIntent::CpuOrGpu);
    mismatch.preferred_executor = "gpu0";  // device 0
    mismatch.resources.gpu_device = 1;
    RoutingDecision mismatch_rejection;
    EXPECT_FALSE(sched::GpuResourceConstraintFilter{}.apply(
        make_request(mismatch, true), caps_two_gpus(), mismatch_rejection));
    EXPECT_NE(mismatch_rejection.detail.find("executor 'gpu0' runs on device 0"),
              std::string::npos)
        << "detail: " << mismatch_rejection.detail;
}

TEST(DirectGpuResourceFilter, RejectionShapeOriginalNameEmptyExecutor) {
    TaskOptions options = make_options("resource-shape", ExecutionIntent::CpuOrGpu);
    options.resources.gpu_device = 1;
    RoutingDecision rejection;
    EXPECT_FALSE(sched::GpuResourceConstraintFilter{}.apply(
        make_request(options, true), caps_healthy_gpu(), rejection));
    EXPECT_EQ(rejection.task_name, "resource-shape");
    EXPECT_EQ(rejection.selected_executor_name, "");
    EXPECT_EQ(rejection.requested_intent, ExecutionIntent::CpuOrGpu);
    EXPECT_EQ(rejection.selected_backend, ExecutionBackend::DefaultAsync);
    EXPECT_EQ(rejection.fell_back, false);

    TaskOptions unnamed = make_options("", ExecutionIntent::CpuOrGpu);
    unnamed.resources.gpu_device = 1;
    RoutingDecision unnamed_rejection;
    EXPECT_FALSE(sched::GpuResourceConstraintFilter{}.apply(
        make_request(unnamed, true), caps_healthy_gpu(), unnamed_rejection));
    EXPECT_EQ(unnamed_rejection.task_name, "");
    EXPECT_EQ(unnamed_rejection.selected_executor_name, "");
}

// ---- I.3 IntentCandidateGenerator::generate 直测（primary 直写 + 计数 +
//      refusal 形态）----

TEST(DirectCandidateGenerator, AutoAcceptedWritesPrimaryDirectly) {
    TaskOptions options = make_options("direct-auto", ExecutionIntent::Auto);
    RoutingDecision primary;
    primary.detail = "sentinel";  // 接受路径必须整体覆盖 primary
    sched::CandidatePlan plan;

    sched::IntentCandidateGenerator{}.generate(make_request(options, false), {},
                                               primary, plan);
    EXPECT_EQ(plan.count, 1);
    EXPECT_EQ(primary.status, RoutingStatus::Accepted);
    EXPECT_EQ(primary.reason, RoutingReason::DefaultPolicy);
    EXPECT_EQ(primary.task_name, "direct-auto");
    EXPECT_EQ(primary.selected_executor_name, "default");
    EXPECT_EQ(primary.detail, "default async policy");
    EXPECT_EQ(primary.fell_back, false);
}

TEST(DirectCandidateGenerator, GeneralCpuAcceptedDirect) {
    RoutingDecision primary;
    sched::CandidatePlan plan;
    sched::IntentCandidateGenerator{}.generate(
        make_request(make_options("direct-cpu", ExecutionIntent::GeneralCpu), false),
        {}, primary, plan);
    EXPECT_EQ(plan.count, 1);
    EXPECT_EQ(primary.reason, RoutingReason::ExplicitIntent);
    EXPECT_EQ(primary.detail, "GeneralCpu selects default async executor");
}

TEST(DirectCandidateGenerator, TypedApiRefusalLeavesPrimaryUntouched) {
    TaskOptions options = make_options("direct-typed", ExecutionIntent::LowLatency);
    RoutingDecision primary;
    primary.detail = "sentinel";  // 拒绝路径不得写 primary（保持哨兵）
    sched::CandidatePlan plan;

    sched::IntentCandidateGenerator{}.generate(make_request(options, false), {},
                                               primary, plan);
    EXPECT_EQ(plan.count, 0);
    EXPECT_EQ(primary.detail, "sentinel")
        << "refusal paths must not touch the primary decision storage";

    // 拒绝决策在 plan 内惰性构造，字段形态为意图路由形态（facade 兜底）。
    RoutingDecision decision;
    sched::select_first(plan, decision);
    EXPECT_EQ(decision.status, RoutingStatus::Rejected);
    EXPECT_EQ(decision.reason, RoutingReason::Rejected);
    EXPECT_EQ(decision.task_name, "direct-typed");
    EXPECT_EQ(decision.selected_executor_name, "default");
    EXPECT_EQ(decision.selected_backend, ExecutionBackend::DefaultAsync);
    EXPECT_EQ(decision.diagnostics, RoutingDiagnostics::None);
    EXPECT_EQ(decision.detail, "task intent requires a typed submission API");
}

TEST(DirectCandidateGenerator, AllowCpuDegradedWritesPrimaryDirectly) {
    TaskOptions options = make_options("direct-allow", ExecutionIntent::CpuOrGpu);
    options.fallback = FallbackPolicy::AllowCpu;
    RoutingDecision primary;
    sched::CandidatePlan plan;

    sched::IntentCandidateGenerator{}.generate(make_request(options, true),
                                               caps_cpu_only(), primary, plan);
    EXPECT_EQ(plan.count, 1);
    EXPECT_EQ(primary.status, RoutingStatus::AcceptedDegraded);
    EXPECT_EQ(primary.reason, RoutingReason::FallbackPolicy);
    EXPECT_EQ(primary.fell_back, true);
    EXPECT_EQ(primary.selected_backend, ExecutionBackend::DefaultAsync);
    EXPECT_EQ(primary.selected_executor_name, "default");
    EXPECT_EQ(primary.detail,
              "GPU unavailable; falling back to default async executor");
}

TEST(DirectCandidateGenerator, CpuGpuHealthyPreferredWritesGpuCandidate) {
    TaskOptions options = make_options("direct-pref", ExecutionIntent::CpuOrGpu);
    options.preferred_executor = "gpu0";
    RoutingDecision primary;
    sched::CandidatePlan plan;

    sched::IntentCandidateGenerator{}.generate(make_request(options, true),
                                               caps_healthy_gpu(), primary, plan);
    EXPECT_EQ(plan.count, 1);
    EXPECT_EQ(primary.status, RoutingStatus::Accepted);
    EXPECT_EQ(primary.reason, RoutingReason::PreferredExecutor);
    EXPECT_EQ(primary.selected_backend, ExecutionBackend::Gpu);
    EXPECT_EQ(primary.selected_executor_name, "gpu0");
    EXPECT_EQ(primary.detail, "preferred GPU executor selected");
}

TEST(DirectCandidateGenerator, CpuGpuRefusalShapeFacadeFallbackDirect) {
    RoutingDecision primary;
    sched::CandidatePlan plan;
    sched::IntentCandidateGenerator{}.generate(
        make_request(make_options("", ExecutionIntent::CpuOrGpu), true),
        caps_cpu_only(), primary, plan);
    EXPECT_EQ(plan.count, 0);
    EXPECT_EQ(primary.status, RoutingStatus::Accepted)  // 默认哨兵未被写
        << "refusal path must not touch primary";

    RoutingDecision decision;
    sched::select_first(plan, decision);
    EXPECT_EQ(decision.task_name, "facade_submit_auto");
    EXPECT_EQ(decision.selected_executor_name, "default");
    EXPECT_EQ(decision.status, RoutingStatus::Rejected);
    EXPECT_EQ(decision.detail, "no GPU executor is available for CpuOrGpu task");
}

// ---- I.4 select_first 直测 ----

TEST(DirectSelectFirst, CountOneLeavesTargetUntouched) {
    TaskOptions options = make_options("sel-one", ExecutionIntent::Auto);
    RoutingDecision primary;
    sched::CandidatePlan plan;
    sched::IntentCandidateGenerator{}.generate(make_request(options, false), {},
                                               primary, plan);
    ASSERT_EQ(plan.count, 1);

    RoutingDecision untouched;
    untouched.detail = "sentinel";
    sched::select_first(plan, untouched);
    EXPECT_EQ(untouched.detail, "sentinel")
        << "count == 1: select_first must not overwrite the target";
    EXPECT_EQ(untouched.status, RoutingStatus::Accepted);
    // primary 仍是最终候选。
    EXPECT_EQ(primary.reason, RoutingReason::DefaultPolicy);
    EXPECT_EQ(primary.detail, "default async policy");
}

TEST(DirectSelectFirst, CountZeroMovesRefusalIntoTarget) {
    RoutingDecision primary;
    sched::CandidatePlan plan;
    sched::IntentCandidateGenerator{}.generate(
        make_request(make_options("sel-zero", ExecutionIntent::LowLatency), false),
        {}, primary, plan);
    ASSERT_EQ(plan.count, 0);

    RoutingDecision decision;
    decision.detail = "sentinel";
    sched::select_first(plan, decision);
    EXPECT_EQ(decision.detail,
              "task intent requires a typed submission API")
        << "count == 0: refusal must be moved into the target";
    EXPECT_EQ(decision.status, RoutingStatus::Rejected);
    EXPECT_EQ(decision.task_name, "sel-zero");
    EXPECT_EQ(decision.selected_executor_name, "default");
}

// ---- I.5 apply_affinity_advisory 直测 ----

TEST(DirectAffinityAdvisory, DisjointDegradedWithBitAndReason) {
    ExecutorCapability cpu = make_cpu_capability();
    cpu.bound_cpus = {0, 1};
    TaskOptions options = make_options("da-disjoint", ExecutionIntent::Auto);
    options.affinity = AffinityHint{{9999}};

    RoutingDecision decision;
    decision.selected_backend = ExecutionBackend::DefaultAsync;
    sched::apply_affinity_advisory(decision, make_request(options, false), {cpu});
    EXPECT_EQ(decision.status, RoutingStatus::AcceptedDegraded);
    EXPECT_EQ(decision.reason, RoutingReason::AffinityMismatch);
    EXPECT_NE(decision.diagnostics & RoutingDiagnostics::AffinityMismatch, 0u);
    EXPECT_NE(decision.detail.find(
                  "AffinityMismatch: requested cpus do not intersect executor "
                  "'default' bound set"),
              std::string::npos);
}

TEST(DirectAffinityAdvisory, IntersectingUntouched) {
    ExecutorCapability cpu = make_cpu_capability();
    cpu.bound_cpus = {0, 1};
    TaskOptions options = make_options("da-intersect", ExecutionIntent::Auto);
    options.affinity = AffinityHint{{0}};

    RoutingDecision decision;
    decision.detail = "default async policy";
    sched::apply_affinity_advisory(decision, make_request(options, false), {cpu});
    EXPECT_EQ(decision.status, RoutingStatus::Accepted);
    EXPECT_EQ(decision.reason, RoutingReason::DefaultPolicy);
    EXPECT_EQ(decision.diagnostics, RoutingDiagnostics::None);
    EXPECT_EQ(decision.detail, "default async policy");
}

TEST(DirectAffinityAdvisory, EmptyCpusOrEmptyCapsOrRejectedAreNoOp) {
    ExecutorCapability cpu = make_cpu_capability();
    cpu.bound_cpus = {0};
    TaskOptions disjoint = make_options("da-noop", ExecutionIntent::Auto);
    disjoint.affinity = AffinityHint{{9999}};

    // 空 cpus。
    RoutingDecision empty_cpus;
    sched::apply_affinity_advisory(
        empty_cpus,
        make_request(make_options("da-empty-cpus", ExecutionIntent::Auto), false),
        {cpu});
    EXPECT_EQ(empty_cpus.diagnostics, RoutingDiagnostics::None);

    // 空 capabilities。
    RoutingDecision empty_caps;
    sched::apply_affinity_advisory(empty_caps, make_request(disjoint, false), {});
    EXPECT_EQ(empty_caps.diagnostics, RoutingDiagnostics::None);

    // 已拒绝决策。
    RoutingDecision rejected;
    rejected.status = RoutingStatus::Rejected;
    rejected.reason = RoutingReason::CapacityPressure;
    rejected.detail = "capacity problem";
    sched::apply_affinity_advisory(rejected, make_request(disjoint, false), {cpu});
    EXPECT_EQ(rejected.status, RoutingStatus::Rejected);
    EXPECT_EQ(rejected.reason, RoutingReason::CapacityPressure);
    EXPECT_EQ(rejected.diagnostics, RoutingDiagnostics::None);
    EXPECT_EQ(rejected.detail, "capacity problem");
}

TEST(DirectAffinityAdvisory, AlreadyDegradedKeepsFirstReasonAddsBit) {
    ExecutorCapability cpu = make_cpu_capability();
    cpu.bound_cpus = {0, 1};
    TaskOptions options = make_options("da-degraded", ExecutionIntent::CpuOrGpu);
    options.affinity = AffinityHint{{9999}};

    RoutingDecision decision;
    decision.status = RoutingStatus::AcceptedDegraded;
    decision.reason = RoutingReason::FallbackPolicy;
    decision.fell_back = true;
    decision.detail = "GPU unavailable; falling back to default async executor";

    sched::apply_affinity_advisory(decision, make_request(options, true), {cpu});
    EXPECT_EQ(decision.status, RoutingStatus::AcceptedDegraded);
    EXPECT_EQ(decision.reason, RoutingReason::FallbackPolicy)
        << "first degradation reason must be preserved";
    EXPECT_NE(decision.diagnostics & RoutingDiagnostics::AffinityMismatch, 0u);
    EXPECT_NE(decision.detail.find("; "), std::string::npos)
        << "appended advisory must use the '; ' separator";
    EXPECT_NE(decision.detail.find(
                  "AffinityMismatch: requested cpus do not intersect executor "
                  "'default' bound set"),
              std::string::npos);
}

TEST(DirectAffinityAdvisory, OnlyFirstMatchingBackendCapabilityConsulted) {
    ExecutorCapability first = make_cpu_capability();
    first.name = "first";
    first.bound_cpus = {5, 6};
    ExecutorCapability second = make_cpu_capability();
    second.name = "second";
    second.bound_cpus = {0};
    TaskOptions options = make_options("da-first", ExecutionIntent::Auto);
    options.affinity = AffinityHint{{0}};

    RoutingDecision decision;
    sched::apply_affinity_advisory(decision, make_request(options, false),
                                   {first, second});
    EXPECT_EQ(decision.status, RoutingStatus::AcceptedDegraded);
    EXPECT_NE(decision.detail.find("executor 'first' bound set"), std::string::npos)
        << "0.6.1 consults only the first matching-backend capability";
}

TEST(DirectAffinityAdvisory, EmptyBoundSetSkippedNextConsulted) {
    ExecutorCapability empty_bound = make_cpu_capability();
    empty_bound.name = "empty-bound";
    empty_bound.bound_cpus = {};
    ExecutorCapability bound = make_cpu_capability();
    bound.name = "bound";
    bound.bound_cpus = {7};
    TaskOptions options = make_options("da-skip", ExecutionIntent::Auto);
    options.affinity = AffinityHint{{0}};

    RoutingDecision decision;
    sched::apply_affinity_advisory(decision, make_request(options, false),
                                   {empty_bound, bound});
    EXPECT_EQ(decision.status, RoutingStatus::AcceptedDegraded);
    EXPECT_NE(decision.detail.find("executor 'bound' bound set"), std::string::npos);
}

TEST(DirectAffinityAdvisory, ConsultsSelectedBackendOnly) {
    ExecutorCapability cpu = make_cpu_capability();
    cpu.name = "cpu-bound";
    cpu.bound_cpus = {3};
    ExecutorCapability gpu = make_gpu_capability(0);
    gpu.name = "gpu0";
    gpu.bound_cpus = {7};
    TaskOptions options = make_options("da-backend", ExecutionIntent::CpuOrGpu);
    options.affinity = AffinityHint{{7}};  // 与 GPU 绑核相交、与 CPU 绑核不相交

    RoutingDecision gpu_decision;
    gpu_decision.selected_backend = ExecutionBackend::Gpu;
    sched::apply_affinity_advisory(gpu_decision, make_request(options, true),
                                   {cpu, gpu});
    EXPECT_EQ(gpu_decision.status, RoutingStatus::Accepted);
    EXPECT_EQ(gpu_decision.diagnostics, RoutingDiagnostics::None);

    RoutingDecision cpu_decision;
    cpu_decision.selected_backend = ExecutionBackend::DefaultAsync;
    sched::apply_affinity_advisory(cpu_decision, make_request(options, true),
                                   {cpu, gpu});
    EXPECT_EQ(cpu_decision.status, RoutingStatus::AcceptedDegraded);
    EXPECT_EQ(cpu_decision.reason, RoutingReason::AffinityMismatch);
    EXPECT_NE(cpu_decision.diagnostics & RoutingDiagnostics::AffinityMismatch, 0u);
}

// ---- I.6 ConstraintFilterChain：库内过滤器参与短路 + 空链恒等（F2）----

static_assert(std::is_default_constructible_v<sched::ConstraintFilterChain<>>,
              "F2 fix: empty chain must be default-constructible");

TEST(DirectFilterChainWithLibraryFilters, DeadlineShortCircuitsBeforeCounting) {
    unsigned counter = 0;
    const sched::ConstraintFilterChain<sched::DeadlineConstraintFilter,
                                       CountingFilter>
        chain{sched::DeadlineConstraintFilter{}, CountingFilter{&counter}};

    TaskOptions expired = make_options("lib-chain-short", ExecutionIntent::Auto);
    expired.deadline = definitely_expired_deadline();
    RoutingDecision rejection;
    EXPECT_FALSE(chain.apply(make_request(expired, false), {}, rejection));
    EXPECT_EQ(counter, 0u)
        << "second filter must not run after the library deadline filter rejects";
    EXPECT_EQ(rejection.reason, RoutingReason::DeadlineExpired);
    EXPECT_EQ(rejection.task_name, "lib-chain-short");
    EXPECT_EQ(rejection.selected_executor_name, "");

    TaskOptions future = make_options("lib-chain-pass", ExecutionIntent::Auto);
    future.deadline = far_future_deadline();
    RoutingDecision pass_rejection;
    EXPECT_TRUE(chain.apply(make_request(future, false), {}, pass_rejection));
    EXPECT_EQ(counter, 1u);
}

TEST(DirectFilterChainWithLibraryFilters, GpuResourceRejectsWhenFirst) {
    unsigned counter = 0;
    const sched::ConstraintFilterChain<CountingFilter,
                                       sched::GpuResourceConstraintFilter>
        chain{CountingFilter{&counter}, sched::GpuResourceConstraintFilter{}};

    TaskOptions options = make_options("lib-chain-gpu", ExecutionIntent::CpuOrGpu);
    options.resources.gpu_device = 1;
    RoutingDecision rejection;
    EXPECT_FALSE(chain.apply(make_request(options, true), caps_healthy_gpu(),
                             rejection));
    EXPECT_EQ(counter, 1u);
    EXPECT_EQ(rejection.reason, RoutingReason::BackendUnavailable);
    EXPECT_NE(rejection.diagnostics & RoutingDiagnostics::ResourceInfeasible, 0u);
}

TEST(DirectFilterChainWithLibraryFilters, DeclarationOrderDecidesWinningRejection) {
    TaskOptions options = make_options("lib-chain-both", ExecutionIntent::CpuOrGpu);
    options.deadline = definitely_expired_deadline();
    options.resources.gpu_device = 1;

    // deadline 在前：DeadlineExpired 胜（与 DefaultScheduler pipeline 顺序一致）。
    const sched::ConstraintFilterChain<sched::DeadlineConstraintFilter,
                                       sched::GpuResourceConstraintFilter>
        deadline_first{};
    RoutingDecision deadline_rejection;
    EXPECT_FALSE(deadline_first.apply(make_request(options, true),
                                      caps_healthy_gpu(), deadline_rejection));
    EXPECT_EQ(deadline_rejection.reason, RoutingReason::DeadlineExpired);

    // 资源过滤在前：BackendUnavailable 胜。
    const sched::ConstraintFilterChain<sched::GpuResourceConstraintFilter,
                                       sched::DeadlineConstraintFilter>
        gpu_first{};
    RoutingDecision gpu_rejection;
    EXPECT_FALSE(gpu_first.apply(make_request(options, true),
                                 caps_healthy_gpu(), gpu_rejection));
    EXPECT_EQ(gpu_rejection.reason, RoutingReason::BackendUnavailable);
}

// F2 修复验证：空包只走默认构造，实例化可编译；apply 为恒等（空包 &&
// 折叠为 true），即便请求本身会被 deadline 过滤器拒绝也放行。
TEST(DirectFilterChainWithLibraryFilters, EmptyChainIsDefaultConstructibleIdentity) {
    const sched::ConstraintFilterChain<> chain{};
    RoutingDecision rejection;
    EXPECT_TRUE(chain.apply(
        make_request(make_options("empty-chain", ExecutionIntent::Auto), false),
        {}, rejection));

    TaskOptions expired = make_options("empty-chain-expired", ExecutionIntent::Auto);
    expired.deadline = definitely_expired_deadline();
    EXPECT_TRUE(chain.apply(make_request(expired, false), {}, rejection))
        << "empty chain must not reject anything (identity)";
}
