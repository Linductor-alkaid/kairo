#include "kairo/executor.hpp"

#include "task/task_dependency_manager.hpp"
#include <algorithm>
#include <stdexcept>

// Executor facade — tracked 任务图与协作取消单元：句柄分配、依赖登记、
// parked 依赖驱动调度、终态级联与 retention、按句柄取消（C1）。
// （v0.7.0 M0：自 executor.cpp 按职责拆分，纯结构改动，零行为变化。）

namespace kairo {

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

} // namespace kairo
