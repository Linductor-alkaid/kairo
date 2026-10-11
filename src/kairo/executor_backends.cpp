#include "kairo/executor.hpp"

#include "executor_detail.hpp"

// Executor facade — 专用后端单元：realtime / blocking-IO / lockfree 执行器的
// 注册、启停、投递与状态查询，以及 GPU facade 透传。
// （v0.7.0 M0：自 executor.cpp 按职责拆分，纯结构改动，零行为变化。）

namespace kairo {

using detail::make_failure;
using detail::validate_realtime_config;
using detail::validate_blocking_io_config;
using detail::validate_gpu_config_for_facade;
using detail::check_gpu_backend_available;

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
