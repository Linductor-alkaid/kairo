#include "kairo/executor.hpp"

#include "kairo/monitor/executor_snapshot_formatter.hpp"
#include "kairo/monitor/executor_monitor.hpp"
#include "thread_pool_executor.hpp"
#include "thread_pool/thread_pool.hpp"
#include "task/task.hpp"
#include "task/task_dependency_manager.hpp"
#include <stdexcept>
#include <algorithm>
#include <chrono>
#include <iterator>
#include <memory>

namespace kairo {

namespace {

ExecutorResult make_failure(ExecutorErrorCode code, const std::string& message) {
    return ExecutorResult::failure(code, message);
}

ExecutorResult validate_executor_config(const ExecutorConfig& config) {
    if (config.min_threads != 0 && config.max_threads != 0 &&
        config.min_threads > config.max_threads) {
        return make_failure(
            ExecutorErrorCode::InvalidConfig,
            "ExecutorConfig invalid: min_threads must be <= max_threads");
    }
    return ExecutorResult::success();
}

ExecutorResult validate_realtime_config(const std::string& name,
                                        const RealtimeThreadConfig& config) {
    if (name.empty()) {
        return make_failure(
            ExecutorErrorCode::InvalidConfig,
            "Realtime executor name must not be empty");
    }
    if (config.thread_name.empty()) {
        return make_failure(
            ExecutorErrorCode::InvalidConfig,
            "RealtimeThreadConfig invalid: thread_name must not be empty");
    }
    if (config.cycle_period_ns <= 0) {
        return make_failure(
            ExecutorErrorCode::InvalidConfig,
            "RealtimeThreadConfig invalid: cycle_period_ns must be greater than 0");
    }
    return ExecutorResult::success();
}

ExecutorResult validate_blocking_io_config(const std::string& name,
                                           const BlockingIoConfig& config,
                                           const IBlockingIoWorker* worker) {
    if (name.empty()) {
        return make_failure(ExecutorErrorCode::InvalidConfig,
                            "Blocking I/O executor name must not be empty");
    }
    if (config.thread_name.empty()) {
        return make_failure(ExecutorErrorCode::InvalidConfig,
                            "BlockingIoConfig invalid: thread_name must not be empty");
    }
    if (config.startup_timeout.count() < 0) {
        return make_failure(ExecutorErrorCode::InvalidConfig,
                            "BlockingIoConfig invalid: startup_timeout must not be negative");
    }
    if (!worker) {
        return make_failure(ExecutorErrorCode::InvalidConfig,
                            "Blocking I/O worker must not be null");
    }
    return ExecutorResult::success();
}

ExecutorResult validate_gpu_config_for_facade(
    const std::string& name,
    const gpu::GpuExecutorConfig& config) {
    if (name.empty()) {
        return make_failure(
            ExecutorErrorCode::InvalidConfig,
            "GPU executor name must not be empty");
    }
    if (config.name.empty()) {
        return make_failure(
            ExecutorErrorCode::InvalidConfig,
            "GpuExecutorConfig invalid: config.name must not be empty");
    }
    if (config.max_queue_size == 0) {
        return make_failure(
            ExecutorErrorCode::InvalidConfig,
            "GpuExecutorConfig invalid: max_queue_size must be greater than 0");
    }
    if (config.device_id < 0) {
        return make_failure(
            ExecutorErrorCode::InvalidConfig,
            "GpuExecutorConfig invalid: device_id must be non-negative");
    }
    if (config.default_stream_count < 1) {
        return make_failure(
            ExecutorErrorCode::InvalidConfig,
            "GpuExecutorConfig invalid: default_stream_count must be at least 1");
    }
    return ExecutorResult::success();
}

ExecutorResult check_gpu_backend_available(const gpu::GpuExecutorConfig& config) {
#ifndef KAIRO_ENABLE_GPU
    (void)config;
    return make_failure(
        ExecutorErrorCode::BackendUnavailable,
        "GPU support is not enabled in this build");
#else
    switch (config.backend) {
    case gpu::GpuBackend::CUDA:
#ifndef KAIRO_ENABLE_CUDA
        return make_failure(
            ExecutorErrorCode::BackendUnavailable,
            "CUDA backend is not enabled in this build");
#else
        return ExecutorResult::success();
#endif
    case gpu::GpuBackend::OPENCL:
#ifndef KAIRO_ENABLE_OPENCL
        return make_failure(
            ExecutorErrorCode::BackendUnavailable,
            "OpenCL backend is not enabled in this build");
#else
        return ExecutorResult::success();
#endif
    case gpu::GpuBackend::SYCL:
        return make_failure(
            ExecutorErrorCode::BackendUnavailable,
            "SYCL backend is not implemented in this build");
    case gpu::GpuBackend::HIP:
        return make_failure(
            ExecutorErrorCode::BackendUnavailable,
            "HIP backend is not implemented in this build");
    default:
        return make_failure(
            ExecutorErrorCode::BackendUnavailable,
            "Requested GPU backend is unavailable");
    }
#endif
}

}  // namespace

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
    , task_dependencies_(std::make_unique<TaskDependencyManager>()) {
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
    , task_dependencies_(std::make_unique<TaskDependencyManager>()) {
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

void Executor::set_timer_thread_factory_for_test(
    std::function<std::thread(std::function<void()>)> factory) {
    ensure_timers().set_thread_factory_for_test(std::move(factory));
}

TaskHandle Executor::allocate_task_handle() {
    TaskHandle handle(generate_task_id());
    {
        std::lock_guard<std::mutex> lock(task_graph_mutex_);
        task_graph_nodes_.emplace(handle.id(), TaskGraphNode{});
    }
    manager_->record_in_flight_task_pending(handle.id(), "task_graph", "default");
    return handle;
}

void Executor::set_task_graph_retention_capacity(size_t capacity) {
    std::lock_guard<std::mutex> lock(task_graph_mutex_);
    task_graph_retention_capacity_ = capacity;
    trim_task_graph_retention_locked();
}

size_t Executor::task_graph_retention_capacity() const {
    std::lock_guard<std::mutex> lock(task_graph_mutex_);
    return task_graph_retention_capacity_;
}

bool Executor::task_handle_known_locked(const TaskHandle& handle) const {
    return handle.valid() && task_graph_nodes_.find(handle.id()) != task_graph_nodes_.end();
}

bool Executor::register_task_graph_dependencies(
    const TaskHandle& handle,
    const std::vector<TaskHandle>& dependencies,
    std::string& error_message) {
    std::lock_guard<std::mutex> lock(task_graph_mutex_);
    for (const auto& dependency : dependencies) {
        if (!task_handle_known_locked(dependency)) {
            error_message = "submit_after dependency handle is invalid";
            return false;
        }
        if (!task_dependencies_->add_dependency(handle.id(), dependency.id())) {
            error_message = "submit_after dependency graph contains a cycle or invalid edge";
            return false;
        }
        task_graph_dependents_[dependency.id()].push_back(handle.id());
        task_graph_nodes_[handle.id()].dependencies.push_back(dependency.id());
    }
    return true;
}

std::exception_ptr Executor::dependency_failure_locked(
    const std::vector<TaskHandle>& dependencies) const {
    for (const auto& dependency : dependencies) {
        auto it = task_graph_nodes_.find(dependency.id());
        if (it == task_graph_nodes_.end()) {
            return make_dependency_exception("dependency handle is invalid");
        }
        if (it->second.state == TaskGraphState::Failed) {
            if (it->second.exception) {
                return it->second.exception;
            }
            return make_dependency_exception(
                it->second.error_message.empty()
                    ? "dependency failed"
                    : it->second.error_message);
        }
    }
    return nullptr;
}

bool Executor::dependencies_succeeded_locked(
    const std::vector<TaskHandle>& dependencies) const {
    for (const auto& dependency : dependencies) {
        auto it = task_graph_nodes_.find(dependency.id());
        if (it == task_graph_nodes_.end() ||
            it->second.state != TaskGraphState::Succeeded) {
            return false;
        }
    }
    return true;
}

void Executor::mark_task_graph_running(const TaskHandle& handle) {
    std::lock_guard<std::mutex> lock(task_graph_mutex_);
    auto it = task_graph_nodes_.find(handle.id());
    if (it != task_graph_nodes_.end() && it->second.state == TaskGraphState::Pending) {
        it->second.state = TaskGraphState::Running;
    }
    manager_->record_in_flight_task_state(handle.id(), TaskLifecycleState::Running);
}

void Executor::mark_task_graph_succeeded(const TaskHandle& handle,
                                         ParkedDrainBag* deferred) {
    ParkedDrainBag local_bag;
    ParkedDrainBag& bag = deferred ? *deferred : local_bag;
    {
        std::lock_guard<std::mutex> lock(task_graph_mutex_);
        auto it = task_graph_nodes_.find(handle.id());
        if (it != task_graph_nodes_.end()) {
            it->second.state = TaskGraphState::Succeeded;
            it->second.exception = nullptr;
            it->second.error_message.clear();
            it->second.parked.reset();
            task_dependencies_->mark_completed(handle.id());
            resolve_task_graph_dependents_locked(
                handle.id(), bag.ready, bag.failures);
            finalize_task_graph_node_locked(handle.id());
        }
    }
    manager_->record_in_flight_task_terminal(handle.id());
    if (!deferred) {
        drain_parked_resolutions(bag.ready, bag.failures);
    }
}

void Executor::mark_task_graph_failed(const TaskHandle& handle,
                                      std::exception_ptr exception,
                                      std::string message,
                                      ParkedDrainBag* deferred) {
    ParkedDrainBag local_bag;
    ParkedDrainBag& bag = deferred ? *deferred : local_bag;
    {
        std::lock_guard<std::mutex> lock(task_graph_mutex_);
        auto it = task_graph_nodes_.find(handle.id());
        if (it != task_graph_nodes_.end()) {
            it->second.state = TaskGraphState::Failed;
            it->second.exception = exception;
            it->second.error_message = std::move(message);
            // 终态节点的驻留载荷必须清空：取消 parked 任务时由这里释放
            // 载荷（wrapper 永不运行），级联据此跳过已结算节点。
            it->second.parked.reset();
            resolve_task_graph_dependents_locked(
                handle.id(), bag.ready, bag.failures);
            finalize_task_graph_node_locked(handle.id());
        }
    }
    manager_->record_in_flight_task_terminal(handle.id());
    if (!deferred) {
        drain_parked_resolutions(bag.ready, bag.failures);
    }
}

void Executor::resolve_task_graph_dependents_locked(
    const std::string& task_id,
    std::vector<ParkedReady>& ready_parked,
    std::vector<std::function<void()>>& deferred_failures) {
    std::vector<std::string> ready_ids{task_id};
    std::vector<std::string> terminal_ids;

    while (!ready_ids.empty()) {
        const std::string current_id = std::move(ready_ids.back());
        ready_ids.pop_back();

        auto dependents_it = task_graph_dependents_.find(current_id);
        if (dependents_it == task_graph_dependents_.end()) {
            continue;
        }

        const auto dependent_ids = dependents_it->second;
        for (const auto& dependent_id : dependent_ids) {
            auto node_it = task_graph_nodes_.find(dependent_id);
            if (node_it == task_graph_nodes_.end()) {
                continue;
            }
            auto& dependent_node = node_it->second;

            // parked 依赖图任务（dependency-driven 调度）：任一依赖失败即
            // 失败结算；否则递减未满足计数，归零时收集出队项（executor
            // 提交在锁外由 drain_parked_resolutions 执行，避免持图锁调用
            // 执行器提交路径）。
            if (dependent_node.parked &&
                dependent_node.state == TaskGraphState::Pending) {
                std::vector<TaskHandle> dependencies;
                for (const auto& dependency_id : dependent_node.dependencies) {
                    dependencies.emplace_back(dependency_id);
                }

                if (auto dependency_exception =
                        dependency_failure_locked(dependencies)) {
                    dependency_exception = reclassify_dependency_exception(
                        dependency_exception);
                    dependent_node.state = TaskGraphState::Failed;
                    dependent_node.exception = dependency_exception;
                    dependent_node.error_message =
                        "Dependency failed before dependent task execution";
                    auto payload = std::move(dependent_node.parked);
                    dependent_node.parked.reset();
                    ready_ids.push_back(dependent_id);
                    terminal_ids.push_back(dependent_id);
                    manager_->record_in_flight_task_terminal(dependent_id);
                    deferred_failures.push_back(
                        [payload, exception = dependency_exception]() mutable {
                            payload->settle_without_run(exception);
                        });
                    continue;
                }

                if (dependent_node.unmet_count > 0) {
                    --dependent_node.unmet_count;
                }
                if (dependent_node.unmet_count == 0) {
                    auto payload = std::move(dependent_node.parked);
                    dependent_node.parked.reset();
                    // 节点保持 Pending：与既有"已入队未开始"语义一致，
                    // wrapper 运行时经 mark_task_graph_running 转 Running。
                    std::string resolved_executor_name = "default";
                    if (payload && payload->executor) {
                        resolved_executor_name = payload->executor->get_name();
                    }
                    ready_parked.push_back(ParkedReady{
                        TaskHandle(dependent_id), std::move(resolved_executor_name),
                        std::move(payload)});
                }
                continue;
            }

            if (dependent_node.state != TaskGraphState::WhenAll) {
                continue;
            }

            std::vector<TaskHandle> dependencies;
            for (const auto& dependency_id : dependent_node.dependencies) {
                dependencies.emplace_back(dependency_id);
            }

            if (auto dependency_exception = dependency_failure_locked(dependencies)) {
                dependent_node.state = TaskGraphState::Failed;
                dependent_node.exception = dependency_exception;
                dependent_node.error_message = "when_all dependency failed";
                ready_ids.push_back(dependent_id);
                terminal_ids.push_back(dependent_id);
                manager_->record_in_flight_task_terminal(dependent_id);
            } else if (dependencies_succeeded_locked(dependencies)) {
                dependent_node.state = TaskGraphState::Succeeded;
                dependent_node.exception = nullptr;
                dependent_node.error_message.clear();
                task_dependencies_->mark_completed(dependent_id);
                ready_ids.push_back(dependent_id);
                terminal_ids.push_back(dependent_id);
                manager_->record_in_flight_task_terminal(dependent_id);
            }
        }
    }

    for (const auto& terminal_id : terminal_ids) {
        finalize_task_graph_node_locked(terminal_id);
    }
}

void Executor::drain_parked_resolutions(
    std::vector<ParkedReady>& ready_parked,
    std::vector<std::function<void()>>& deferred_failures) {
    // 依赖失败的 parked 结算先执行：尽快释放 admission 与 registry 槽位。
    for (auto& failure : deferred_failures) {
        if (failure) {
            failure();
        }
    }

    // ready 出队：提交时定格的 priority/executor 原样使用；提交被拒按
    // 既有 on_rejected 语义落 Failed 并结算，不让任务静默消失。
    for (auto& ready : ready_parked) {
        if (!ready.payload || !ready.payload->executor) {
            continue;
        }
        bool accepted = false;
        try {
            if (ready.payload->priority) {
                accepted = ready.payload->executor->try_submit_priority_task(
                    *ready.payload->priority,
                    std::move(ready.payload->wrapper),
                    std::move(ready.payload->on_timeout));
            } else {
                accepted = ready.payload->executor->try_submit_task(
                    std::move(ready.payload->wrapper),
                    std::move(ready.payload->on_timeout));
            }
        } catch (...) {
            accepted = false;
        }
        if (accepted) {
            // PR-3 监控补记：parked 任务在真正进入执行器队列时补记 Queued
            // （设计 §7：DependencyBlocked → Queued → Running）。
            manager_->record_in_flight_task_state(
                ready.handle.id(), TaskLifecycleState::Queued);
        }
        if (!accepted) {
            auto exception = std::make_exception_ptr(std::runtime_error(
                "Async executor rejected task submission"));
            mark_task_graph_failed(
                ready.handle, exception,
                "Tracked task submission rejected after dependency readiness");
            ready.payload->settle_without_run(exception);
            record_submit_rejected(
                ready.executor_name, ready.handle.id(),
                "Async executor rejected tracked submission after dependency "
                "readiness",
                exception);
        }
    }
}

void Executor::finalize_task_graph_node_locked(const std::string& task_id) {
    auto node_it = task_graph_nodes_.find(task_id);
    if (node_it == task_graph_nodes_.end()) {
        return;
    }

    auto& node = node_it->second;
    if (node.state != TaskGraphState::Succeeded &&
        node.state != TaskGraphState::Failed) {
        return;
    }

    // Remove this node from every dependency's reverse edge.  A dependency
    // becomes evictable only after all active dependents have finished.
    for (const auto& dependency_id : node.dependencies) {
        auto dependents_it = task_graph_dependents_.find(dependency_id);
        if (dependents_it != task_graph_dependents_.end()) {
            auto& dependents = dependents_it->second;
            dependents.erase(
                std::remove(dependents.begin(), dependents.end(), task_id),
                dependents.end());
            if (dependents.empty()) {
                task_graph_dependents_.erase(dependents_it);
            }
        }
        task_dependencies_->remove_dependency(task_id, dependency_id);
    }
    node.dependencies.clear();
    task_graph_terminal_order_.push_back(task_id);
    trim_task_graph_retention_locked();
}

void Executor::trim_task_graph_retention_locked() {
    // CR-102 结论（2026-10-01 复测）：审查主张的 O(capacity) 每终态 7-10µs
    // 在当前 HEAD 不复现——独立任务负载下 find_if 扫描深度恒为 1（最老终态
    // 立即可驱逐），单次 trim 为 O(1)（deque pop_front + 两个哈希擦除 +
    // prune），cap=1024 与 cap=0 的差距在测量噪声内（交错三轮 ±20% 摆动，
    // 大小关系翻转）。线性扫描仅在这些旧终态仍有未决 dependent 时出现，
    // 那是精确保留契约的组成部分（ActiveDependentPreventsEarlyHandleExpiration）。
    // 曾试高水位摊还方案，被 retention 精确上界契约测试否决——保留即时过期语义。
    while (task_graph_terminal_order_.size() > task_graph_retention_capacity_) {
        auto candidate = std::find_if(
            task_graph_terminal_order_.begin(), task_graph_terminal_order_.end(),
            [this](const std::string& task_id) {
                const auto it = task_graph_dependents_.find(task_id);
                return it == task_graph_dependents_.end() || it->second.empty();
            });
        if (candidate == task_graph_terminal_order_.end()) {
            // Every old terminal node is still needed by an active dependent.
            // The active graph is allowed to exceed the terminal cache bound.
            break;
        }

        const std::string task_id = *candidate;
        task_graph_terminal_order_.erase(candidate);
        task_graph_nodes_.erase(task_id);
        task_graph_dependents_.erase(task_id);
        task_dependencies_->prune(task_id);
    }
}

std::exception_ptr Executor::make_dependency_exception(const std::string& message) const {
    return std::make_exception_ptr(std::runtime_error(message));
}

TaskHandle Executor::when_all(std::vector<TaskHandle> dependencies) {
    TaskHandle handle = allocate_task_handle();

    bool dependencies_valid = true;
    bool terminal = false;
    std::string validation_error;
    {
        std::lock_guard<std::mutex> lock(task_graph_mutex_);
        for (const auto& dependency : dependencies) {
            if (!task_handle_known_locked(dependency)) {
                dependencies_valid = false;
                validation_error = "when_all dependency handle is invalid";
                break;
            }
            if (!task_dependencies_->add_dependency(handle.id(), dependency.id())) {
                dependencies_valid = false;
                validation_error = "when_all dependency graph contains a cycle or invalid edge";
                break;
            }
            task_graph_dependents_[dependency.id()].push_back(handle.id());
            task_graph_nodes_[handle.id()].dependencies.push_back(dependency.id());
        }
        if (dependencies_valid) {
            auto& node = task_graph_nodes_[handle.id()];
            if (auto dependency_exception = dependency_failure_locked(dependencies)) {
                node.state = TaskGraphState::Failed;
                node.exception = dependency_exception;
                node.error_message = "when_all dependency failed";
                terminal = true;
            } else if (dependencies_succeeded_locked(dependencies)) {
                node.state = TaskGraphState::Succeeded;
                task_dependencies_->mark_completed(handle.id());
                terminal = true;
            } else {
                node.state = TaskGraphState::WhenAll;
            }
            if (terminal) {
                finalize_task_graph_node_locked(handle.id());
            }
        }
    }

    if (!dependencies_valid) {
        auto exception = make_dependency_exception(validation_error);
        mark_task_graph_failed(handle, exception, validation_error);
        record_submit_rejected("default", handle.id(), validation_error, exception);
        return handle;
    }

    if (terminal) {
        manager_->record_in_flight_task_terminal(handle.id());
    } else {
        manager_->record_in_flight_task_state(
            handle.id(), TaskLifecycleState::DependencyBlocked);
    }

    return handle;
}

// ---------------------------------------------------------------------------
// 定时器基础设施与周期任务
// ---------------------------------------------------------------------------

detail::TimerScheduler& Executor::ensure_timers() {
    // timers_ 在构造函数创建且生命周期内地址稳定；此处仅做防御性兜底
    // （例如用户在其它成员构造完成前经由合法路径进入）。
    std::lock_guard<std::mutex> lock(timers_mutex_);
    if (!timers_) {
        timers_ = std::make_shared<detail::TimerScheduler>();
        configure_timer_scheduler_hooks();
    }
    return *timers_;
}

void Executor::configure_timer_scheduler_hooks() {
    if (!timers_) {
        return;
    }
    // 取消传播 hook：向已派发任务（delayed 派发后 / periodic 在途 tick）
    // 传播排队/运行中取消。graph_handle 为空：定时任务不在任务图中。
    timers_->set_task_cancel_hook(
        [this](const std::string& task_state_id,
               const std::shared_ptr<TaskCancellationState>& state) noexcept {
            propagate_timer_task_cancel(task_state_id, state);
        });
}

void Executor::start_timer_thread() {
    ensure_timers().start();
}

void Executor::stop_timer_thread() {
    if (timers_) {
        timers_->stop();
    }
}

// 提交带句柄的周期任务
TimerHandle Executor::submit_periodic(int64_t period_ms,
                                      std::function<void()> task) {
    if (period_ms <= 0) {
        throw std::invalid_argument("period_ms must be greater than 0");
    }
    if (!task) {
        throw std::invalid_argument("task must not be null");
    }

    auto executor = manager_->get_default_async_executor_snapshot();
    const std::string executor_name = executor ? executor->get_name() : "default";
    if (!executor) {
        record_submit_rejected(
            executor_name,
            "facade_submit_periodic",
            "Async executor not initialized. Call initialize() first.");
        throw std::runtime_error("Async executor not initialized. Call initialize() first.");
    }

    const std::string task_id = generate_task_id();

    detail::TimerScheduler::TickBuilderFactory tick_builder =
        [this, executor_name, task_id, task = std::move(task)]()
            -> detail::TimerTickPlan {
        std::function<void()> pool_task =
            [this, executor_name, task_id, task]() mutable {
                try {
                    task();
                    timers_->report_tick_success(task_id);
                } catch (...) {
                    auto exception = std::current_exception();
                    timers_->report_tick_failure(
                        task_id, "Periodic task threw an exception");
                    record_periodic_task_exception(
                        executor_name,
                        task_id,
                        "Periodic task threw an exception",
                        exception);
                    throw;
                }
            };

        std::function<void()> dispatch =
            [this, executor_name, task_id,
             pool_task = std::move(pool_task)]() mutable {
            auto executor_snapshot = manager_->get_default_async_executor_snapshot();
            if (!executor_snapshot) {
                record_periodic_submit_rejected(
                    executor_name,
                    task_id,
                    "Async executor unavailable for periodic task");
                return;
            }
            if (!executor_snapshot->try_submit_task(std::move(pool_task))) {
                auto exception = std::make_exception_ptr(std::runtime_error(
                    "Async executor rejected periodic task submission"));
                record_periodic_submit_rejected(
                    executor_name,
                    task_id,
                    "Async executor rejected periodic task submission",
                    exception);
            }
        };

        return detail::TimerTickPlan{std::move(dispatch), nullptr, {}};
    };

    try {
        start_timer_thread();
    } catch (...) {
        auto exception = std::current_exception();
        record_submit_rejected(
            executor_name,
            task_id,
            "Timer thread creation failed for periodic task",
            exception);
        throw;
    }

    const std::string scheduled_id =
        ensure_timers().schedule_periodic(period_ms, task_id,
                                          std::move(tick_builder));
    if (scheduled_id.empty()) {
        auto exception = std::make_exception_ptr(std::runtime_error(
            "Timer stopped before periodic task execution"));
        record_submit_rejected(executor_name, task_id,
                               "Timer stopped before periodic task execution",
                               exception);
        throw std::runtime_error(
            "Timer stopped before periodic task execution");
    }


    return TimerHandle(task_id, timers_);
}

TimerHandle Executor::submit_periodic_cancellable(
    int64_t period_ms, std::function<void(StopToken)> task) {
    if (period_ms <= 0) {
        throw std::invalid_argument("period_ms must be greater than 0");
    }
    if (!task) {
        throw std::invalid_argument("task must not be null");
    }

    auto executor = manager_->get_default_async_executor_snapshot();
    const std::string executor_name = executor ? executor->get_name() : "default";
    if (!executor) {
        record_submit_rejected(
            executor_name,
            "facade_submit_periodic_cancellable",
            "Async executor not initialized. Call initialize() first.");
        throw std::runtime_error("Async executor not initialized. Call initialize() first.");
    }

    const std::string timer_id = generate_task_id();

    detail::TimerScheduler::TickBuilderFactory tick_builder =
        [this, timer_id, task = std::move(task)]() -> detail::TimerTickPlan {
        auto state = std::make_shared<TaskCancellationState>();
        const std::string tick_id = generate_task_id();

        std::function<void()> pool_task =
            [this, timer_id, tick_id, state, task]() mutable {
                if (!state->try_begin_execution()) {
                    timers_->release_tick(timer_id, tick_id);
                    return;  // 已取消，无 future 需要满足
                }
                try {
                    task(state->stop_token());
                } catch (const TaskCancelled&) {
                    if (state->cancel_requested()) {
                        state->try_finish_running(
                            TaskCancellationState::Phase::Cancelled);
                        timers_->release_tick(timer_id, tick_id);
                        return;  // 协作取消：生命周期事件，不记 failure
                    }
                    throw;
                } catch (...) {
                    auto exception = std::current_exception();
                    state->try_finish_running(
                        TaskCancellationState::Phase::Failed);
                    record_task_exception(
                        "default",
                        tick_id,
                        "Periodic cancellable tick threw an exception",
                        exception);
                    timers_->report_tick_failure(
                        timer_id, "Periodic task threw an exception");
                    timers_->release_tick(timer_id, tick_id);
                    throw;
                }
                state->try_finish_running(
                    TaskCancellationState::Phase::Succeeded);
                if (state->cancel_requested()) {
                    cancellation_registry_->on_completed_after_request();
                }
                timers_->report_tick_success(timer_id);
                timers_->release_tick(timer_id, tick_id);
            };

        std::function<void()> dispatch =
            [this, timer_id, tick_id, state,
             pool_task = std::move(pool_task)]() mutable {
            auto executor_snapshot = manager_->get_default_async_executor_snapshot();
            if (!executor_snapshot) {
                state->try_reject();
                record_periodic_submit_rejected(
                    "default", timer_id,
                    "Async executor unavailable for periodic task");
                return;
            }
            if (!executor_snapshot->try_submit_task(std::move(pool_task))) {
                state->try_reject();
                auto exception = std::make_exception_ptr(std::runtime_error(
                    "Async executor rejected periodic task submission"));
                record_periodic_submit_rejected(
                    "default", timer_id,
                    "Async executor rejected periodic task submission",
                    exception);
                // pool_task 不会运行，active 登记在此释放。
                timers_->release_tick(timer_id, tick_id);
            }
        };

        return detail::TimerTickPlan{std::move(dispatch), std::move(state),
                                     tick_id};
    };

    try {
        start_timer_thread();
    } catch (...) {
        auto exception = std::current_exception();
        record_submit_rejected(
            executor_name,
            timer_id,
            "Timer thread creation failed for periodic task",
            exception);
        throw;
    }

    const std::string scheduled_id =
        ensure_timers().schedule_periodic(period_ms, timer_id,
                                          std::move(tick_builder));
    if (scheduled_id.empty()) {
        auto exception = std::make_exception_ptr(std::runtime_error(
            "Timer stopped before periodic task execution"));
        record_submit_rejected(executor_name, timer_id,
                               "Timer stopped before periodic task execution",
                               exception);
        throw std::runtime_error(
            "Timer stopped before periodic task execution");
    }

    return TimerHandle(timer_id, timers_);
}

TimerStatusSummary Executor::get_timer_status_summary() const {
    if (!timers_) {
        return {};
    }
    return timers_->summary();
}

// ---------------------------------------------------------------------------
// 任务级协作取消（C1）
// ---------------------------------------------------------------------------

TaskCancellationResponse Executor::request_task_cancel(
    const TaskHandle& handle) noexcept {
    try {
        if (!handle.valid()) {
            return TaskCancellationResponse{
                TaskCancellationResult::NotFound};
        }

        std::shared_ptr<TaskCancellationState> state;
        const auto lookup = cancellation_registry_->find(handle.id(), state);
        if (lookup == TaskCancellationRegistry::LookupResult::NotFound) {
            return TaskCancellationResponse{TaskCancellationResult::NotFound};
        }
        if (lookup == TaskCancellationRegistry::LookupResult::Terminal) {
            return TaskCancellationResponse{
                TaskCancellationResult::AlreadyCompleted};
        }

        return propagate_cancel_state(handle.id(), state, &handle);
    } catch (...) {
        return TaskCancellationResponse{TaskCancellationResult::NotFound};
    }
}

TaskCancellationResponse Executor::propagate_cancel_state(
    const std::string& task_id,
    const std::shared_ptr<TaskCancellationState>& state,
    const TaskHandle* graph_handle) noexcept {
    try {
        if (!state) {
            return TaskCancellationResponse{TaskCancellationResult::NotFound};
        }
        if (state->terminal()) {
            // 工作线程刚到达终态但 registry finalize 尚未可见。
            return TaskCancellationResponse{
                TaskCancellationResult::AlreadyCompleted};
        }

        const bool first_request = state->mark_cancel_requested_once();

        if (state->try_cancel_before_start()) {
            // 排队取消：取消方立即满足 future，不依赖 worker 何时取到节点。
            state->stop_source().request_stop();
            state->notify_cancelled(std::make_exception_ptr(TaskCancelled(
                TaskCancellationReason::Explicit,
                "Task cancelled before execution")));
            cancellation_registry_->on_first_request(/*queued_cancel=*/true);
            if (graph_handle) {
                auto exception = std::make_exception_ptr(TaskCancelled(
                    TaskCancellationReason::Explicit,
                    "Task cancelled before execution"));
                ParkedDrainBag drain_bag;
                mark_task_graph_failed(
                    *graph_handle, exception, "Task cancelled before execution",
                    &drain_bag);
                manager_->record_in_flight_task_state(
                    task_id, TaskLifecycleState::Cancelled);
                // 排队取消路径同样外移 drain：被取消节点的下游 dependent
                // 在本任务完全终态（registry finalize 等）之后才结算。
                drain_parked_resolutions(drain_bag.ready, drain_bag.failures);
            }
            manager_->record_in_flight_task_terminal(task_id);
            cancellation_registry_->finalize(task_id);
            return TaskCancellationResponse{
                TaskCancellationResult::RequestedBeforeStart};
        }

        if (state->phase() == TaskCancellationState::Phase::Running) {
            // 运行中：协作请求，不抢占、不中断。
            state->stop_source().request_stop();
            if (first_request) {
                cancellation_registry_->on_first_request(
                    /*queued_cancel=*/false);
            }
            return TaskCancellationResponse{
                first_request
                    ? TaskCancellationResult::RequestedRunning
                    : TaskCancellationResult::AlreadyRequested};
        }

        return TaskCancellationResponse{
            TaskCancellationResult::AlreadyCompleted};
    } catch (...) {
        return TaskCancellationResponse{TaskCancellationResult::NotFound};
    }
}

void Executor::propagate_timer_task_cancel(
    const std::string& task_state_id,
    const std::shared_ptr<TaskCancellationState>& state) noexcept {
    // 定时任务不在任务图中：graph_handle 为空，仅做状态仲裁与计数。
    (void)propagate_cancel_state(task_state_id, state, nullptr);
}

CancellationStatus Executor::get_cancellation_status() const {
    return cancellation_registry_->status();
}

size_t Executor::closure_graveyard_size() const {
    std::lock_guard<std::mutex> lock(task_graph_mutex_);
    return closure_graveyard_.size();
}

void Executor::set_cancellation_registry_capacity(size_t capacity) {
    cancellation_registry_->set_capacity(capacity);
}

size_t Executor::cancellation_registry_capacity() const {
    return cancellation_registry_->capacity();
}

std::exception_ptr Executor::reclassify_dependency_exception(
    std::exception_ptr exception) const {
    if (!exception) {
        return exception;
    }
    try {
        std::rethrow_exception(exception);
    } catch (const TaskCancelled& cancelled) {
        if (cancelled.reason() == TaskCancellationReason::DependencyCancelled) {
            return exception;
        }
        return std::make_exception_ptr(TaskCancelled(
            TaskCancellationReason::DependencyCancelled,
            "Dependency was cancelled"));
    } catch (...) {
        return exception;  // 非取消类依赖失败：保持原异常
    }
}

std::optional<PeriodicTaskStatus> Executor::get_periodic_task_status(
    const std::string& task_id) const {
    if (!timers_) {
        return std::nullopt;
    }
    return timers_->get_periodic_status(task_id);
}

std::vector<PeriodicTaskStatus> Executor::get_all_periodic_task_status() const {
    if (!timers_) {
        return {};
    }
    return timers_->get_all_periodic_status();
}

// 注册实时任务
ExecutorResult Executor::register_realtime_task(
    const std::string& name,
    const RealtimeThreadConfig& config) {
    if (auto validation = validate_realtime_config(name, config); !validation.ok) {
        record_result_failure(
            validation, FailureKind::SubmitRejected, name, "facade_register_realtime_task");
        return validation;
    }

    auto executor = manager_->create_realtime_executor(name, config);
    if (!executor) {
        auto result = make_failure(
            ExecutorErrorCode::InvalidConfig,
            "Realtime executor creation failed");
        record_result_failure(
            result, FailureKind::SubmitRejected, name, "facade_register_realtime_task");
        return result;
    }

    if (!manager_->register_realtime_executor(name, std::move(executor))) {
        auto result = make_failure(
            ExecutorErrorCode::DuplicateName,
            "Realtime executor registration failed or duplicate name");
        record_result_failure(
            result, FailureKind::SubmitRejected, name, "facade_register_realtime_task");
        return result;
    }

    return ExecutorResult::success("Realtime executor registered");
}

// 启动实时任务
ExecutorResult Executor::start_realtime_task(const std::string& name) {
    if (name.empty()) {
        auto result = make_failure(
            ExecutorErrorCode::InvalidConfig,
            "Realtime executor name must not be empty");
        record_result_failure(
            result, FailureKind::SubmitRejected, name, "facade_start_realtime_task");
        return result;
    }

    auto executor = manager_->get_realtime_executor_snapshot(name);
    if (!executor) {
        auto result = make_failure(
            ExecutorErrorCode::NotFound,
            "Realtime executor '" + name + "' not found");
        record_result_failure(
            result, FailureKind::SubmitRejected, name, "facade_start_realtime_task");
        return result;
    }

    if (!executor->start()) {
        const auto status = executor->get_status();
        auto code = status.is_running
                        ? ExecutorErrorCode::AlreadyInitialized
                        : ExecutorErrorCode::StartFailed;
        auto result = make_failure(
            code,
            status.is_running
                ? "Realtime executor '" + name + "' is already running"
                : "Realtime executor '" + name + "' start failed");
        record_result_failure(
            result, FailureKind::SubmitRejected, name, "facade_start_realtime_task");
        return result;
    }

    return ExecutorResult::success("Realtime executor started");
}

// 停止实时任务
void Executor::stop_realtime_task(const std::string& name) {
    auto executor = manager_->get_realtime_executor_snapshot(name);
    if (executor) {
        executor->stop();
    }
}

ExecutorResult Executor::register_blocking_io_worker(
    const std::string& name,
    const BlockingIoConfig& config,
    std::unique_ptr<IBlockingIoWorker> worker) {
    if (auto validation = validate_blocking_io_config(name, config, worker.get()); !validation.ok) {
        record_result_failure(
            validation, FailureKind::SubmitRejected, name, "facade_register_blocking_io_worker");
        return validation;
    }
    auto executor = manager_->create_blocking_io_executor(name, config, std::move(worker));
    if (!executor) {
        auto result = make_failure(ExecutorErrorCode::StartFailed,
                                   "Blocking I/O executor creation failed");
        record_result_failure(
            result, FailureKind::SubmitRejected, name, "facade_register_blocking_io_worker");
        return result;
    }
    if (!manager_->register_blocking_io_executor(name, std::move(executor))) {
        auto result = make_failure(ExecutorErrorCode::DuplicateName,
                                   "Blocking I/O executor registration failed or duplicate name");
        record_result_failure(
            result, FailureKind::SubmitRejected, name, "facade_register_blocking_io_worker");
        return result;
    }
    return ExecutorResult::success("Blocking I/O executor registered");
}

ExecutorResult Executor::start_blocking_io_worker(const std::string& name) {
    if (name.empty()) {
        auto result = make_failure(ExecutorErrorCode::InvalidConfig,
                                   "Blocking I/O executor name must not be empty");
        record_result_failure(
            result, FailureKind::SubmitRejected, name, "facade_start_blocking_io_worker");
        return result;
    }
    auto executor = manager_->get_blocking_io_executor_snapshot(name);
    if (!executor) {
        auto result = make_failure(ExecutorErrorCode::NotFound,
                                   "Blocking I/O executor '" + name + "' not found");
        record_result_failure(
            result, FailureKind::SubmitRejected, name, "facade_start_blocking_io_worker");
        return result;
    }
    if (!executor->start()) {
        const auto status = executor->get_status();
        const auto code = status.is_running ? ExecutorErrorCode::AlreadyInitialized
                                            : ExecutorErrorCode::StartFailed;
        auto result = make_failure(
            code,
            status.is_running ? "Blocking I/O executor '" + name + "' is already running"
                              : "Blocking I/O executor '" + name + "' start failed");
        record_result_failure(
            result, FailureKind::SubmitRejected, name, "facade_start_blocking_io_worker");
        return result;
    }
    return ExecutorResult::success("Blocking I/O executor started");
}

void Executor::stop_blocking_io_worker(const std::string& name) {
    manager_->stop_blocking_io_executor(name);
}

BlockingIoExecutorStatus Executor::get_blocking_io_worker_status(const std::string& name) const {
    return manager_->get_blocking_io_executor_status(name);
}

std::vector<std::string> Executor::get_blocking_io_worker_list() const {
    return manager_->get_blocking_io_executor_names();
}

WorkerHandle Executor::start_worker(BlockingWorkerSpec spec) {
    const std::string name = spec.name;
    auto result = register_blocking_io_worker(
        spec.name, spec.config, std::move(spec.worker));
    if (result.ok) {
        result = start_blocking_io_worker(name);
    }
    return WorkerHandle(manager_, name, std::move(result));
}

void WorkerHandle::request_stop() noexcept {
    if (manager_) {
        manager_->request_stop_blocking_io_executor(name_);
    }
}

void WorkerHandle::stop() {
    if (manager_) {
        manager_->stop_blocking_io_executor(name_);
    }
}

BlockingIoExecutorStatus WorkerHandle::status() const {
    if (manager_) {
        return manager_->get_blocking_io_executor_status(name_);
    }
    BlockingIoExecutorStatus status;
    status.name = name_;
    return status;
}

bool Executor::push_realtime_task(const std::string& name, std::function<void()> task) {
    auto executor = manager_->get_realtime_executor_snapshot(name);
    if (!executor) {
        record_submit_rejected(
            name,
            "facade_push_realtime_task",
            "Realtime executor not found");
        return false;
    }
    const auto before = executor->get_status();
    const auto push_result = executor->push_task(std::move(task));
    if (push_result.ok) {
        return true;
    }

    const auto after = executor->get_status();
    std::string message = "Realtime task push rejected";
    if (after.rejected_not_running_count > before.rejected_not_running_count) {
        message = "Realtime task push rejected: executor is not running";
    } else if (after.rejected_empty_task_count > before.rejected_empty_task_count) {
        message = "Realtime task push rejected: task is empty";
    } else if (after.pool_exhausted_count > before.pool_exhausted_count) {
        message = "Realtime task push rejected: task object pool exhausted";
    } else if (after.queue_full_count > before.queue_full_count ||
               after.failed_pushes > before.failed_pushes) {
        message = "Realtime task push rejected: queue is full";
    }

    record_realtime_drop(
        executor->get_name(),
        "facade_push_realtime_task",
        message);
    return false;
}

bool Executor::try_push_realtime_task(const std::string& name, std::function<void()> task) {
    return push_realtime_task(name, std::move(task));
}

// 获取实时执行器
IRealtimeExecutor* Executor::get_realtime_executor(const std::string& name) {
    return manager_->get_realtime_executor(name);
}

// 获取所有实时任务列表
std::vector<std::string> Executor::get_realtime_task_list() const {
    return manager_->get_realtime_executor_names();
}

bool Executor::register_lockfree_executor(
    const std::string& name,
    std::unique_ptr<LockFreeTaskExecutor> executor) {
    if (name.empty() || !executor || !manager_->register_lockfree_executor(name, std::move(executor))) {
        record_submit_rejected(name, "facade_register_lockfree_executor",
                               "Lock-free executor registration failed or duplicate name");
        return false;
    }
    return true;
}

bool Executor::start_lockfree_executor(const std::string& name) {
    if (!manager_->start_lockfree_executor(name)) {
        record_submit_rejected(name, "facade_start_lockfree_executor",
                               "Lock-free executor not found, already running, or stopped");
        return false;
    }
    return true;
}

void Executor::stop_lockfree_executor(const std::string& name) {
    manager_->stop_lockfree_executor(name);
}

std::vector<std::string> Executor::get_lockfree_executor_names() const {
    return manager_->get_lockfree_executor_names();
}

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
        result.decision.detail = "dispatch task is empty";
        result.message = result.decision.detail;
        record_routing_decision(result.decision);
        record_submit_rejected(result.executor_name, result.decision.task_name, result.message);
        return result;
    }

    if (result.decision.reason == RoutingReason::Rejected ||
        result.decision.reason == RoutingReason::BackendUnavailable ||
        result.decision.reason == RoutingReason::BackendNotRunning ||
        result.decision.reason == RoutingReason::CapacityPressure) {
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

// 获取实时执行器状态
RealtimeExecutorStatus Executor::get_realtime_executor_status(const std::string& name) const {
    auto executor = manager_->get_realtime_executor_snapshot(name);
    if (!executor) {
        RealtimeExecutorStatus status;
        status.name = name;
        status.is_running = false;
        return status;
    }
    
    return executor->get_status();
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
    return task_router_.route(
        TaskRouter::Request{options, cpu_gpu_task, gpu_selected},
        cpu_gpu_task ? manager_->get_executor_capabilities()
                     : std::vector<ExecutorCapability>{});
}

void Executor::record_routing_decision(RoutingDecision decision) {
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
    return monitor::format_executor_snapshot(get_snapshot());
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

// 注册 GPU 执行器
ExecutorResult Executor::register_gpu_executor(
    const std::string& name,
    const gpu::GpuExecutorConfig& config) {
    if (auto validation = validate_gpu_config_for_facade(name, config); !validation.ok) {
        record_result_failure(
            validation, FailureKind::GpuFailure, name, "facade_register_gpu_executor");
        return validation;
    }

    if (auto backend = check_gpu_backend_available(config); !backend.ok) {
        record_result_failure(
            backend, FailureKind::GpuFailure, name, "facade_register_gpu_executor");
        return backend;
    }

    auto executor = manager_->create_gpu_executor(config);
    if (!executor) {
        auto result = make_failure(
            ExecutorErrorCode::BackendUnavailable,
            "GPU executor creation failed");
        record_result_failure(
            result, FailureKind::GpuFailure, name, "facade_register_gpu_executor");
        return result;
    }

    if (!executor->start()) {
        auto status = executor->get_status();
        auto result = make_failure(
            ExecutorErrorCode::StartFailed,
            status.last_error_message.empty()
                ? "GPU executor start failed"
                : "GPU executor start failed: " + status.last_error_message);
        record_result_failure(
            result, FailureKind::GpuFailure, name, "facade_register_gpu_executor");
        return result;
    }

    if (!manager_->register_gpu_executor(name, std::move(executor))) {
        auto result = make_failure(
            ExecutorErrorCode::DuplicateName,
            "GPU executor registration failed or duplicate name");
        record_result_failure(
            result, FailureKind::GpuFailure, name, "facade_register_gpu_executor");
        return result;
    }

    return ExecutorResult::success("GPU executor registered");
}

// 获取 GPU 执行器
IGpuExecutor* Executor::get_gpu_executor(const std::string& name) {
    return manager_->get_gpu_executor(name);
}

// 获取所有 GPU 执行器名称
std::vector<std::string> Executor::get_gpu_executor_names() const {
    return manager_->get_gpu_executor_names();
}

// 获取 GPU 执行器状态
gpu::GpuExecutorStatus Executor::get_gpu_executor_status(const std::string& name) const {
    auto executor = manager_->get_gpu_executor_snapshot(name);
    if (!executor) {
        gpu::GpuExecutorStatus status;
        status.name = name;
        status.is_running = false;
        status.backend = gpu::GpuBackend::CUDA;  // 默认值
        status.device_id = 0;
        return status;
    }
    
    return executor->get_status();
}

// 获取所有 GPU 执行器状态
std::map<std::string, gpu::GpuExecutorStatus> Executor::get_all_gpu_executor_status() const {
    return manager_->get_all_gpu_executor_statuses();
}

// 更新调度器配置
void Executor::update_scheduler_config(const gpu::GpuScheduler::Config& config) {
    scheduler_.update_config(config);
}

// 获取调度器配置
gpu::GpuScheduler::Config Executor::get_scheduler_config() const {
    return scheduler_.get_config();
}

} // namespace kairo
