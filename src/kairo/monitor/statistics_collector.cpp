#include "kairo/monitor/statistics_collector.hpp"

namespace kairo {
namespace monitor {

TaskMonitor& StatisticsCollector::get_task_monitor() {
    return task_monitor_;
}

TaskStatistics StatisticsCollector::get_task_statistics(
    const std::string& task_type) const {
    return task_monitor_.get_statistics(task_type);
}

std::map<std::string, TaskStatistics>
StatisticsCollector::get_all_task_statistics() const {
    return task_monitor_.get_all_statistics();
}

InFlightTaskDiagnostics StatisticsCollector::get_in_flight_task_diagnostics() const {
    return task_monitor_.get_in_flight_diagnostics();
}

void StatisticsCollector::set_gpu_status_provider(
    std::function<std::map<std::string, gpu::GpuExecutorStatus>()> provider) {
    gpu_status_provider_ = std::move(provider);
}

gpu::GpuExecutorStatus StatisticsCollector::get_gpu_executor_status(
    const std::string& name) const {
    if (!gpu_status_provider_) {
        gpu::GpuExecutorStatus status;
        status.name = name;
        return status;
    }
    auto all = gpu_status_provider_();
    auto it = all.find(name);
    if (it == all.end()) {
        gpu::GpuExecutorStatus status;
        status.name = name;
        return status;
    }
    return it->second;
}

std::map<std::string, gpu::GpuExecutorStatus>
StatisticsCollector::get_all_gpu_executor_statuses() const {
    if (!gpu_status_provider_) {
        return {};
    }
    return gpu_status_provider_();
}

}  // namespace monitor
}  // namespace kairo
