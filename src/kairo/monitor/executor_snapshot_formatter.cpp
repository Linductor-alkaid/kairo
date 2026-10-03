#include "kairo/monitor/executor_snapshot_formatter.hpp"

#include <iomanip>
#include <memory>
#include <sstream>

namespace kairo::monitor {
namespace {

const char* lifecycle_to_string(ExecutorLifecycleState state) {
    switch (state) {
    case ExecutorLifecycleState::Created: return "Created";
    case ExecutorLifecycleState::Initializing: return "Initializing";
    case ExecutorLifecycleState::Running: return "Running";
    case ExecutorLifecycleState::Draining: return "Draining";
    case ExecutorLifecycleState::Stopped: return "Stopped";
    case ExecutorLifecycleState::Failed: return "Failed";
    default: return "Unknown";
    }
}

const char* failure_kind_to_string(FailureKind kind) {
    switch (kind) {
    case FailureKind::TaskException: return "TaskException";
    case FailureKind::SubmitRejected: return "SubmitRejected";
    case FailureKind::TaskTimeout: return "TaskTimeout";
    case FailureKind::RealtimeDrop: return "RealtimeDrop";
    case FailureKind::GpuFailure: return "GpuFailure";
    case FailureKind::WaitTimeout: return "WaitTimeout";
    case FailureKind::TuningFallback: return "TuningFallback";
    default: return "Unknown";
    }
}

const char* task_lifecycle_to_string(TaskLifecycleState state) {
    switch (state) {
    case TaskLifecycleState::Pending: return "Pending";
    case TaskLifecycleState::Queued: return "Queued";
    case TaskLifecycleState::Running: return "Running";
    case TaskLifecycleState::Succeeded: return "Succeeded";
    case TaskLifecycleState::Failed: return "Failed";
    case TaskLifecycleState::TimedOut: return "TimedOut";
    case TaskLifecycleState::Rejected: return "Rejected";
    case TaskLifecycleState::Cancelled: return "Cancelled";
    case TaskLifecycleState::DependencyBlocked: return "DependencyBlocked";
    default: return "Unknown";
    }
}

const char* gpu_backend_to_string(gpu::GpuBackend backend) {
    switch (backend) {
    case gpu::GpuBackend::CUDA: return "CUDA";
    case gpu::GpuBackend::OPENCL: return "OPENCL";
    case gpu::GpuBackend::SYCL: return "SYCL";
    case gpu::GpuBackend::HIP: return "HIP";
    default: return "Unknown";
    }
}

const char* blocking_stop_reason_to_string(BlockingIoStopReason reason) {
    switch (reason) {
    case BlockingIoStopReason::None: return "None";
    case BlockingIoStopReason::Requested: return "Requested";
    case BlockingIoStopReason::WorkerReturned: return "WorkerReturned";
    case BlockingIoStopReason::WorkerException: return "WorkerException";
    case BlockingIoStopReason::StartFailed: return "StartFailed";
    default: return "Unknown";
    }
}

int64_t time_point_ns(std::chrono::steady_clock::time_point time_point) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        time_point.time_since_epoch()).count();
}

template <typename Output>
void write_bool(Output& output, bool value) {
    output << (value ? "true" : "false");
}

struct AllocationCounter {
    size_t count = 0;
};

template <typename T>
class CountingAllocator {
public:
    using value_type = T;

    CountingAllocator() = default;
    explicit CountingAllocator(AllocationCounter* counter) noexcept : counter_(counter) {}

    template <typename U>
    CountingAllocator(const CountingAllocator<U>& other) noexcept : counter_(other.counter()) {}

    T* allocate(size_t count) {
        if (counter_ != nullptr) {
            ++counter_->count;
        }
        return std::allocator<T>{}.allocate(count);
    }

    void deallocate(T* pointer, size_t count) noexcept {
        std::allocator<T>{}.deallocate(pointer, count);
    }

    AllocationCounter* counter() const noexcept { return counter_; }

    template <typename U>
    bool operator==(const CountingAllocator<U>& other) const noexcept {
        return counter_ == other.counter();
    }

    template <typename U>
    bool operator!=(const CountingAllocator<U>& other) const noexcept {
        return !(*this == other);
    }

private:
    template <typename>
    friend class CountingAllocator;
    AllocationCounter* counter_ = nullptr;
};

