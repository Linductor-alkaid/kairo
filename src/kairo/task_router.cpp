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

    const auto reject = [&](RoutingReason reason, std::string detail) {
        decision.reason = reason;
        decision.detail = std::move(detail);
        return decision;
    };

    if (!request.cpu_gpu_task) {
        if (request.options.intent == ExecutionIntent::Auto) {
            return reject(RoutingReason::DefaultPolicy, "default async policy");
        }
        if (request.options.intent == ExecutionIntent::GeneralCpu) {
            return reject(RoutingReason::ExplicitIntent, "GeneralCpu selects default async executor");
        }
        return reject(RoutingReason::Rejected,
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
            return reject(RoutingReason::Rejected,
                          "RequireRequestedBackend requires preferred_executor");
        }
        if (!gpu_available) {
            return reject(unavailable_reason(), "requested GPU executor is unavailable, stopped, or at capacity");
        }
        decision.selected_backend = ExecutionBackend::Gpu;
        decision.selected_executor_name = gpu->name;
        return reject(RoutingReason::PreferredExecutor, "required GPU executor is available");
    }

    if (!gpu_available) {
        if (request.options.fallback == FallbackPolicy::AllowCpu) {
            decision.fell_back = true;
            return reject(RoutingReason::FallbackPolicy,
                          "GPU unavailable; falling back to default async executor");
        }
        return reject(unavailable_reason(), "no GPU executor is available for CpuOrGpu task");
    }

    if (request.gpu_selected && !*request.gpu_selected) {
        return reject(RoutingReason::GpuHeuristic, "GPU scheduler selected CPU");
    }

    decision.selected_backend = ExecutionBackend::Gpu;
    decision.selected_executor_name = gpu->name;
    return reject(request.options.preferred_executor ? RoutingReason::PreferredExecutor : RoutingReason::GpuHeuristic,
                  request.options.preferred_executor ? "preferred GPU executor selected" : "GPU scheduler selected GPU");
}

RoutingDecision TaskRouter::route_dispatch(
    const TaskOptions& options,
    const std::vector<ExecutorCapability>& capabilities) const {
    RoutingDecision decision;
    decision.task_name = options.name.empty() ? "facade_dispatch_auto" : options.name;
    decision.requested_intent = options.intent;
    decision.selected_backend = ExecutionBackend::LockFree;

    const auto reject = [&](RoutingReason reason, std::string detail) {
        decision.reason = reason;
        decision.detail = std::move(detail);
        return decision;
    };
    if (options.intent != ExecutionIntent::LowLatency &&
        options.intent != ExecutionIntent::RealtimeQueue) {
        return reject(RoutingReason::Rejected,
                      "dispatch_auto only supports LowLatency or RealtimeQueue");
    }
    if (!options.preferred_executor || options.preferred_executor->empty()) {
        return reject(RoutingReason::Rejected,
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
        return reject(RoutingReason::BackendUnavailable, "requested bounded executor is not registered");
    }
    if (!capability->running) {
        return reject(RoutingReason::BackendNotRunning, "requested bounded executor is not running");
    }
    if (capability->capacity_hint != 0 && capability->pending_work >= capability->capacity_hint) {
        return reject(RoutingReason::CapacityPressure, "requested bounded executor is at capacity");
    }
    return reject(RoutingReason::PreferredExecutor, "requested bounded executor selected");
}

}  // namespace kairo
