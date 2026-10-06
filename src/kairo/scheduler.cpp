#include "kairo/scheduler.hpp"

#include <chrono>
#include <algorithm>

namespace kairo {

namespace {

const ExecutorCapability* find_gpu_capability(
    const std::vector<ExecutorCapability>& capabilities,
    const std::optional<std::string>& preferred) {
    if (preferred) {
        for (const auto& capability : capabilities) {
            if (capability.backend == ExecutionBackend::Gpu &&
                capability.name == *preferred) {
                return &capability;
            }
        }
        return nullptr;
    }
    for (const auto& capability : capabilities) {
        if (capability.backend == ExecutionBackend::Gpu && capability.running) {
            return &capability;
        }
    }
    return nullptr;
}

RoutingDecision make_rejection(const TaskOptions& options,
                               RoutingReason reason,
                               uint32_t diagnostics,
                               std::string detail) {
    RoutingDecision decision;
    decision.task_name = options.name;
    decision.requested_intent = options.intent;
    decision.selected_backend = ExecutionBackend::DefaultAsync;
    decision.reason = reason;
    decision.status = RoutingStatus::Rejected;
    decision.diagnostics = diagnostics;
    decision.detail = std::move(detail);
    return decision;
}

}  // namespace

RoutingDecision DefaultScheduler::route(
    const TaskRouter::Request& request,
    const std::vector<ExecutorCapability>& capabilities) {
    const TaskOptions& options = request.options;

    // deadline：提交时点已过期的任务没有调偿价值，明确拒绝而不是
    // 让它排队后必然错过（deadline-aware admission 的第一层）。
    // 0.6.1：拒绝语义锁定为"严格已过"（now > deadline）；恰好等于
    // deadline 的提交仍被接受。reason 使用结构化 DeadlineExpired。
    if (options.deadline) {
        const auto now_ns = std::chrono::steady_clock::now().time_since_epoch();
        if (now_ns > options.deadline->time_since_epoch()) {
            return make_rejection(
                options, RoutingReason::DeadlineExpired,
                RoutingDiagnostics::None,
                "scheduling: deadline already missed at submission");
        }
    }

    const bool gpu_relevant = request.cpu_gpu_task ||
                              options.intent == ExecutionIntent::CpuOrGpu;
    if (gpu_relevant &&
        (options.resources.gpu_device >= 0 || options.resources.memory_bytes > 0)) {
        const ExecutorCapability* gpu =
            find_gpu_capability(capabilities, options.preferred_executor);
        if (gpu) {
            if (options.resources.gpu_device >= 0 &&
                gpu->gpu_device >= 0 &&
                options.resources.gpu_device != gpu->gpu_device) {
                return make_rejection(
                    options,
                    RoutingReason::BackendUnavailable,
                    RoutingDiagnostics::ResourceInfeasible,
                    "scheduling: requested gpu_device " +
                        std::to_string(options.resources.gpu_device) +
                        " but executor '" + gpu->name + "' runs on device " +
                        std::to_string(gpu->gpu_device));
            }
            if (options.resources.memory_bytes > 0 && gpu->gpu_memory_total_bytes > 0) {
                const size_t available =
                    gpu->gpu_memory_free_bytes > 0 ? gpu->gpu_memory_free_bytes
                                                   : gpu->gpu_memory_total_bytes;
                if (options.resources.memory_bytes > available) {
                    return make_rejection(
                        options,
                        RoutingReason::CapacityPressure,
                        RoutingDiagnostics::ResourceInfeasible,
                        "scheduling: requested " +
                            std::to_string(options.resources.memory_bytes) +
                            " bytes exceeds GPU '" + gpu->name + "' availability");
                }
            }
        }
        // 无匹配 GPU 能力时交给 TaskRouter：意图路由已有成熟的
        // BackendUnavailable/回退语义，不在此重复。capability 未知
        // （gpu_device < 0 / 内存总量 0）时按弱一致模型跳过检查
        // （feasibility hint，非 reservation）。
    }

    RoutingDecision decision = router_.route(request, capabilities);

    // affinity：advisory 检查——请求核集合与目标后端绑核不相交时任务
    // 仍被接受（绑核是后端启动期属性），但记录结构化诊断：
    // diagnostics |= AffinityMismatch + status = AcceptedDegraded。
    // 已拒绝的决策不再附加 affinity 警告（拒绝原因保持单一明确）；
    // 已降级的决策（如 fallback）仍记录诊断位，但 reason 保留首个
    // 降级原因（FallbackPolicy），不覆盖。
    if (!options.affinity.cpus.empty() && !capabilities.empty() &&
        decision.status != RoutingStatus::Rejected) {
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

    return decision;
}

}  // namespace kairo
