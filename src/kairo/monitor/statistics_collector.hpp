#pragma once

#include "kairo/types.hpp"
#include "kairo/monitor/task_monitor.hpp"
#include <string>
#include <map>
#include <functional>

namespace kairo {
namespace monitor {

/**
 * @brief 统计收集器
 *
 * 聚合与查询门面，持有 TaskMonitor，对外提供 get_task_statistics /
 * get_all_task_statistics。供 ExecutorManager / Executor 暴露监控 API。
 */
class StatisticsCollector {
public:
    StatisticsCollector() = default;
    ~StatisticsCollector() = default;

    StatisticsCollector(const StatisticsCollector&) = delete;
    StatisticsCollector& operator=(const StatisticsCollector&) = delete;
    StatisticsCollector(StatisticsCollector&&) = delete;
    StatisticsCollector& operator=(StatisticsCollector&&) = delete;

    /**
     * @brief 获取 TaskMonitor，供 ThreadPool / 实时扩展写入
     */
    TaskMonitor& get_task_monitor();

    /**
     * @brief 按 task_type 获取聚合统计
     */
    TaskStatistics get_task_statistics(const std::string& task_type) const;

    /**
     * @brief 获取全部 task_type 的聚合统计
     */
    std::map<std::string, TaskStatistics> get_all_task_statistics() const;

    InFlightTaskDiagnostics get_in_flight_task_diagnostics() const;

    /**
     * @brief 设置 GPU 状态提供者（由 ExecutorManager 注入，避免循环依赖）
     */
    void set_gpu_status_provider(
        std::function<std::map<std::string, gpu::GpuExecutorStatus>()> provider);

    /**
     * @brief 按名称获取 GPU 执行器状态
     */
    gpu::GpuExecutorStatus get_gpu_executor_status(const std::string& name) const;

    /**
     * @brief 获取所有 GPU 执行器状态
     */
    std::map<std::string, gpu::GpuExecutorStatus> get_all_gpu_executor_statuses() const;

private:
    TaskMonitor task_monitor_;
    std::function<std::map<std::string, gpu::GpuExecutorStatus>()> gpu_status_provider_;
};

}  // namespace monitor
}  // namespace kairo
