#include "kairo/task_router.hpp"

#include "kairo/scheduling_pipeline.hpp"

namespace kairo {

// TaskRouter::route 的定义在 scheduling_pipeline.cpp：M1 pipeline 化后，
// 意图路由决策树由 scheduling::IntentCandidateGenerator 承载，选择由
// scheduling::select_first 完成；本方法保留为 "无约束过滤、无评分、
// 无 advisory" 的参考路径（0.6.1 结果逐项一致）。与 pipeline 同 TU
// 定义以便内联委派。

RoutingDecision TaskRouter::route_dispatch(
    const TaskOptions& options,
    const std::vector<ExecutorCapability>& capabilities) const {
    using detail::find_capability;

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
