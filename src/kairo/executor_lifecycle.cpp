#include "kairo/executor.hpp"

#include "executor_detail.hpp"
#include "kairo/monitor/executor_snapshot_formatter.hpp"
#include "kairo/monitor/executor_monitor.hpp"
#include "task/task_dependency_manager.hpp"
#include <stdexcept>

// Executor facade — 生命周期单元：单例/实例构造与析构、initialize/shutdown、
// parked 任务关停结算、监控透传与等待/快照查询。
// （v0.7.0 M0：自 executor.cpp 按职责拆分，纯结构改动，零行为变化。）

namespace kairo {

using detail::make_failure;
using detail::validate_executor_config;

// 单例模式实现
Executor& Executor::instance() {
    static Executor inst(ExecutorManager::instance());
    return inst;
}

// 单例模式构造函数（私有）
Executor::Executor(ExecutorManager& manager)
    : manager_(&manager)
    , owned_manager_(nullptr)
    , cancellation_registry_(std::make_unique<TaskCancellationRegistry>())
    , timers_(std::make_shared<detail::TimerScheduler>())
    , task_dependencies_(std::make_unique<TaskDependencyManager>())
    , task_scheduler_(std::make_unique<DefaultScheduler>()) {
    configure_timer_scheduler_hooks();
    monitor_ = std::make_unique<monitor::ExecutorMonitor>(
        *manager_, lifecycle_state_,
        [this]() { return get_completion_status(); },
        [this]() { return get_failure_status(); },
        [this]() { return get_recent_failures(); },
        [this]() { return get_all_task_statistics(); },
        [this]() { return manager_->get_in_flight_task_diagnostics(); },
        [this]() { return get_cancellation_status(); },
        [this]() { return get_timer_status_summary(); });
}

// 实例化模式构造函数
Executor::Executor()
    : manager_(nullptr)
    , owned_manager_(std::make_unique<ExecutorManager>())
    , cancellation_registry_(std::make_unique<TaskCancellationRegistry>())
    , timers_(std::make_shared<detail::TimerScheduler>())
    , task_dependencies_(std::make_unique<TaskDependencyManager>())
    , task_scheduler_(std::make_unique<DefaultScheduler>()) {
    manager_ = owned_manager_.get();
    configure_timer_scheduler_hooks();
    monitor_ = std::make_unique<monitor::ExecutorMonitor>(
        *manager_, lifecycle_state_,
        [this]() { return get_completion_status(); },
        [this]() { return get_failure_status(); },
        [this]() { return get_recent_failures(); },
        [this]() { return get_all_task_statistics(); },
        [this]() { return manager_->get_in_flight_task_diagnostics(); },
        [this]() { return get_cancellation_status(); },
        [this]() { return get_timer_status_summary(); });
}

// 析构函数
Executor::~Executor() {
    stop_timer_thread();
    // 实例模式：池排空必须在 facade 状态成员析构之前完成。成员按声明逆序
    // 析构时 owned_manager_ 几乎最后销毁，若依赖析构链触发排空，
    // task_graph_mutex_、closure_graveyard_、failure_mutex_、
    // periodic_tasks_mutex_ 等会先一步被销毁，仍在运行的 wrapper（捕获
    // this）随即 use-after-free。
    // shutdown() 幂等：用户已显式 shutdown 时这里基本是空操作。
    if (owned_manager_) {
        try {
            (void)shutdown(true);
        } catch (...) {
            // 析构不外泄异常；~ExecutorManager 内部还有 RAII 兜底。
        }
        try {
            owned_manager_.reset();
        } catch (...) {
        }
    } else if (manager_ != nullptr && manager_->has_default_async_executor()) {
        // 单例模式（CR-001）：函数级静态按构造逆序，本析构先于
        // ExecutorManager 静态析构执行，manager_ 此刻仍存活。此前单例析构
        // 不排空，默认池在途任务在静态析构窗口内触达已销毁的 facade 成员
        // （failure_mutex_/图锁/timers_/cancellation_registry_）导致 UAF。
        // has_default_async_executor() 只查询不懒建池：从未用默认池的进程
        // 退出时不产生建池副作用，其余后端仍由 atexit 兜底关停。
        try {
            (void)shutdown(true);
        } catch (...) {
            // 析构不外泄异常。
        }
    }
}

// 初始化执行器
ExecutorResult Executor::initialize(const ExecutorConfig& config) {
    if (auto validation = validate_executor_config(config); !validation.ok) {
        lifecycle_state_.store(ExecutorLifecycleState::Failed, std::memory_order_release);
        record_result_failure(
            validation, FailureKind::SubmitRejected, "default", "facade_initialize");
        return validation;
    }

    if (manager_->is_default_async_shutdown()) {
        auto result = make_failure(
            ExecutorErrorCode::AlreadyShutdown,
            "Async executor has already been shutdown");
        record_result_failure(
            result, FailureKind::SubmitRejected, "default", "facade_initialize");
        return result;
    }

    if (manager_->has_default_async_executor()) {
        auto result = make_failure(
            ExecutorErrorCode::AlreadyInitialized,
            "Async executor is already initialized");
        record_result_failure(
            result, FailureKind::SubmitRejected, "default", "facade_initialize");
        return result;
    }

    lifecycle_state_.store(ExecutorLifecycleState::Initializing, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(task_graph_mutex_);
        task_graph_retention_capacity_ = config.task_graph_retention_capacity;
        trim_task_graph_retention_locked();
    }
    max_in_flight_tasks_.store(config.max_in_flight_tasks, std::memory_order_release);
    // 默认池由同一配置创建，其队列计时器时长与本值一致；parked 依赖图任务
    // 的 facade 侧超时定时器（D1）据此武装。
    default_task_timeout_ms_.store(config.task_timeout_ms, std::memory_order_release);
    if (!manager_->initialize_async_executor(config)) {
        auto code = manager_->is_default_async_shutdown()
                        ? ExecutorErrorCode::AlreadyShutdown
                        : manager_->has_default_async_executor()
                              ? ExecutorErrorCode::AlreadyInitialized
                              : ExecutorErrorCode::StartFailed;
        auto result = make_failure(
            code,
            code == ExecutorErrorCode::StartFailed
                ? "Async executor initialization failed"
                : "Async executor initialization was rejected");
        record_result_failure(
            result, FailureKind::SubmitRejected, "default", "facade_initialize");
        if (code == ExecutorErrorCode::StartFailed) {
            lifecycle_state_.store(ExecutorLifecycleState::Failed, std::memory_order_release);
        }
        return result;
    }

    lifecycle_state_.store(ExecutorLifecycleState::Running, std::memory_order_release);
    return ExecutorResult::success("Async executor initialized");
}

// 关闭执行器
ShutdownResult Executor::shutdown(bool wait_for_tasks) {
    stop_timer_thread();
    lifecycle_state_.store(ExecutorLifecycleState::Draining, std::memory_order_release);
    // CR-013: shutdown 不建池——未初始化时没有可停的对象，也不该在退出
    // 路径上拉起一个线程池再关掉。
    const auto async_executor =
        manager_->get_default_async_executor_snapshot_no_create();
    if (async_executor && async_executor->is_current_worker_thread()) {
        const auto result = manager_->shutdown(wait_for_tasks);
        fail_all_parked_tasks_for_shutdown();
        if (result == ShutdownResult::Completed) {
            lifecycle_state_.store(ExecutorLifecycleState::Stopped, std::memory_order_release);
        }
        return result;
    }
    if (wait_for_tasks && manager_->has_default_async_executor()) {
        const auto wait_result = wait_for_completion(kDefaultWaitForCompletionTimeout);
        const auto result = manager_->shutdown(wait_result.completed);
        fail_all_parked_tasks_for_shutdown();
        if (result == ShutdownResult::Completed) {
            lifecycle_state_.store(ExecutorLifecycleState::Stopped, std::memory_order_release);
        }
        return result;
    }

    const auto result = manager_->shutdown(wait_for_tasks);
    fail_all_parked_tasks_for_shutdown();
    if (result == ShutdownResult::Completed) {
        lifecycle_state_.store(ExecutorLifecycleState::Stopped, std::memory_order_release);
    }
    return result;
}

void Executor::fail_all_parked_tasks_for_shutdown() {
    // D2（docs/design/dependency_driven_scheduling.md §5.5）：shutdown 返回
    // 时仍 parked 的节点，其依赖要么已被丢弃、要么不可能再推进——统一失败
    // 结算，不悬空 future、不滞留 admission/registry/in-flight 槽位。
    // setp 与结算分离：单节点一锁，结算（promise/admission/registry/
    // terminal hook）全部在图锁外经 drain 执行。
    std::vector<std::string> parked_ids;
    {
        std::lock_guard<std::mutex> lock(task_graph_mutex_);
        parked_ids.reserve(task_graph_nodes_.size());
        for (const auto& entry : task_graph_nodes_) {
            if (entry.second.parked &&
                entry.second.state == TaskGraphState::Pending) {
                parked_ids.push_back(entry.first);
            }
        }
    }
    if (parked_ids.empty()) {
        // 无 parked 节点同样要清墓地：parked 超时/竞争输家路径转入墓地的
        // promise/state 引用在 shutdown 终局统一释放（契约：shutdown 后
        // closure_graveyard_size() == 0，与有无 parked 节点无关）。
        std::lock_guard<std::mutex> lock(task_graph_mutex_);
        closure_graveyard_.clear();
        return;
    }

    auto shutdown_exception = std::make_exception_ptr(std::runtime_error(
        "Executor is shutting down; parked task was never executed"));
    ParkedDrainBag bag;
    for (const auto& task_id : parked_ids) {
        std::vector<std::function<void()>> node_failures;
        std::vector<ParkedReady> node_ready;
        {
            std::lock_guard<std::mutex> lock(task_graph_mutex_);
            auto it = task_graph_nodes_.find(task_id);
            if (it == task_graph_nodes_.end() ||
                !it->second.parked ||
                it->second.state != TaskGraphState::Pending) {
                continue;  // 已被依赖终态级联或并发 sweep 结算
            }
            auto payload = std::move(it->second.parked);
            it->second.parked.reset();
            it->second.state = TaskGraphState::Failed;
            it->second.exception = shutdown_exception;
            it->second.error_message =
                "Executor shutting down; parked task will not run";
            manager_->record_in_flight_task_terminal(task_id);
            resolve_task_graph_dependents_locked(
                task_id, node_ready, node_failures);
            finalize_task_graph_node_locked(task_id);
            bag.failures.push_back([payload, shutdown_exception]() mutable {
                payload->settle_without_run(shutdown_exception);
            });
        }
        for (auto& failure : node_failures) {
            bag.failures.push_back(std::move(failure));
        }
        // 依赖已失败的 parked 节点不会产生 ready 出队列；防御性合并，
        // 避免任何遗漏路径静默丢失载荷。
        for (auto& ready : node_ready) {
            bag.ready.push_back(std::move(ready));
        }
    }
    drain_parked_resolutions(bag.ready, bag.failures);
    // 墓地终局清空（安全：shared_ptr 引用计数，在途闭包仍持有各自引用；
    // shutdown(true) 下 worker 已 join，shutdown(false) 下由闭包自身引用
    // 保活到其终局）。仅回收墓地持有的引用，降低常驻内存。
    {
        std::lock_guard<std::mutex> lock(task_graph_mutex_);
        closure_graveyard_.clear();
    }
}

// 获取异步执行器状态
AsyncExecutorStatus Executor::get_async_executor_status() const {
    // CR-013: 只读诊断不得懒创建默认池（与 get_snapshot 的文档承诺一致）。
    auto executor = manager_->get_default_async_executor_snapshot_no_create();
    if (!executor) {
        AsyncExecutorStatus status;
        status.name = "default";
        status.is_running = false;
        return status;
    }

    return executor->get_status();
}

void Executor::enable_monitoring(bool enable) {
    manager_->enable_monitoring(enable);
}

void Executor::set_monitoring_sampling_rate(double rate) {
    manager_->set_monitoring_sampling_rate(rate);
}

void Executor::set_in_flight_task_capacity(size_t capacity) {
    manager_->set_in_flight_task_capacity(capacity);
}

void Executor::set_in_flight_task_sampling_rate(double rate) {
    manager_->set_in_flight_task_sampling_rate(rate);
}

TaskStatistics Executor::get_task_statistics(const std::string& task_type) const {
    return manager_->get_task_statistics(task_type);
}

std::map<std::string, TaskStatistics> Executor::get_all_task_statistics() const {
    return manager_->get_all_task_statistics();
}

bool Executor::try_wait_for_completion(std::chrono::milliseconds timeout) {
    return wait_for_completion(timeout).completed;
}

WaitResult Executor::wait_for_completion(std::chrono::milliseconds timeout) {
    WaitResult result;
    result.timeout = timeout;

    // Waiting for an absent backend is complete; it must not lazily create one.
    if (!manager_->has_default_async_executor()) {
        result.completed = true;
        result.timed_out = false;
        result.status = get_completion_status();
        result.message = "Async executor is not initialized";
        return result;
    }

    auto ex = manager_->get_default_async_executor_snapshot();
    if (!ex) {
        result.completed = true;
        result.timed_out = false;
        result.status = get_completion_status();
        result.message = "Async executor is not initialized";
        return result;
    }

    result.completed = ex->try_wait_for_completion(timeout);
    result.timed_out = !result.completed;
    result.status = get_completion_status();

    if (result.completed) {
        result.message = "All async tasks completed";
        return result;
    }

    result.message = "wait_for_completion timed out before all tasks completed";

    ExecutorFailureEvent event;
    event.kind = FailureKind::WaitTimeout;
    event.executor_name = result.status.executor_name;
    event.task_id = "facade_wait_for_completion";
    event.message = result.message + ": active=" +
                    std::to_string(result.status.active_tasks) +
                    ", queued=" + std::to_string(result.status.queued_tasks) +
                    ", pending=" + std::to_string(result.status.pending_tasks);
    record_failure(std::move(event));
    result.diagnostic_snapshot = get_snapshot();
    emit_snapshot_diagnostic(*result.diagnostic_snapshot);
    return result;
}

bool Executor::is_idle() const {
    return get_completion_status().is_idle;
}

CompletionStatus Executor::get_completion_status() const {
    CompletionStatus completion;
    if (!manager_->has_default_async_executor()) {
        return completion;
    }

    auto ex = manager_->get_default_async_executor_snapshot();
    if (!ex) {
        return completion;
    }

    const auto status = ex->get_status();
    completion.executor_name = status.name;
    completion.is_initialized = true;
    completion.is_running = status.is_running;
    completion.active_tasks = status.active_tasks;
    completion.queued_tasks = status.queue_size;
    completion.pending_tasks = status.active_tasks + status.queue_size;
    completion.completed_tasks = status.completed_tasks;
    completion.failed_tasks = status.failed_tasks;
    completion.is_idle = completion.pending_tasks == 0;
    return completion;
}

ExecutorSnapshot Executor::get_snapshot() const {
    return monitor_->collect();
}

std::string Executor::get_snapshot_text() const {
    std::string text = monitor::format_executor_snapshot(get_snapshot());
    // 0.7.0 M2：追加反馈聚合段（无样本时只输出汇总计数行）。
    text += scheduling::FeedbackAggregator::format_text(
        *feedback_aggregator_.refresh_if_stale());
    return text;
}

scheduling::FeedbackSnapshot Executor::get_feedback_snapshot() const {
    // 超过聚合器 merge_interval 未合并时先重合并（诊断读方驱动周期合并；
    // 读路径本身无锁）。
    feedback_aggregator_.refresh_if_stale();
    return *feedback_aggregator_.snapshot();
}

void Executor::emit_snapshot_diagnostic() const {
    emit_snapshot_diagnostic(get_snapshot());
}

void Executor::emit_snapshot_diagnostic(const ExecutorSnapshot& snapshot) const {
    ExecutorSnapshotCallback callback;
    {
        std::lock_guard<std::mutex> lock(snapshot_diagnostic_mutex_);
        callback = snapshot_diagnostic_callback_;
    }
    if (!callback) {
        return;
    }
    try {
        callback(snapshot);
    } catch (...) {
        // Diagnostics must not change facade results or lifecycle behavior.
    }
}

} // namespace kairo
