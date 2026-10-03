#pragma once

#include "kairo/types.hpp"

#include <atomic>
#include <functional>

namespace kairo {
class ExecutorManager;

namespace monitor {

/**
 * @brief 汇总 Executor 各只读 provider 的低频诊断快照采集器。
 *
 * collect() 是 best-effort 操作：单个 provider 异常只会使快照标记为
 * partial，不会向业务调用方传播诊断异常。Manager epoch 在采集前后
 * 校验，发生结构性变化时有限重试，仍不稳定则保留 partial 标记。
 */
class ExecutorMonitor {
public:
    using CompletionProvider = std::function<CompletionStatus()>;
    using FailureStatusProvider = std::function<ExecutorFailureStatus()>;
    using RecentFailuresProvider = std::function<std::vector<ExecutorFailureEvent>()>;
    using TaskStatisticsProvider = std::function<std::map<std::string, TaskStatistics>()>;
    using InFlightTaskDiagnosticsProvider = std::function<InFlightTaskDiagnostics()>;
    using CancellationStatusProvider = std::function<CancellationStatus()>;
    using TimerStatusProvider = std::function<TimerStatusSummary()>;

    ExecutorMonitor(const ExecutorManager& manager,
                    const std::atomic<ExecutorLifecycleState>& lifecycle,
                    CompletionProvider completion_provider,
                    FailureStatusProvider failure_status_provider,
                    RecentFailuresProvider recent_failures_provider,
                    TaskStatisticsProvider task_statistics_provider,
                    InFlightTaskDiagnosticsProvider in_flight_task_diagnostics_provider = {},
                    CancellationStatusProvider cancellation_status_provider = {},
                    TimerStatusProvider timer_status_provider = {});

    ExecutorSnapshot collect() const;

private:
    const ExecutorManager& manager_;
    const std::atomic<ExecutorLifecycleState>& lifecycle_;
    CompletionProvider completion_provider_;
    FailureStatusProvider failure_status_provider_;
    RecentFailuresProvider recent_failures_provider_;
    TaskStatisticsProvider task_statistics_provider_;
    InFlightTaskDiagnosticsProvider in_flight_task_diagnostics_provider_;
    CancellationStatusProvider cancellation_status_provider_;
    TimerStatusProvider timer_status_provider_;
    mutable std::atomic<uint64_t> next_sequence_{0};
};

} // namespace monitor
} // namespace kairo