template <typename Output>
void write_executor_snapshot(Output& output, const ExecutorSnapshot& snapshot) {

    output << "executor_snapshot\n";
    output << "schema_version=" << snapshot.schema_version << '\n';
    output << "snapshot_sequence=" << snapshot.snapshot_sequence << '\n';
    output << "state_epoch=" << snapshot.state_epoch << '\n';
    output << "captured_at_steady_ns=" << time_point_ns(snapshot.captured_at) << '\n';
    output << "collection_duration_ns=" << snapshot.collection_duration.count() << '\n';
    output << "lifecycle=" << lifecycle_to_string(snapshot.lifecycle) << '\n';
    output << "partial=";
    write_bool(output, snapshot.partial);
    output << '\n';
    output << "consistency_note=" << snapshot.consistency_note << '\n';
    output << "aggregate.running_backend_count=" << snapshot.running_backend_count << '\n';
    output << "aggregate.stopping_backend_count=" << snapshot.stopping_backend_count << '\n';
    output << "aggregate.active_task_count=" << snapshot.active_task_count << '\n';
    output << "aggregate.queued_task_count=" << snapshot.queued_task_count << '\n';
    output << "aggregate.failed_task_count=" << snapshot.failed_task_count << '\n';
    output << "aggregate.dropped_work_count=" << snapshot.dropped_work_count << '\n';
    output << "cancellation.request_count=" << snapshot.cancellation.request_count << '\n';
    output << "cancellation.queued_cancelled_count="
           << snapshot.cancellation.queued_cancelled_count << '\n';
    output << "cancellation.running_request_count="
           << snapshot.cancellation.running_request_count << '\n';
    output << "cancellation.completed_after_request_count="
           << snapshot.cancellation.completed_after_request_count << '\n';
    output << "timers.pending_count=" << snapshot.timers.pending_count << '\n';
    output << "timers.executed_count=" << snapshot.timers.executed_count << '\n';
    output << "timers.cancelled_count=" << snapshot.timers.cancelled_count << '\n';
    output << "in_flight.count=" << snapshot.in_flight_count << '\n';
    output << "in_flight.oldest_age_ns=" << snapshot.oldest_in_flight_age.count() << '\n';
    output << "in_flight.dropped_count=" << snapshot.in_flight_dropped_count << '\n';
    output << "in_flight.incomplete=";
    write_bool(output, snapshot.in_flight_diagnostics_incomplete);
    output << '\n';
    for (const auto& [state, count] : snapshot.in_flight_state_counts) {
        output << "in_flight.state[" << task_lifecycle_to_string(state) << "]=" << count << '\n';
    }
    output << "in_flight.tasks.count=" << snapshot.in_flight_tasks.size() << '\n';
    for (size_t index = 0; index < snapshot.in_flight_tasks.size(); ++index) {
        const auto& task = snapshot.in_flight_tasks[index];
        output << "in_flight.tasks[" << index << "].id=" << task.task_id << '\n';
        output << "in_flight.tasks[" << index << "].type=" << task.task_type << '\n';
        output << "in_flight.tasks[" << index << "].executor_name=" << task.executor_name << '\n';
        output << "in_flight.tasks[" << index << "].state=" << task_lifecycle_to_string(task.state) << '\n';
        output << "in_flight.tasks[" << index << "].submitted_at_steady_ns="
               << time_point_ns(task.submitted_at) << '\n';
        output << "in_flight.tasks[" << index << "].state_changed_at_steady_ns="
               << time_point_ns(task.state_changed_at) << '\n';
    }
    output << "completion.name=" << snapshot.completion.executor_name << '\n';
    output << "completion.initialized=";
    write_bool(output, snapshot.completion.is_initialized);
    output << "\ncompletion.running=";
    write_bool(output, snapshot.completion.is_running);
    output << "\ncompletion.idle=";
    write_bool(output, snapshot.completion.is_idle);
    output << "\ncompletion.active_tasks=" << snapshot.completion.active_tasks << '\n';
    output << "completion.queued_tasks=" << snapshot.completion.queued_tasks << '\n';
    output << "completion.pending_tasks=" << snapshot.completion.pending_tasks << '\n';
    output << "completion.completed_tasks=" << snapshot.completion.completed_tasks << '\n';
    output << "completion.failed_tasks=" << snapshot.completion.failed_tasks << '\n';
    output << "async.name=" << snapshot.async.name << '\n';
    output << "async.running=";
    write_bool(output, snapshot.async.is_running);
    output << "\nasync.active_tasks=" << snapshot.async.active_tasks << '\n';
    output << "async.queue_size=" << snapshot.async.queue_size << '\n';
    output << "async.completed_tasks=" << snapshot.async.completed_tasks << '\n';
    output << "async.failed_tasks=" << snapshot.async.failed_tasks << '\n';
    output << "async.avg_task_time_ms=" << std::fixed << std::setprecision(3)
           << snapshot.async.avg_task_time_ms << '\n';

    output << "realtime.count=" << snapshot.realtime.size() << '\n';
    for (const auto& [name, status] : snapshot.realtime) {
        output << "realtime[" << name << "].running=";
        write_bool(output, status.is_running);
        output << "\nrealtime[" << name << "].cycle_period_ns=" << status.cycle_period_ns << '\n';
        output << "realtime[" << name << "].cycle_count=" << status.cycle_count << '\n';
        output << "realtime[" << name << "].cycle_timeout_count=" << status.cycle_timeout_count << '\n';
        output << "realtime[" << name << "].avg_cycle_time_ns=" << std::fixed
               << std::setprecision(3) << status.avg_cycle_time_ns << '\n';
        output << "realtime[" << name << "].max_cycle_time_ns=" << std::fixed
               << std::setprecision(3) << status.max_cycle_time_ns << '\n';
        output << "realtime[" << name << "].priority_applied=";
        write_bool(output, status.priority_applied);
        output << "\nrealtime[" << name << "].cpu_affinity_applied=";
        write_bool(output, status.cpu_affinity_applied);
        output << "\nrealtime[" << name << "].timer_slack_applied=";
        write_bool(output, status.timer_slack_applied);
        // CR-070: 此处缺 '\n' 曾把布尔值与下一个 key 拼成
        // `...applied=truerealtime[rt].dropped_...`，破坏 line-oriented 契约。
        output << "\nrealtime[" << name << "].dropped_task_count=" << status.dropped_task_count << '\n';
        output << "realtime[" << name << "].failed_pushes=" << status.failed_pushes << '\n';
        output << "realtime[" << name << "].peak_queue_size=" << status.peak_queue_size << '\n';
        output << "realtime[" << name << "].queue_capacity=" << status.queue_capacity << '\n';
        output << "realtime[" << name << "].rejected_not_running_count="
               << status.rejected_not_running_count << '\n';
        output << "realtime[" << name << "].rejected_empty_task_count="
               << status.rejected_empty_task_count << '\n';
        output << "realtime[" << name << "].pool_exhausted_count="
               << status.pool_exhausted_count << '\n';
        output << "realtime[" << name << "].queue_full_count=" << status.queue_full_count << '\n';
        output << "realtime[" << name << "].cycle_manager_error_count="
               << status.cycle_manager_error_count << '\n';
    }
    output << "blocking_io.count=" << snapshot.blocking_io.size() << '\n';
    for (const auto& [name, status] : snapshot.blocking_io) {
        output << "blocking_io[" << name << "].running=";
        write_bool(output, status.is_running);
        output << "\nblocking_io[" << name << "].stop_requested=";
        write_bool(output, status.stop_requested);
        output << "\nblocking_io[" << name << "].ready=";
        write_bool(output, status.ready);
        output << "\nblocking_io[" << name << "].cpu_affinity_applied=";
        write_bool(output, status.cpu_affinity_applied);
        output << "\nblocking_io[" << name << "].memory_locked=";
        write_bool(output, status.memory_locked);
        output << "\nblocking_io[" << name << "].stop_reason="
               << blocking_stop_reason_to_string(status.stop_reason) << '\n';
        output << "blocking_io[" << name << "].wakeup_count=" << status.wakeup_count << '\n';
        output << "blocking_io[" << name << "].last_error_message="
               << status.last_error_message << '\n';
    }
    output << "gpu.count=" << snapshot.gpu.size() << '\n';
    for (const auto& [name, status] : snapshot.gpu) {
        output << "gpu[" << name << "].running=";
        write_bool(output, status.is_running);
        output << "\ngpu[" << name << "].backend=" << gpu_backend_to_string(status.backend) << '\n';
        output << "gpu[" << name << "].device_id=" << status.device_id << '\n';
        output << "gpu[" << name << "].active_kernels=" << status.active_kernels << '\n';
        output << "gpu[" << name << "].completed_kernels=" << status.completed_kernels << '\n';
        output << "gpu[" << name << "].queue_size=" << status.queue_size << '\n';
        output << "gpu[" << name << "].queue_capacity=" << status.queue_capacity << '\n';
        output << "gpu[" << name << "].failed_kernels=" << status.failed_kernels << '\n';
        output << "gpu[" << name << "].memory_used_bytes=" << status.memory_used_bytes << '\n';
        output << "gpu[" << name << "].memory_total_bytes=" << status.memory_total_bytes << '\n';
        output << "gpu[" << name << "].memory_usage_percent=" << std::fixed
               << std::setprecision(3) << status.memory_usage_percent << '\n';
        output << "gpu[" << name << "].avg_kernel_time_ms=" << std::fixed
               << std::setprecision(3) << status.avg_kernel_time_ms << '\n';
        output << "gpu[" << name << "].last_error_message=" << status.last_error_message << '\n';
    }
    output << "failures.total_count=" << snapshot.failures.total_count << '\n';
    output << "failures.task_exception_count=" << snapshot.failures.task_exception_count << '\n';
    output << "failures.submit_rejected_count=" << snapshot.failures.submit_rejected_count << '\n';
    output << "failures.timeout_count=" << snapshot.failures.timeout_count << '\n';
    output << "failures.realtime_drop_count=" << snapshot.failures.realtime_drop_count << '\n';
    output << "failures.gpu_failure_count=" << snapshot.failures.gpu_failure_count << '\n';
    output << "failures.wait_timeout_count=" << snapshot.failures.wait_timeout_count << '\n';
    output << "failures.tuning_fallback_count=" << snapshot.failures.tuning_fallback_count << '\n';
    output << "recent_failures.count=" << snapshot.recent_failures.size() << '\n';
    for (size_t index = 0; index < snapshot.recent_failures.size(); ++index) {
        const auto& event = snapshot.recent_failures[index];
        output << "recent_failures[" << index << "].kind=" << failure_kind_to_string(event.kind) << '\n';
        output << "recent_failures[" << index << "].executor_name=" << event.executor_name << '\n';
        output << "recent_failures[" << index << "].task_id=" << event.task_id << '\n';
        output << "recent_failures[" << index << "].message=" << event.message << '\n';
        output << "recent_failures[" << index << "].timestamp_steady_ns="
               << time_point_ns(event.timestamp) << '\n';
    }
    output << "task_statistics.count=" << snapshot.task_statistics.size() << '\n';
    for (const auto& [task_type, statistics] : snapshot.task_statistics) {
        output << "task_statistics[" << task_type << "].total_count=" << statistics.total_count << '\n';
        output << "task_statistics[" << task_type << "].success_count=" << statistics.success_count << '\n';
        output << "task_statistics[" << task_type << "].fail_count=" << statistics.fail_count << '\n';
        output << "task_statistics[" << task_type << "].timeout_count=" << statistics.timeout_count << '\n';
        output << "task_statistics[" << task_type << "].total_execution_time_ns="
               << statistics.total_execution_time_ns << '\n';
        output << "task_statistics[" << task_type << "].max_execution_time_ns="
               << statistics.max_execution_time_ns << '\n';
        output << "task_statistics[" << task_type << "].min_execution_time_ns="
               << statistics.min_execution_time_ns << '\n';
    }
}

} // namespace

std::string format_executor_snapshot(const ExecutorSnapshot& snapshot) {
    std::ostringstream output;
    write_executor_snapshot(output, snapshot);
    return output.str();
}

ExecutorSnapshotTextExport format_executor_snapshot_with_metrics(
    const ExecutorSnapshot& snapshot) {
    AllocationCounter allocation_counter;
    using CountingStream = std::basic_ostringstream<
        char, std::char_traits<char>, CountingAllocator<char>>;
    const auto start = std::chrono::steady_clock::now();
    using CountingString = std::basic_string<
        char, std::char_traits<char>, CountingAllocator<char>>;
    // The (openmode, allocator) stream constructor is not implemented by libstdc++ 10.
    CountingString initial_text{CountingAllocator<char>{&allocation_counter}};
    CountingStream output(initial_text, std::ios_base::out);
    write_executor_snapshot(output, snapshot);
    const auto counted_text = output.str();

    ExecutorSnapshotTextExport result;
    result.text.assign(counted_text.data(), counted_text.size());
    // The exported std::string is larger than small-string storage for this fixed format.
    ++allocation_counter.count;
    result.metrics.formatting_allocation_count = allocation_counter.count;
    result.metrics.formatting_duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start);
    return result;
}

} // namespace kairo::monitor
