#include "kairo/executor.hpp"

#include <algorithm>

// Executor facade — 路由与调度单元：意图路由（dispatch/route_task）、可注入
// 调度器生命周期、结构化决策记录与调度指标（0.6.1）、failure 事件面板、
// 总量有界 admission。
// （v0.7.0 M0：自 executor.cpp 按职责拆分，纯结构改动，零行为变化。）

namespace kairo {

RoutingDecision Executor::route_dispatch(const TaskOptions& options) const {
    // CR-106: 与 route_task 同理——intent 不支持或未指定 preferred_executor
    // 时是纯策略拒绝（TaskRouter::route_dispatch 的早退分支），能力采集
    // 仅在通过前置校验后才执行。
    const bool policy_rejected =
        (options.intent != ExecutionIntent::LowLatency &&
         options.intent != ExecutionIntent::RealtimeQueue) ||
        !options.preferred_executor || options.preferred_executor->empty();
    return task_router_.route_dispatch(
        options,
        policy_rejected ? std::vector<ExecutorCapability>{}
                        : manager_->get_executor_capabilities());
}

DispatchResult Executor::dispatch_auto(TaskOptions options, std::function<void()> task) {
    DispatchResult result;
    result.decision = route_dispatch(options);
    result.backend = result.decision.selected_backend;
    result.executor_name = result.decision.selected_executor_name;

    if (!task) {
        result.decision.reason = RoutingReason::Rejected;
        result.decision.status = RoutingStatus::Rejected;
        result.decision.detail = "dispatch task is empty";
        result.message = result.decision.detail;
        record_routing_decision(result.decision);
        record_submit_rejected(result.executor_name, result.decision.task_name, result.message);
        return result;
    }

    // 0.6.1：status 是权威判据（收敛了原来按 reason 枚举组合判断的写法）。
    if (result.decision.status == RoutingStatus::Rejected) {
        result.message = result.decision.detail;
        record_routing_decision(result.decision);
        record_submit_rejected(result.executor_name, result.decision.task_name, result.message);
        return result;
    }

    result.accepted = result.backend == ExecutionBackend::LockFree
                          ? manager_->try_push_lockfree_task(result.executor_name, std::move(task))
                          : manager_->try_push_realtime_task(result.executor_name, std::move(task));
    if (!result.accepted) {
        result.decision.reason = RoutingReason::Rejected;
        result.decision.status = RoutingStatus::Rejected;
        result.decision.detail = "bounded executor rejected dispatch (stopped, full, or object pool exhausted)";
        result.message = result.decision.detail;
        record_submit_rejected(result.executor_name, result.decision.task_name, result.message);
    }
    record_routing_decision(result.decision);
    return result;
}

std::vector<ExecutorCapability> Executor::get_executor_capabilities() const {
    return manager_->get_executor_capabilities();
}

void Executor::set_failure_callback(ExecutorFailureCallback callback) {
    std::lock_guard<std::mutex> lock(failure_mutex_);
    failure_callback_ = std::move(callback);
}

void Executor::set_snapshot_diagnostic_callback(ExecutorSnapshotCallback callback) {
    std::lock_guard<std::mutex> lock(snapshot_diagnostic_mutex_);
    snapshot_diagnostic_callback_ = std::move(callback);
}

ExecutorFailureStatus Executor::get_failure_status() const {
    std::lock_guard<std::mutex> lock(failure_mutex_);
    return failure_status_;
}

std::vector<ExecutorFailureEvent> Executor::get_recent_failures(size_t max_count) const {
    std::lock_guard<std::mutex> lock(failure_mutex_);

    const size_t available = recent_failures_.size();
    const size_t count = (max_count == 0 || max_count > available)
                             ? available
                             : max_count;

    std::vector<ExecutorFailureEvent> result;
    result.reserve(count);

    const size_t start = available - count;
    auto it = recent_failures_.begin();
    std::advance(it, static_cast<std::ptrdiff_t>(start));
    for (; it != recent_failures_.end(); ++it) {
        result.push_back(*it);
    }

    return result;
}

void Executor::clear_recent_failures() {
    std::lock_guard<std::mutex> lock(failure_mutex_);
    recent_failures_.clear();
}

void Executor::set_recent_failure_capacity(size_t capacity) {
    std::lock_guard<std::mutex> lock(failure_mutex_);
    recent_failure_capacity_ = capacity;
    while (recent_failures_.size() > recent_failure_capacity_) {
        recent_failures_.pop_front();
    }
}

std::optional<RoutingDecision> Executor::get_last_routing_decision() const {
    std::lock_guard<std::mutex> lock(routing_mutex_);
    if (recent_routing_decisions_.empty()) {
        return std::nullopt;
    }
    return recent_routing_decisions_.back();
}

std::vector<RoutingDecision> Executor::get_recent_routing_decisions(size_t max_count) const {
    std::lock_guard<std::mutex> lock(routing_mutex_);
    const size_t count = max_count == 0
                             ? recent_routing_decisions_.size()
                             : std::min(max_count, recent_routing_decisions_.size());
    return {recent_routing_decisions_.end() - static_cast<std::ptrdiff_t>(count),
            recent_routing_decisions_.end()};
}

void Executor::clear_recent_routing_decisions() {
    std::lock_guard<std::mutex> lock(routing_mutex_);
    recent_routing_decisions_.clear();
}

void Executor::set_recent_routing_capacity(size_t capacity) {
    std::lock_guard<std::mutex> lock(routing_mutex_);
    recent_routing_capacity_ = capacity;
    while (recent_routing_decisions_.size() > recent_routing_capacity_) {
        recent_routing_decisions_.pop_front();
    }
    // CR-106: 维护热路径观测快速开关（容量 0 且无回调 = 未观测）
    routing_observed_.store(recent_routing_capacity_ > 0 ||
                            static_cast<bool>(routing_callback_),
                            std::memory_order_release);
}

void Executor::set_routing_callback(std::function<void(const RoutingDecision&)> callback) {
    std::lock_guard<std::mutex> lock(routing_mutex_);
    routing_callback_ = std::move(callback);
    // CR-106: 同 set_recent_routing_capacity
    routing_observed_.store(recent_routing_capacity_ > 0 ||
                            static_cast<bool>(routing_callback_),
                            std::memory_order_release);
}

RoutingDecision Executor::route_task(const TaskOptions& options,
                                     bool cpu_gpu_task,
                                     std::optional<bool> gpu_selected) const {
    // CR-106 / NN-06: cpu_gpu_task == false 时路由结果是纯策略判定
    //（DefaultPolicy/ExplicitIntent/Rejected，见 TaskRouter::route 的早退分支），
    // 不会读取能力表——此前每次 submit_auto 仍全量锁 5 把注册表采能力，是
    // submit_auto 相对 submit 慢 65-70% 的主要构成。惰性采集：仅在
    // CpuOrGpu 意图（真正消费能力表）时才执行。
    // CR-106 惰性采集：仅在真正消费能力表的路径（CpuOrGpu / affinity
    // 相交性检查）才执行采集，其余保持纯策略判定。
    const bool needs_capabilities =
        cpu_gpu_task || !options.affinity.cpus.empty();
    RoutingDecision decision = task_scheduler_->route(
        TaskRouter::Request{options, cpu_gpu_task, gpu_selected},
        needs_capabilities ? manager_->get_executor_capabilities()
                           : std::vector<ExecutorCapability>{});
    // 0.6.1 兼容归一化：0.6.0 风格的自定义调度器只设置 reason（拒绝类
    // reason ∈ {Rejected, BackendUnavailable, BackendNotRunning,
    // CapacityPressure}）而不设置 status。这类决策统一升级为
    // status == Rejected，保证 future/result 一致性不因版本升级而漂移。
    if (decision.status != RoutingStatus::Rejected) {
        switch (decision.reason) {
        case RoutingReason::Rejected:
        case RoutingReason::BackendUnavailable:
        case RoutingReason::BackendNotRunning:
        case RoutingReason::CapacityPressure:
            decision.status = RoutingStatus::Rejected;
            break;
        default:
            break;
        }
    }
    return decision;
}

void Executor::set_scheduler(std::unique_ptr<IScheduler> scheduler) {
    if (scheduler) {
        task_scheduler_ = std::move(scheduler);
    } else {
        task_scheduler_ = std::make_unique<DefaultScheduler>();
    }
    // 0.6.1：缓存 feedback 开关。wants_feedback() == false（默认，含
    // DefaultScheduler）时 submit_auto 不附加测量包装，反馈通道零开销。
    scheduling_feedback_enabled_.store(task_scheduler_->wants_feedback(),
                                       std::memory_order_release);
}

void Executor::record_routing_decision(RoutingDecision decision) {
    // 0.6.1 调度指标：无条件累加（与下方 CR-106 观测开关解耦——健康度
    // 计数不应要求预先配置）。relaxed 足够：字段间无顺序要求，快照读取
    // 接受最终一致。
    count_routing_metrics(decision);
    // CR-106: 观测未配置（容量 0 且无回调）时零开销返回——路由诊断是纯
    // 可观测性设施，不该向提交热路径收税。routing_observed_ 由容量/回调
    // 的设置点维护，热路径只付一次 acquire load。
    if (!routing_observed_.load(std::memory_order_acquire)) {
        return;
    }
    std::function<void(const RoutingDecision&)> callback;
    {
        std::lock_guard<std::mutex> lock(routing_mutex_);
        // 回调读取必须在容量判断之外：容量 0 + 有回调 = 仅回调观测
        //（test_routing_callback_and_buffer_are_isolated 契约）。
        callback = routing_callback_;
        if (recent_routing_capacity_ > 0) {
            while (recent_routing_decisions_.size() >= recent_routing_capacity_) {
                recent_routing_decisions_.pop_front();
            }
            // 无回调消费方时直接移动入库（避免 2-3 次 std::string 堆拷贝）；
            // 有回调时保留副本供回调读取。
            if (callback) {
                recent_routing_decisions_.push_back(decision);
            } else {
                recent_routing_decisions_.push_back(std::move(decision));
            }
        }
    }
    if (callback) {
        try {
            callback(decision);
        } catch (...) {
            // Routing observation must not affect submission or worker threads.
        }
    }
}

size_t Executor::recent_failure_capacity() const {
    std::lock_guard<std::mutex> lock(failure_mutex_);
    return recent_failure_capacity_;
}

void Executor::count_routing_metrics(const RoutingDecision& decision) {
    constexpr auto relaxed = std::memory_order_relaxed;
    switch (decision.status) {
    case RoutingStatus::Accepted:
        scheduling_counters_.accepted.fetch_add(1, relaxed);
        break;
    case RoutingStatus::AcceptedDegraded:
        scheduling_counters_.accepted_degraded.fetch_add(1, relaxed);
        break;
    case RoutingStatus::Rejected:
        scheduling_counters_.rejected.fetch_add(1, relaxed);
        switch (decision.reason) {
        case RoutingReason::DeadlineExpired:
            scheduling_counters_.deadline_rejected.fetch_add(1, relaxed);
            break;
        case RoutingReason::BackendUnavailable:
            scheduling_counters_.backend_unavailable_rejected.fetch_add(1, relaxed);
            break;
        case RoutingReason::CapacityPressure:
            scheduling_counters_.capacity_rejected.fetch_add(1, relaxed);
            break;
        default:
            break;
        }
        if (decision.diagnostics & RoutingDiagnostics::ResourceInfeasible) {
            scheduling_counters_.resource_rejected.fetch_add(1, relaxed);
        }
        break;
    default:
        break;
    }
    if (decision.diagnostics & RoutingDiagnostics::AffinityMismatch) {
        scheduling_counters_.affinity_mismatch.fetch_add(1, relaxed);
    }
}

SchedulingMetrics Executor::get_scheduling_metrics() const {
    constexpr auto relaxed = std::memory_order_relaxed;
    SchedulingMetrics metrics;
    metrics.accepted_count = scheduling_counters_.accepted.load(relaxed);
    metrics.accepted_degraded_count = scheduling_counters_.accepted_degraded.load(relaxed);
    metrics.rejected_count = scheduling_counters_.rejected.load(relaxed);
    metrics.deadline_rejected_count = scheduling_counters_.deadline_rejected.load(relaxed);
    metrics.backend_unavailable_rejected_count =
        scheduling_counters_.backend_unavailable_rejected.load(relaxed);
    metrics.capacity_rejected_count = scheduling_counters_.capacity_rejected.load(relaxed);
    metrics.resource_rejected_count = scheduling_counters_.resource_rejected.load(relaxed);
    metrics.affinity_mismatch_count = scheduling_counters_.affinity_mismatch.load(relaxed);
    metrics.deadline_missed_count = scheduling_counters_.deadline_missed.load(relaxed);
    metrics.feedback_reported_count = scheduling_counters_.feedback_reported.load(relaxed);
    return metrics;
}

void Executor::report_scheduling_feedback(const SchedulingFeedback& feedback) {
    // 0.6.1 feedback 通道：交付注入的 IScheduler 并计数。on_task_completed
    // 在 worker 线程同步调用，其异常必须被隔离——反馈不能杀死任务执行；
    // 抛出时该条反馈的语义即"已尝试交付"。
    scheduling_counters_.feedback_reported.fetch_add(1, std::memory_order_relaxed);
    try {
        task_scheduler_->on_task_completed(feedback);
    } catch (...) {
        // Scheduling feedback must not affect worker threads.
    }
}

void Executor::record_failure(ExecutorFailureEvent event) {
    ExecutorFailureCallback callback;

    {
        std::lock_guard<std::mutex> lock(failure_mutex_);

        ++failure_status_.total_count;
        switch (event.kind) {
        case FailureKind::TaskException:
            ++failure_status_.task_exception_count;
            break;
        case FailureKind::SubmitRejected:
            ++failure_status_.submit_rejected_count;
            break;
        case FailureKind::TaskTimeout:
            ++failure_status_.timeout_count;
            break;
        case FailureKind::RealtimeDrop:
            ++failure_status_.realtime_drop_count;
            break;
        case FailureKind::GpuFailure:
            ++failure_status_.gpu_failure_count;
            break;
        case FailureKind::WaitTimeout:
            ++failure_status_.wait_timeout_count;
            break;
        case FailureKind::TuningFallback:
            ++failure_status_.tuning_fallback_count;
            break;
        case FailureKind::CapacityExhausted:
            ++failure_status_.capacity_exhausted_count;
            break;
        case FailureKind::DeadlineMissed:
            ++failure_status_.deadline_missed_count;
            break;
        default:
            break;
        }

        if (recent_failure_capacity_ > 0) {
            while (recent_failures_.size() >= recent_failure_capacity_) {
                recent_failures_.pop_front();
            }
            recent_failures_.push_back(event);
        }

        callback = failure_callback_;
    }

    if (callback) {
        try {
            callback(event);
        } catch (...) {
            // Failure observation must never become a new worker/background failure.
        }
    }
}

void Executor::record_result_failure(const ExecutorResult& result,
                                     FailureKind kind,
                                     const std::string& executor_name,
                                     const std::string& task_id) {
    if (result.ok) {
        return;
    }

    ExecutorFailureEvent event;
    event.kind = kind;
    event.executor_name = executor_name;
    event.task_id = task_id;
    event.message = std::string(executor_error_code_to_string(result.error_code)) +
                    ": " + result.message;
    record_failure(std::move(event));
    emit_snapshot_diagnostic();
}

void Executor::record_submit_rejected(const std::string& executor_name,
                                      const std::string& task_id,
                                      const std::string& message,
                                      std::exception_ptr exception) {
    ExecutorFailureEvent event;
    event.kind = FailureKind::SubmitRejected;
    event.executor_name = executor_name;
    event.task_id = task_id;
    event.message = message;
    event.exception = exception;
    record_failure(std::move(event));
}

void Executor::record_capacity_exhausted(const std::string& executor_name,
                                         const std::string& task_id,
                                         const std::string& message) {
    ExecutorFailureEvent event;
    event.kind = FailureKind::CapacityExhausted;
    event.executor_name = executor_name;
    event.task_id = task_id;
    event.message = message;
    event.exception = std::make_exception_ptr(CapacityExhaustedException(message));
    record_failure(std::move(event));
}

Executor::AdmissionDecision Executor::try_admit_submission(
    const std::string& executor_name,
    const std::string& task_id,
    const std::string& scope) {
    AdmissionDecision decision;
    const int64_t max = static_cast<int64_t>(
        max_in_flight_tasks_.load(std::memory_order_acquire));
    if (max <= 0) {
        return decision;  // 未启用：无计数、无释放器
    }
    const int64_t current =
        in_flight_submissions_.fetch_add(1, std::memory_order_acq_rel);
    if (current >= max) {
        in_flight_submissions_.fetch_sub(1, std::memory_order_release);
        record_capacity_exhausted(
            executor_name, task_id,
            "In-flight submission capacity exhausted (" + scope + "); "
            "max_in_flight_tasks=" + std::to_string(max));
        decision.accepted = false;
        return decision;
    }
    decision.releaser = std::make_shared<AdmissionReleaser>(&in_flight_submissions_);
    return decision;
}

void Executor::set_max_in_flight_tasks(size_t max) {
    max_in_flight_tasks_.store(max, std::memory_order_release);
}

size_t Executor::get_max_in_flight_tasks() const {
    return max_in_flight_tasks_.load(std::memory_order_acquire);
}

size_t Executor::get_in_flight_submissions() const {
    const int64_t max = static_cast<int64_t>(
        max_in_flight_tasks_.load(std::memory_order_acquire));
    if (max <= 0) {
        return 0;
    }
    const int64_t current = in_flight_submissions_.load(std::memory_order_acquire);
    return current > 0 ? static_cast<size_t>(current) : 0;
}

void Executor::record_task_exception(const std::string& executor_name,
                                     const std::string& task_id,
                                     const std::string& message,
                                     std::exception_ptr exception) {
    ExecutorFailureEvent event;
    event.kind = FailureKind::TaskException;
    event.executor_name = executor_name;
    event.task_id = task_id;
    event.message = message;
    event.exception = exception;
    record_failure(std::move(event));
}

void Executor::record_task_timeout(const std::string& executor_name,
                                   const std::string& task_id,
                                   const std::string& message,
                                   std::exception_ptr exception) {
    ExecutorFailureEvent event;
    event.kind = FailureKind::TaskTimeout;
    event.executor_name = executor_name;
    event.task_id = task_id;
    event.message = message;
    event.exception = exception;
    record_failure(std::move(event));
}

void Executor::record_realtime_drop(const std::string& executor_name,
                                    const std::string& task_id,
                                    const std::string& message,
                                    std::exception_ptr exception) {
    ExecutorFailureEvent event;
    event.kind = FailureKind::RealtimeDrop;
    event.executor_name = executor_name;
    event.task_id = task_id;
    event.message = message;
    event.exception = exception;
    record_failure(std::move(event));
}

void Executor::record_periodic_task_exception(const std::string& executor_name,
                                              const std::string& task_id,
                                              const std::string& message,
                                              std::exception_ptr exception) {
    // PeriodicTaskStatus 计数由 TimerScheduler::report_tick_failure 维护；
    // 这里只保留 failure 事件可观测性。
    record_task_exception(executor_name, task_id, message, exception);
}

void Executor::record_periodic_submit_rejected(const std::string& executor_name,
                                               const std::string& task_id,
                                               const std::string& message,
                                               std::exception_ptr exception) {
    record_submit_rejected(executor_name, task_id, message, exception);
}

} // namespace kairo
