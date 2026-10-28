#include "kairo/executor.hpp"

#include "subject_quota_registry.hpp"

#include <algorithm>
#include <chrono>
#include <memory>

// Executor facade — 路由与调度单元：意图路由（dispatch/route_task）、可注入
// 调度器生命周期、结构化决策记录与调度指标（0.6.1）、failure 事件面板、
// 总量有界 admission。
// （v0.7.0 M0：自 executor.cpp 按职责拆分，纯结构改动，零行为变化。）

namespace kairo {

namespace {

int64_t steady_now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

Executor::CapabilityView Executor::acquire_route_capabilities(bool needs) const {
    constexpr auto relaxed = std::memory_order_relaxed;
    if (!needs) {
        return {};
    }

    // 命中路径预算：一次 acquire load + epoch/TTL 两个整数比较 + 一次
    // 时钟采样（时钟本来就要为 TTL 服务，采样结果不另作他用）。未命中
    // 才付 CR-106 惰性采集的注册表锁成本。
    const int64_t ttl_ns =
        capability_cache_ttl_ns_.load(std::memory_order_relaxed);
    const int64_t now_ns = steady_now_ns();
    if (ttl_ns > 0) {
        if (std::shared_ptr<const CapabilitySnapshot> cached =
                capability_cache_.load()) {
            const bool epoch_unchanged =
                cached->manager_epoch == manager_->get_state_epoch();
            const bool within_ttl =
                now_ns - cached->collected_ns < ttl_ns;
            if (epoch_unchanged && within_ttl) {
                scheduling_counters_.capability_cache_hit.fetch_add(1, relaxed);
                return CapabilityView{std::move(cached), true};
            }
        }
    }

    // 未命中：实时采集并发布。发布持有一份 shared_ptr、返回视图持有
    // 另一份——并发未命中各自采集、最后发布者胜出，被覆盖的旧快照由
    // 引用计数兜底回收（在读者手中保持有效）；采集每 TTL 至多一轮，
    // 不为它引入热路径互斥。
    scheduling_counters_.capability_cache_miss.fetch_add(1, relaxed);
    auto fresh = std::make_shared<CapabilitySnapshot>();
    fresh->manager_epoch = manager_->get_state_epoch();
    fresh->collected_ns = now_ns;
    fresh->capabilities = manager_->get_executor_capabilities();
    capability_cache_.store(fresh);
    return CapabilityView{std::move(fresh), false};
}

RoutingDecision Executor::route_dispatch(const TaskOptions& options) const {
    // 0.7.1 §3.0.3 执行器自动选择：意图受支持但用户未显式指定
    // preferred_executor 时，咨询注入的调度器（默认实现返回空，行为
    // 不变；AdaptiveScheduler 按能力快照的瞬时队列负载选择）。选择
    // 消费能力快照——经 TTL 缓存（§3.0 联动）。
    const bool intent_supported =
        options.intent == ExecutionIntent::LowLatency ||
        options.intent == ExecutionIntent::RealtimeQueue;
    const bool has_explicit_executor =
        options.preferred_executor && !options.preferred_executor->empty();
    if (intent_supported && !has_explicit_executor) {
        CapabilityView selection_view = acquire_route_capabilities(true);
        if (auto selected = task_scheduler_->select_bounded_executor(
                options.intent, selection_view.get())) {
            TaskOptions effective = options;
            effective.preferred_executor = selected;
            RoutingDecision decision =
                task_router_.route_dispatch(effective, selection_view.get());
            decision.diagnostics |= RoutingDiagnostics::AutoExecutorSelection;
            decision.detail +=
                "; executor auto-selected by scheduler (min queued load)";
            return decision;
        }
    }

    // CR-106: 与 route_task 同理——intent 不支持或未指定 preferred_executor
    // 时是纯策略拒绝（TaskRouter::route_dispatch 的早退分支），能力采集
    // 仅在通过前置校验后才执行。
    const bool policy_rejected = !intent_supported || !has_explicit_executor;
    // 0.7.1 §3.0.1：与 route_task 共用能力快照 TTL 缓存；纯策略拒绝
    // 不消费能力表（不触发采集/计数）。
    CapabilityView capability_view =
        policy_rejected ? CapabilityView{} : acquire_route_capabilities(true);
    return task_router_.route_dispatch(options, capability_view.get());
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

void Executor::set_capability_cache_ttl(std::chrono::nanoseconds ttl) noexcept {
    // 先失效缓存再改 TTL：从"长 TTL 切到 0"的瞬时窗口内，旧快照不会
    // 被误判为命中（改完后下一次访问按新 TTL 判定，0 即必采集）。
    capability_cache_.store(nullptr);
    capability_cache_ttl_ns_.store(ttl.count(), std::memory_order_release);
}

std::chrono::nanoseconds Executor::capability_cache_ttl() const noexcept {
    return std::chrono::nanoseconds(
        capability_cache_ttl_ns_.load(std::memory_order_acquire));
}

void Executor::invalidate_capability_cache() noexcept {
    capability_cache_.store(nullptr);
}

ReservationResult Executor::try_reserve(
    const ResourceRequirements& requirements) {
    ReservationResult result;
    if (!resource_reservations_) {
        result.error = ReserveError::NotConfigured;
        return result;
    }
    std::shared_ptr<detail::TerminalReleaser> releaser;
    result.error = resource_reservations_->try_acquire(requirements, releaser);
    if (result.error == ReserveError::None) {
        result.lease = ResourceLease(std::move(releaser));
    }
    return result;
}

ResourceReservationStatus Executor::get_resource_reservation_status() const {
    if (!resource_reservations_) {
        return {};
    }
    return resource_reservations_->status();
}

void Executor::set_auto_inference_enabled(bool enabled) noexcept {
    auto_inference_enabled_.store(enabled, std::memory_order_release);
    // 推断历史经反馈通道采样：开关切换需同步测量包装门。
    refresh_feedback_gate();
}

void Executor::refresh_feedback_gate() noexcept {
    scheduling_feedback_enabled_.store(
        task_scheduler_ ? (task_scheduler_->wants_feedback() ||
                           auto_inference_enabled_.load(std::memory_order_acquire))
                        : false,
        std::memory_order_release);
}

void Executor::annotate_auto_inference(const TaskOptions& options,
                                       RoutingDecision& decision) const noexcept {
    if (decision.status == RoutingStatus::Rejected ||
        !auto_inference_enabled_.load(std::memory_order_acquire) ||
        !task_inference_) {
        return;
    }
    const int64_t now_ns = steady_now_ns();
    const TaskCharacteristics characteristics = task_inference_->analyze(
        options.name.empty() ? std::string("submit_auto") : options.name,
        now_ns);
    const char* reason = nullptr;
    const char* suggestion = nullptr;
    if (characteristics.is_blocking_io) {
        reason = "blocking_io_pattern";
        suggestion = "consider start_worker(BlockingWorkerSpec) for a "
                     "dedicated blocking-I/O worker";
    } else if (characteristics.needs_low_latency) {
        reason = "low_latency_pattern";
        suggestion =
            "consider dispatch_auto(LowLatency) with a lock-free executor "
            "for bounded low-latency dispatch";
    } else if (characteristics.is_periodic) {
        reason = "periodic_pattern";
        suggestion = "consider submit_periodic() for regular soft-period work";
    }
    if (reason == nullptr) {
        return;
    }
    if (options.intent != ExecutionIntent::Auto) {
        // 用户显式 intent 优先：推断被覆盖，计数不注解决策。
        scheduling_counters_.auto_inference_rejected.fetch_add(
            1, std::memory_order_relaxed);
        return;
    }
    decision.diagnostics |= RoutingDiagnostics::AutoInferredIntent;
    decision.inferred_reason = reason;
    decision.detail += decision.detail.empty() ? "" : "; ";
    decision.detail +=
        std::string("auto-inference (") + reason +
        "): " + suggestion;
}

bool Executor::evaluate_deadline_policy(int64_t deadline_ns,
                                        DeadlinePolicy policy,
                                        const std::string& executor_name,
                                        const std::string& task_id,
                                        StopSource* cancel_target) noexcept {
    // 无 deadline 或未严格错过（恰好相等接受，与 0.6.1 准入边界一致）：
    // 不产生任何观测或处置。
    if (deadline_ns <= 0) {
        return true;
    }
    const int64_t now_ns = steady_now_ns();
    if (now_ns <= deadline_ns) {
        return true;
    }

    constexpr auto relaxed = std::memory_order_relaxed;
    scheduling_counters_.deadline_missed.fetch_add(1, relaxed);

    switch (policy) {
    case DeadlinePolicy::DropIfLate: {
        scheduling_counters_.deadline_dropped.fetch_add(1, relaxed);
        return false;
    }
    case DeadlinePolicy::CancelIfLate:
        // 协作式取消请求，不是抢占：任务仍启动，用户代码经
        // this_task::get_stop_token() 观测并自行退出。
        if (cancel_target) {
            cancel_target->request_stop();
        }
        return true;
    case DeadlinePolicy::RunAnyway:
    default: {
        // 0.6.x miss-observe：记录 failure 事件（任务仍执行）。
        ExecutorFailureEvent event;
        event.kind = FailureKind::DeadlineMissed;
        event.executor_name = executor_name;
        event.task_id = task_id;
        event.message = "task started after declared deadline";
        record_failure(std::move(event));
        return true;
    }
    }
}

bool Executor::is_deadline_drop_exception(std::exception_ptr exception) noexcept {
    if (!exception) {
        return false;
    }
    try {
        std::rethrow_exception(exception);
    } catch (const TaskCancelled& cancelled) {
        return cancelled.reason() == TaskCancellationReason::DeadlineMissed;
    } catch (...) {
        return false;
    }
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
    // CR-106 惰性采集 + 0.7.1 §3.0.1 缓存化：仅在真正消费能力表的路径
    // （CpuOrGpu / affinity 相交性检查）才取能力快照，其余保持纯策略
    // 判定；取快照经 TTL 缓存（默认 100ms，manager state_epoch 变化主动
    // 失效），命中路径零注册表锁。命中时决策附带 CapabilityCacheHit
    // 诊断位与 capability_from_cache 字段标记来源。
    const bool needs_capabilities =
        cpu_gpu_task || !options.affinity.cpus.empty();
    CapabilityView capability_view = acquire_route_capabilities(needs_capabilities);
    RoutingDecision decision = task_scheduler_->route(
        TaskRouter::Request{options, cpu_gpu_task, gpu_selected},
        capability_view.get());
    if (capability_view.from_cache) {
        decision.diagnostics |= RoutingDiagnostics::CapabilityCacheHit;
        decision.capability_from_cache = true;
    }
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
        case RoutingReason::LoadShedding:
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
    // 0.7.1 §3.0.2：自动推断开启时同样需要反馈流（历史采样）。
    refresh_feedback_gate();
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
        case RoutingReason::LoadShedding:
            // 0.7.0 M3：QoS 感知降载的结构化拒绝。
            scheduling_counters_.load_shedding_rejected.fetch_add(1, relaxed);
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
    // 0.7.0 M3：历史驱动的自适应决策（含确认启发式的"维持"结果）。
    if (decision.diagnostics & RoutingDiagnostics::AdaptiveHistory) {
        scheduling_counters_.adaptive_history.fetch_add(1, relaxed);
    }
    // 0.7.1 §3.0.2/§3.0.3：自动推断建议与执行器自动选择计数。
    if (decision.diagnostics & RoutingDiagnostics::AutoInferredIntent) {
        scheduling_counters_.auto_inferred.fetch_add(1, relaxed);
    }
    if (decision.diagnostics & RoutingDiagnostics::AutoExecutorSelection) {
        scheduling_counters_.auto_executor_selected.fetch_add(1, relaxed);
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
    metrics.adaptive_history_count =
        scheduling_counters_.adaptive_history.load(relaxed);
    metrics.load_shedding_rejected_count =
        scheduling_counters_.load_shedding_rejected.load(relaxed);
    metrics.priority_promoted_count =
        scheduling_counters_.priority_promoted.load(relaxed);
    // 0.7.1 §3.0.1：能力快照缓存行为计数（访问点统计，与决策记录解耦）。
    metrics.capability_cache_hit_count =
        scheduling_counters_.capability_cache_hit.load(relaxed);
    metrics.capability_cache_miss_count =
        scheduling_counters_.capability_cache_miss.load(relaxed);
    // 0.7.1 §3.2：deadline policy 丢弃计数。
    metrics.deadline_dropped_count =
        scheduling_counters_.deadline_dropped.load(relaxed);
    // 0.7.1 §3.0.2/§3.0.3：自动推断与执行器自动选择计数。
    metrics.auto_inferred_count =
        scheduling_counters_.auto_inferred.load(relaxed);
    metrics.auto_inference_rejected_count =
        scheduling_counters_.auto_inference_rejected.load(relaxed);
    metrics.auto_executor_selected_count =
        scheduling_counters_.auto_executor_selected.load(relaxed);
    return metrics;
}

void Executor::report_scheduling_feedback(const SchedulingFeedback& feedback) {
    // 0.6.1 feedback 通道：交付注入的 IScheduler 并计数。on_task_completed
    // 在 worker 线程同步调用，其异常必须被隔离——反馈不能杀死任务执行；
    // 抛出时该条反馈的语义即"已尝试交付"。
    scheduling_counters_.feedback_reported.fetch_add(1, std::memory_order_relaxed);
    // 0.7.0 M2 反馈聚合：worker 热路径无锁分片累加，noexcept（与下方
    // IScheduler 交付解耦；仅 wants_feedback() 时本通道有样本流入）。
    feedback_aggregator_.record(feedback);
    // 0.7.1 §3.0.2：自动推断历史采样（仅推断开启时有流入）。
    if (auto_inference_enabled_.load(std::memory_order_acquire) && task_inference_) {
        task_inference_->record(
            feedback,
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }
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
            // 分桶份额拒绝是总量拒绝的子集（issue #222）：同一 kind 下
            // 依 event.subject 区分，细分计数同步递增。
            if (!event.subject.empty()) {
                ++failure_status_.subject_quota_exhausted_count;
            }
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

void Executor::record_subject_quota_exhausted(
    const std::string& executor_name,
    const std::string& task_id,
    const std::string& subject,
    const std::string& message,
    std::exception_ptr exception) {
    ExecutorFailureEvent event;
    event.kind = FailureKind::CapacityExhausted;
    event.executor_name = executor_name;
    event.task_id = task_id;
    event.message = message;
    // 与 future 结算共用同一异常（调用方传入，避免重复构造）。
    event.exception = std::move(exception);
    event.subject = subject;
    record_failure(std::move(event));
}

Executor::AdmissionDecision Executor::try_admit_submission(
    const std::string& executor_name,
    const std::string& task_id,
    const std::string& scope,
    const std::string& subject) {
    AdmissionDecision decision;

    // 分桶查表先于任何计数（issue #222）：未带键的提交不触碰注册表锁；
    // 未配置主体 = 仅全局约束（"未知即宽容"）。计数 shared 所有权在此
    // 取得，fetch_add 在注册表锁外。
    std::shared_ptr<detail::SubjectKeyStats> bucket_stats;
    int64_t bucket_max = 0;
    if (!subject.empty()) {
        detail::SubjectQuotaRegistry* registry = nullptr;
        {
            std::lock_guard<std::mutex> lock(subject_quotas_mutex_);
            registry = subject_quotas_.get();
        }
        if (registry) {
            bucket_stats = registry->find(subject, bucket_max);
        }
    }

    const int64_t max = static_cast<int64_t>(
        max_in_flight_tasks_.load(std::memory_order_acquire));
    if (max <= 0 && !bucket_stats) {
        return decision;  // 两维皆未启用：无计数、无释放器
    }

    // 全局维度先行（总量语义逐位不变，docs/design/bounded_admission.md）。
    bool global_counted = false;
    if (max > 0) {
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
        global_counted = true;
    }

    // 分桶维度：全局通过后评估；超限则回滚分桶与全局并拒绝。拒绝消息
    // 与 future 结算共用同一异常（decision.rejection_exception）。
    if (bucket_stats) {
        const int64_t bucket_current =
            bucket_stats->in_flight.fetch_add(1, std::memory_order_acq_rel);
        if (bucket_current >= bucket_max) {
            bucket_stats->in_flight.fetch_sub(1, std::memory_order_release);
            if (global_counted) {
                in_flight_submissions_.fetch_sub(1, std::memory_order_release);
            }
            const std::string message =
                "Subject in-flight quota exhausted (key=" + subject + "); "
                "subject_max_in_flight=" + std::to_string(bucket_max);
            auto exception =
                std::make_exception_ptr(CapacityExhaustedException(message));
            record_subject_quota_exhausted(
                executor_name, task_id, subject, message, exception);
            decision.accepted = false;
            decision.rejection_exception = std::move(exception);
            return decision;
        }
        // 接纳成功：累计接纳数（修订 R-1 诊断面，relaxed 即可——接入方
        // 验证键确实流过接纳点，不参与任何判定）。别名 shared_ptr 与
        // in_flight 共享 keepalive 所有权。
        bucket_stats->admitted_total.fetch_add(1, std::memory_order_relaxed);
    }

    decision.releaser = std::make_shared<AdmissionReleaser>(
        global_counted ? &in_flight_submissions_ : nullptr,
        bucket_stats
            ? std::shared_ptr<std::atomic<int64_t>>(bucket_stats,
                                                    &bucket_stats->in_flight)
            : nullptr);
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

void Executor::set_subject_max_in_flight_tasks(const std::string& subject,
                                               size_t max) {
    std::lock_guard<std::mutex> lock(subject_quotas_mutex_);
    if (!subject_quotas_) {
        subject_quotas_ = std::make_unique<detail::SubjectQuotaRegistry>();
    }
    subject_quotas_->set(subject, max);
}

size_t Executor::get_subject_max_in_flight_tasks(
    const std::string& subject) const {
    std::lock_guard<std::mutex> lock(subject_quotas_mutex_);
    if (!subject_quotas_) {
        return 0;  // 未配置任何主体："未知即宽容"
    }
    int64_t max = 0;
    const auto stats = subject_quotas_->find(subject, max);
    return stats ? static_cast<size_t>(max) : 0;
}

size_t Executor::get_subject_in_flight_submissions(
    const std::string& subject) const {
    std::lock_guard<std::mutex> lock(subject_quotas_mutex_);
    if (!subject_quotas_) {
        return 0;
    }
    int64_t max = 0;
    const auto stats = subject_quotas_->find(subject, max);
    if (!stats) {
        return 0;
    }
    const int64_t current = stats->in_flight.load(std::memory_order_acquire);
    return current > 0 ? static_cast<size_t>(current) : 0;
}

std::vector<SubjectQuotaStatus> Executor::get_subject_quota_status() const {
    std::lock_guard<std::mutex> lock(subject_quotas_mutex_);
    if (!subject_quotas_) {
        return {};
    }
    return subject_quotas_->status();
}

AggregateExecutionStats Executor::get_aggregate_execution_stats(
    const std::string& executor_name) const {
    // 0.7.2 §4.2：按名称解析 realtime / lockfree 后端的聚合计数。
    // unknown 即零值（"未知即宽容"），调用方无需区分后端类型。
    if (auto realtime = manager_->get_realtime_executor_snapshot(executor_name)) {
        return realtime->get_aggregate_execution_stats();
    }
    if (auto lockfree = manager_->get_lockfree_executor_snapshot(executor_name)) {
        return lockfree->get_aggregate_execution_stats();
    }
    return AggregateExecutionStats{};
}

std::map<std::string, AggregateExecutionStats>
Executor::get_all_aggregate_execution_stats() const {
    std::map<std::string, AggregateExecutionStats> result;
    for (const auto& name : manager_->get_realtime_executor_names()) {
        if (auto realtime = manager_->get_realtime_executor_snapshot(name)) {
            result.emplace(name, realtime->get_aggregate_execution_stats());
        }
    }
    for (const auto& name : manager_->get_lockfree_executor_names()) {
        if (auto lockfree = manager_->get_lockfree_executor_snapshot(name)) {
            result.emplace(name, lockfree->get_aggregate_execution_stats());
        }
    }
    return result;
}

AllocationMetrics Executor::get_allocation_metrics() const {
    auto executor = manager_->get_default_async_executor_snapshot();
    if (!executor) {
        return AllocationMetrics{};
    }
    return executor->get_allocation_metrics();
}

double Executor::get_task_pool_hit_rate() const {
    const AllocationMetrics metrics = get_allocation_metrics();
    const uint64_t total =
        metrics.task_pool_hit_count + metrics.task_pool_miss_count;
    if (total == 0) {
        return 0.0;
    }
    return static_cast<double>(metrics.task_pool_hit_count) /
           static_cast<double>(total);
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
