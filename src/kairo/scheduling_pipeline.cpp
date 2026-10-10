#include "kairo/scheduling_pipeline.hpp"

#include <algorithm>
#include <chrono>
#include <string>

namespace kairo::scheduling {

// ============================================================================
// 四个阶段的热路径实现（唯一编译单元）。
//
// 结构：file-static 的内部实现函数（impl 后缀，KAIRO_SCHED_HOT）承载
// 全部逻辑，公开的阶段成员函数是普通外联定义（可从任意 TU 链接、可
// 单独组合），内部委派同一实现。SchedulingPipeline<IdentityScoring>
// 的显式实例化只触碰内部实现函数——全部折叠进单一扁平函数。
//
// 为什么不允许公开函数参与热路径折叠：带 always_inline 的定义不产出
// 库外符号（外部 TU 链接失败）；普通外联定义又会给 route 引入真实的
// 阶段边界调用（实测每个 ~10ns，而策略路径在 0.6.1 基线 ~36ns 上只有
// ~2ns 预算）。内部实现函数无递归、无取地址逃逸，always_inline 不改变
// 语义，只是恢复与 0.6.1 相同的形状。
// ============================================================================

#if defined(__GNUC__) || defined(__clang__)
#define KAIRO_SCHED_HOT __attribute__((always_inline)) inline
#else
#define KAIRO_SCHED_HOT inline
#endif

namespace {

// ---- 阶段一：约束过滤（deadline → GPU 资源可行性），首个拒绝短路 ----

KAIRO_SCHED_HOT bool deadline_filter_impl(
    const TaskRouter::Request& request,
    const std::vector<ExecutorCapability>& capabilities,
    RoutingDecision& rejection) {
    (void)capabilities;
    const TaskOptions& options = request.options;

    if (options.deadline) {
        const auto now_ns = std::chrono::steady_clock::now().time_since_epoch();
        if (now_ns > options.deadline->time_since_epoch()) {
            rejection.task_name = options.name;
            rejection.requested_intent = options.intent;
            rejection.selected_backend = ExecutionBackend::DefaultAsync;
            rejection.reason = RoutingReason::DeadlineExpired;
            rejection.status = RoutingStatus::Rejected;
            rejection.diagnostics = RoutingDiagnostics::None;
            rejection.detail = "scheduling: deadline already missed at submission";
            return false;
        }
    }
    return true;
}

KAIRO_SCHED_HOT bool gpu_resource_filter_impl(
    const TaskRouter::Request& request,
    const std::vector<ExecutorCapability>& capabilities,
    RoutingDecision& rejection) {
    const TaskOptions& options = request.options;

    const bool gpu_relevant = request.cpu_gpu_task ||
                              options.intent == ExecutionIntent::CpuOrGpu;
    if (!gpu_relevant ||
        (options.resources.gpu_device < 0 && options.resources.memory_bytes == 0)) {
        return true;
    }

    const ExecutorCapability* gpu =
        detail::find_gpu_capability(capabilities, options.preferred_executor);
    if (!gpu) {
        // 无匹配 GPU 能力时交给候选生成阶段：意图路由已有成熟的
        // BackendUnavailable/回退语义，不在此重复。capability 未知
        // （gpu_device < 0 / 内存总量 0）时按弱一致模型跳过检查
        // （feasibility hint，非 reservation）。
        return true;
    }
    if (options.resources.gpu_device >= 0 &&
        gpu->gpu_device >= 0 &&
        options.resources.gpu_device != gpu->gpu_device) {
        rejection.task_name = options.name;
        rejection.requested_intent = options.intent;
        rejection.selected_backend = ExecutionBackend::DefaultAsync;
        rejection.reason = RoutingReason::BackendUnavailable;
        rejection.status = RoutingStatus::Rejected;
        rejection.diagnostics = RoutingDiagnostics::ResourceInfeasible;
        rejection.detail = "scheduling: requested gpu_device " +
                           std::to_string(options.resources.gpu_device) +
                           " but executor '" + gpu->name + "' runs on device " +
                           std::to_string(gpu->gpu_device);
        return false;
    }
    if (options.resources.memory_bytes > 0 && gpu->gpu_memory_total_bytes > 0) {
        const size_t available =
            gpu->gpu_memory_free_bytes > 0 ? gpu->gpu_memory_free_bytes
                                           : gpu->gpu_memory_total_bytes;
        if (options.resources.memory_bytes > available) {
            rejection.task_name = options.name;
            rejection.requested_intent = options.intent;
            rejection.selected_backend = ExecutionBackend::DefaultAsync;
            rejection.reason = RoutingReason::CapacityPressure;
            rejection.status = RoutingStatus::Rejected;
            rejection.diagnostics = RoutingDiagnostics::ResourceInfeasible;
            rejection.detail = "scheduling: requested " +
                               std::to_string(options.resources.memory_bytes) +
                               " bytes exceeds GPU '" + gpu->name + "' availability";
            return false;
        }
    }
    return true;
}

// ---- 阶段二：意图路由候选生成 ----

// TaskRouter::route 的原始字段形态：task_name 有兜底、
// selected_executor_name 默认 "default"。0.6.1：status 是结果的权威
// 判据；两条诊断路径（fallback / heuristic-CPU）不伪装成 "reject"。
// 候选决策直写 primary（0.6.1 树至多一个候选），单遍形状与旧实现一致。
KAIRO_SCHED_HOT void propose_candidate(RoutingDecision& primary,
                                       CandidatePlan& plan,
                                       const TaskOptions& options,
                                       RoutingReason reason,
                                       RoutingStatus status,
                                       ExecutionBackend backend,
                                       std::string_view executor_name,
                                       std::string_view detail,
                                       bool fell_back) {
    primary.task_name = options.name.empty() ? "facade_submit_auto"
                                             : options.name;
    primary.requested_intent = options.intent;
    primary.reason = reason;
    primary.status = status;
    primary.selected_backend = backend;
    primary.selected_executor_name = std::string(executor_name);
    primary.fell_back = fell_back;
    primary.detail = std::string(detail);
    plan.count = 1;
}

KAIRO_SCHED_HOT void refuse_candidate(CandidatePlan& plan,
                                      const TaskOptions& options,
                                      RoutingReason reason,
                                      std::string_view detail) {
    RoutingDecision& decision = plan.make_refusal();
    decision.task_name = options.name.empty() ? "facade_submit_auto"
                                              : options.name;
    decision.requested_intent = options.intent;
    decision.selected_backend = ExecutionBackend::DefaultAsync;
    decision.selected_executor_name = "default";
    decision.reason = reason;
    decision.status = RoutingStatus::Rejected;
    decision.detail = std::string(detail);
}

KAIRO_SCHED_HOT void generate_candidates_impl(
    const TaskRouter::Request& request,
    const std::vector<ExecutorCapability>& capabilities,
    RoutingDecision& primary, CandidatePlan& plan) {
    const TaskOptions& options = request.options;

    if (!request.cpu_gpu_task) {
        if (options.intent == ExecutionIntent::Auto) {
            propose_candidate(primary, plan, options,
                              RoutingReason::DefaultPolicy, RoutingStatus::Accepted,
                              ExecutionBackend::DefaultAsync, "default",
                              "default async policy", false);
        } else if (options.intent == ExecutionIntent::GeneralCpu) {
            propose_candidate(primary, plan, options,
                              RoutingReason::ExplicitIntent, RoutingStatus::Accepted,
                              ExecutionBackend::DefaultAsync, "default",
                              "GeneralCpu selects default async executor", false);
        } else {
            refuse_candidate(plan, options, RoutingReason::Rejected,
                             "task intent requires a typed submission API");
        }
        return;
    }

    const std::string requested_gpu = options.preferred_executor.value_or("");
    const auto* gpu = detail::find_capability(capabilities, ExecutionBackend::Gpu,
                                              requested_gpu);
    if (requested_gpu.empty()) {
        const size_t gpu_count = static_cast<size_t>(std::count_if(
            capabilities.begin(), capabilities.end(), [](const ExecutorCapability& capability) {
                return capability.backend == ExecutionBackend::Gpu;
            }));
        if (gpu_count != 1) {
            gpu = nullptr;
        }
    }
    const bool gpu_available = detail::gpu_submittable(gpu);
    const auto unavailable_reason = [&] {
        if (!gpu || !gpu->registered) return RoutingReason::BackendUnavailable;
        if (!gpu->running) return RoutingReason::BackendNotRunning;
        return RoutingReason::CapacityPressure;
    };

    if (options.fallback == FallbackPolicy::RequireRequestedBackend) {
        if (requested_gpu.empty()) {
            refuse_candidate(plan, options, RoutingReason::Rejected,
                             "RequireRequestedBackend requires preferred_executor");
        } else if (!gpu_available) {
            refuse_candidate(plan, options, unavailable_reason(),
                             "requested GPU executor is unavailable, stopped, or at capacity");
        } else {
            propose_candidate(primary, plan, options,
                              RoutingReason::PreferredExecutor, RoutingStatus::Accepted,
                              ExecutionBackend::Gpu, gpu->name,
                              "required GPU executor is available", false);
        }
        return;
    }

    if (!gpu_available) {
        if (options.fallback == FallbackPolicy::AllowCpu) {
            propose_candidate(primary, plan, options,
                              RoutingReason::FallbackPolicy,
                              RoutingStatus::AcceptedDegraded,
                              ExecutionBackend::DefaultAsync, "default",
                              "GPU unavailable; falling back to default async executor",
                              true);
        } else {
            refuse_candidate(plan, options, unavailable_reason(),
                             "no GPU executor is available for CpuOrGpu task");
        }
        return;
    }

    if (request.gpu_selected && !*request.gpu_selected) {
        // 启发式选择 CPU 是 CpuOrGpu 的正常结果之一，不是降级。
        propose_candidate(primary, plan, options,
                          RoutingReason::GpuHeuristic, RoutingStatus::Accepted,
                          ExecutionBackend::DefaultAsync, "default",
                          "GPU scheduler selected CPU", false);
        return;
    }

    if (options.preferred_executor) {
        propose_candidate(primary, plan, options,
                          RoutingReason::PreferredExecutor, RoutingStatus::Accepted,
                          ExecutionBackend::Gpu, gpu->name,
                          "preferred GPU executor selected", false);
    } else {
        propose_candidate(primary, plan, options,
                          RoutingReason::GpuHeuristic, RoutingStatus::Accepted,
                          ExecutionBackend::Gpu, gpu->name,
                          "GPU scheduler selected GPU", false);
    }
}

// ---- 阶段四：选择 + affinity advisory ----

KAIRO_SCHED_HOT void select_first_impl(CandidatePlan& plan,
                                       RoutingDecision& decision) {
    if (plan.count == 0) {
        decision = plan.take_refusal();
    }
    // count == 1：生成阶段已直写最终候选，无需搬运。
}

KAIRO_SCHED_HOT void affinity_advisory_impl(
    RoutingDecision& decision,
    const TaskRouter::Request& request,
    const std::vector<ExecutorCapability>& capabilities) {
    const TaskOptions& options = request.options;

    // affinity：advisory 检查——请求核集合与目标后端绑核不相交时任务
    // 仍被接受（绑核是后端启动期属性），但记录结构化诊断。
    if (options.affinity.cpus.empty() || capabilities.empty() ||
        decision.status == RoutingStatus::Rejected) {
        return;
    }
    const ExecutionBackend target = decision.selected_backend;
    for (const auto& capability : capabilities) {
        if (capability.backend != target || capability.bound_cpus.empty()) {
            continue;
        }
        bool intersects = false;
        for (const int cpu : options.affinity.cpus) {
            if (std::find(capability.bound_cpus.begin(),
                          capability.bound_cpus.end(),
                          cpu) != capability.bound_cpus.end()) {
                intersects = true;
                break;
            }
        }
        if (!intersects) {
            const bool first_degradation =
                decision.status == RoutingStatus::Accepted;
            decision.diagnostics |= RoutingDiagnostics::AffinityMismatch;
            decision.status = RoutingStatus::AcceptedDegraded;
            if (first_degradation) {
                decision.reason = RoutingReason::AffinityMismatch;
            }
            decision.detail += (decision.detail.empty() ? "" : "; ");
            decision.detail += "AffinityMismatch: requested cpus do not "
                               "intersect executor '" + capability.name +
                               "' bound set";
        }
        break;
    }
}

}  // namespace

// ============================================================================
// 公开阶段定义：普通外联（可链接、可组合），内部委派实现函数。
// ============================================================================

bool DeadlineConstraintFilter::apply(
    const TaskRouter::Request& request,
    const std::vector<ExecutorCapability>& capabilities,
    RoutingDecision& rejection) const {
    return deadline_filter_impl(request, capabilities, rejection);
}

bool GpuResourceConstraintFilter::apply(
    const TaskRouter::Request& request,
    const std::vector<ExecutorCapability>& capabilities,
    RoutingDecision& rejection) const {
    return gpu_resource_filter_impl(request, capabilities, rejection);
}

void IntentCandidateGenerator::generate(
    const TaskRouter::Request& request,
    const std::vector<ExecutorCapability>& capabilities,
    RoutingDecision& primary, CandidatePlan& plan) const {
    generate_candidates_impl(request, capabilities, primary, plan);
}

void select_first(CandidatePlan& plan, RoutingDecision& decision) {
    select_first_impl(plan, decision);
}

void apply_affinity_advisory(RoutingDecision& decision,
                             const TaskRouter::Request& request,
                             const std::vector<ExecutorCapability>& capabilities) {
    affinity_advisory_impl(decision, request, capabilities);
}

// ============================================================================
// pipeline 显式实例化：只触碰内部实现函数，全阶段折叠。
// ============================================================================

template <typename ScoringStage>
RoutingDecision SchedulingPipeline<ScoringStage>::route(
    const TaskRouter::Request& request,
    const std::vector<ExecutorCapability>& capabilities) const {
    RoutingDecision decision;  // NRVO：直接构造于调用方返回槽位

    // 阶段一：约束过滤（deadline → 资源可行性），首个拒绝短路。
    if (!deadline_filter_impl(request, capabilities, decision)) {
        return decision;
    }
    if (!gpu_resource_filter_impl(request, capabilities, decision)) {
        return decision;
    }

    // 阶段二：候选生成（意图路由决策树，直写最终决策存储）。
    CandidatePlan plan;
    generate_candidates_impl(request, capabilities, decision, plan);

    // 阶段三：评分/排序（0.6.1 默认为空实现，保持生成序）。
    scoring_.rank(plan, request);

    // 阶段四：选择 + affinity advisory 后处理。
    select_first_impl(plan, decision);
    affinity_advisory_impl(decision, request, capabilities);
    return decision;
}

// 0.6.1 确定性 pipeline 的唯一显式实例化：本 TU 内全阶段折叠。
// 后续 AdaptiveScoring 在此追加实例化；用户自定义评分请直接组合各阶段。
template class SchedulingPipeline<IdentityScoring>;

}  // namespace kairo::scheduling

namespace kairo {

// TaskRouter::route（非热路径参考实现，与 pipeline 同 TU 以便内联委派）：
// "无约束过滤、无评分、无 advisory" 的 0.6.1 意图路由结果。
RoutingDecision TaskRouter::route(
    const Request& request,
    const std::vector<ExecutorCapability>& capabilities) const {
    RoutingDecision decision;
    scheduling::CandidatePlan plan;
    scheduling::IntentCandidateGenerator{}.generate(request, capabilities,
                                                    decision, plan);
    scheduling::select_first(plan, decision);
    return decision;
}

}  // namespace kairo
