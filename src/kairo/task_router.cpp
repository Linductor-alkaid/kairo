#include "kairo/task_router.hpp"

#include <algorithm>

namespace kairo {
namespace {

const ExecutorCapability* find_capability(
    const std::vector<ExecutorCapability>& capabilities,
    ExecutionBackend backend,
    const std::string& name = {}) {
    const auto iterator = std::find_if(capabilities.begin(), capabilities.end(),
        [&](const ExecutorCapability& capability) {
            return capability.backend == backend &&
                   (name.empty() || capability.name == name);
        });
    return iterator == capabilities.end() ? nullptr : &*iterator;
}

bool gpu_submittable(const ExecutorCapability* capability) {
    return capability && capability->registered && capability->running &&
           capability->supports_gpu_kernel &&
           (capability->capacity_hint == 0 ||
            capability->pending_work < capability->capacity_hint);
}

}  // namespace

RoutingDecision TaskRouter::route(
    const Request& request,
    const std::vector<ExecutorCapability>& capabilities) const {
    RoutingDecision decision;
    decision.task_name = request.options.name.empty() ? "facade_submit_auto" : request.options.name;
    decision.requested_intent = request.options.intent;
    decision.selected_backend = ExecutionBackend::DefaultAsync;
    decision.selected_executor_name = "default";

    // 0.6.1：每条返回路径显式声明 RoutingStatus。status 是结果的权威
    // 判据，reason 只解释原因；两条诊断路径（fallback / heuristic-CPU）
    // 不再伪装成 "reject"。
    const auto finalize = [&](RoutingReason reason, RoutingStatus status,
                              std::string detail) {
        decision.reason = reason;
        decision.status = status;
        decision.detail = std::move(detail);
        return decision;
    };

    if (!request.cpu_gpu_task) {
        if (request.options.intent == ExecutionIntent::Auto) {
            return finalize(RoutingReason::DefaultPolicy, RoutingStatus::Accepted,
                            "default async policy");
        }
        if (request.options.intent == ExecutionIntent::GeneralCpu) {
            return finalize(RoutingReason::ExplicitIntent, RoutingStatus::Accepted,
                            "GeneralCpu selects default async executor");
        }
        return finalize(RoutingReason::Rejected, RoutingStatus::Rejected,
                        "task intent requires a typed submission API");
    }

    const std::string requested_gpu = request.options.preferred_executor.value_or("");
    const auto* gpu = find_capability(capabilities, ExecutionBackend::Gpu, requested_gpu);
    if (requested_gpu.empty()) {
        const size_t gpu_count = static_cast<size_t>(std::count_if(
            capabilities.begin(), capabilities.end(), [](const ExecutorCapability& capability) {
                return capability.backend == ExecutionBackend::Gpu;
            }));
        if (gpu_count != 1) {
            gpu = nullptr;
        }
    }
    const bool gpu_available = gpu_submittable(gpu);
    const auto unavailable_reason = [&] {
        if (!gpu || !gpu->registered) return RoutingReason::BackendUnavailable;
        if (!gpu->running) return RoutingReason::BackendNotRunning;
        return RoutingReason::CapacityPressure;
    };

    if (request.options.fallback == FallbackPolicy::RequireRequestedBackend) {
        if (requested_gpu.empty()) {
            return finalize(RoutingReason::Rejected, RoutingStatus::Rejected,
                            "RequireRequestedBackend requires preferred_executor");
        }
        if (!gpu_available) {
            return finalize(unavailable_reason(), RoutingStatus::Rejected,
                            "requested GPU executor is unavailable, stopped, or at capacity");
        }
        decision.selected_backend = ExecutionBackend::Gpu;
        decision.selected_executor_name = gpu->name;
        return finalize(RoutingReason::PreferredExecutor, RoutingStatus::Accepted,
                        "required GPU executor is available");
    }

    if (!gpu_available) {
        if (request.options.fallback == FallbackPolicy::AllowCpu) {
            decision.fell_back = true;
            return finalize(RoutingReason::FallbackPolicy, RoutingStatus::AcceptedDegraded,
                            "GPU unavailable; falling back to default async executor");
        }
        return finalize(unavailable_reason(), RoutingStatus::Rejected,
                        "no GPU executor is available for CpuOrGpu task");
    }

    if (request.gpu_selected && !*request.gpu_selected) {
        // 启发式选择 CPU 是 CpuOrGpu 的正常结果之一，不是降级。
        return finalize(RoutingReason::GpuHeuristic, RoutingStatus::Accepted,
                        "GPU scheduler selected CPU");
    }

    decision.selected_backend = ExecutionBackend::Gpu;
    decision.selected_executor_name = gpu->name;
    return finalize(request.options.preferred_executor ? RoutingReason::PreferredExecutor
                                                       : RoutingReason::GpuHeuristic,
                    RoutingStatus::Accepted,
                    request.options.preferred_executor ? "preferred GPU executor selected"
                                                       : "GPU scheduler selected GPU");
}

RoutingDecision TaskRouter::route_dispatch(
    const TaskOptions& options,
    const std::vector<ExecutorCapability>& capabilities) const {
    RoutingDecision decision;
    decision.task_name = options.name.empty() ? "facade_dispatch_auto" : options.name;
    decision.requested_intent = options.intent;
    decision.selected_backend = ExecutionBackend::LockFree;

    const auto finalize = [&](RoutingReason reason, RoutingStatus status,
                              std::string detail) {
        decision.reason = reason;
        decision.status = status;
        decision.detail = std::move(detail);
        return decision;
    };
    if (options.intent != ExecutionIntent::LowLatency &&
        options.intent != ExecutionIntent::RealtimeQueue) {
        return finalize(RoutingReason::Rejected, RoutingStatus::Rejected,
                        "dispatch_auto only supports LowLatency or RealtimeQueue");
    }
    if (!options.preferred_executor || options.preferred_executor->empty()) {
        return finalize(RoutingReason::Rejected, RoutingStatus::Rejected,
                        "bounded dispatch requires preferred_executor");
    }
    decision.selected_executor_name = *options.preferred_executor;
    const ExecutionBackend backend = options.intent == ExecutionIntent::LowLatency
                                         ? ExecutionBackend::LockFree
                                         : ExecutionBackend::Realtime;
    decision.selected_backend = backend;
    const auto* capability = find_capability(
        capabilities, backend, decision.selected_executor_name);
    if (!capability || !capability->registered) {
        return finalize(RoutingReason::BackendUnavailable, RoutingStatus::Rejected,
                        "requested bounded executor is not registered");
    }
    if (!capability->running) {
        return finalize(RoutingReason::BackendNotRunning, RoutingStatus::Rejected,
                        "requested bounded executor is not running");
    }
    if (capability->capacity_hint != 0 && capability->pending_work >= capability->capacity_hint) {
        return finalize(RoutingReason::CapacityPressure, RoutingStatus::Rejected,
                        "requested bounded executor is at capacity");
    }
    return finalize(RoutingReason::PreferredExecutor, RoutingStatus::Accepted,
                    "requested bounded executor selected");
}

}  // namespace kairo
