#pragma once

#include "types.hpp"
#include "gpu/gpu_scheduler.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace kairo {

/**
 * @brief 用户声明的任务执行意图。
 *
 * 路由器只能依据此声明和后端能力决定投递位置，不会推断 callable 的内容。
 */
enum class ExecutionIntent : uint8_t {
    Auto,
    GeneralCpu,
    CpuOrGpu,
    LowLatency,
    RealtimeQueue,
    BlockingWorker
};

/**
 * @brief 首选后端不可提交时的处理策略。
 */
enum class FallbackPolicy : uint8_t {
    NoFallback,
    AllowCpu,
    RequireRequestedBackend
};

/**
 * @brief 自动路由使用的后端类别。
 */
enum class ExecutionBackend : uint8_t {
    DefaultAsync,
    Gpu,
    LockFree,
    Realtime,
    BlockingIo
};

/**
 * @brief 路由决定的主要依据。
 */
enum class RoutingReason : uint8_t {
    DefaultPolicy,
    ExplicitIntent,
    PreferredExecutor,
    GpuHeuristic,
    AdaptiveHistory,
    BackendUnavailable,
    BackendNotRunning,
    CapacityPressure,
    FallbackPolicy,
    Rejected
};

/**
 * @brief 自动路由的不可变输入选项。
 *
 * `deadline` 仅供路由与诊断使用，不表示中断已开始执行的任务，也不改变
 * ThreadPoolConfig::task_timeout_ms 的软超时语义。
 */
struct TaskOptions {
    std::string name;
    TaskPriority priority = TaskPriority::NORMAL;
    ExecutionIntent intent = ExecutionIntent::Auto;
    std::optional<std::string> preferred_executor;
    FallbackPolicy fallback = FallbackPolicy::NoFallback;
    std::optional<std::chrono::steady_clock::time_point> deadline;
};

/** @brief A routable executor's advisory capability snapshot. */
struct ExecutorCapability {
    ExecutionBackend backend = ExecutionBackend::DefaultAsync;
    std::string name;
    bool registered = false;
    bool running = false;
    bool supports_future_submission = false;
    bool supports_bounded_dispatch = false;
    bool supports_gpu_kernel = false;
    size_t pending_work = 0;
    size_t capacity_hint = 0;
};

/** @brief Explanation of one automatic routing decision. */
struct RoutingDecision {
    std::string task_name;
    ExecutionIntent requested_intent = ExecutionIntent::Auto;
    ExecutionBackend selected_backend = ExecutionBackend::DefaultAsync;
    std::string selected_executor_name;
    RoutingReason reason = RoutingReason::DefaultPolicy;
    bool fell_back = false;
    std::string detail;
    std::chrono::steady_clock::time_point timestamp =
        std::chrono::steady_clock::now();
};

/** @brief Result of a bounded fire-and-forget automatic dispatch attempt. */
struct DispatchResult {
    bool accepted = false;
    ExecutionBackend backend = ExecutionBackend::LockFree;
    std::string executor_name;
    RoutingDecision decision;
    std::string message;
};

/**
 * @brief 将 callable 与自动路由选项组合的按值 builder。
 *
 * 此类型只表达任务意图；实际投递由后续 `Executor::submit_auto()` 重载完成。
 */
template <typename Function>
class TaskBuilder {
public:
    explicit TaskBuilder(Function function)
        : function_(std::move(function)) {}

    TaskBuilder& name(std::string value) {
        options_.name = std::move(value);
        return *this;
    }

    TaskBuilder& priority(TaskPriority value) noexcept {
        options_.priority = value;
        return *this;
    }

    TaskBuilder& intent(ExecutionIntent value) noexcept {
        options_.intent = value;
        return *this;
    }

    TaskBuilder& preferred_executor(std::string value) {
        options_.preferred_executor = std::move(value);
        return *this;
    }

    TaskBuilder& fallback(FallbackPolicy value) noexcept {
        options_.fallback = value;
        return *this;
    }

    TaskBuilder& deadline(std::chrono::steady_clock::time_point value) noexcept {
        options_.deadline = value;
        return *this;
    }

    const TaskOptions& options() const noexcept {
        return options_;
    }

    TaskOptions& options() noexcept {
        return options_;
    }

    const Function& function() const& noexcept {
        return function_;
    }

    Function& function() & noexcept {
        return function_;
    }

    Function&& function() && noexcept {
        return std::move(function_);
    }

private:
    Function function_;
    TaskOptions options_;
};

/**
 * @brief 创建可配置自动路由意图的 callable 包装。
 */
template <typename Function>
auto task(Function&& function) -> TaskBuilder<std::decay_t<Function>> {
    return TaskBuilder<std::decay_t<Function>>(std::forward<Function>(function));
}

/**
 * @brief CPU/GPU 自动路由任务的双路径 callable。
 *
 * CPU 与 GPU 路径彼此独立：CPU callable 不接收 stream，GPU callable 可接收
 * `void* stream` 或不接收参数。首版仅支持 `void` 返回，以避免混淆设备同步与
 * GPU callback 的返回值语义。
 */
template <typename CpuFunction, typename GpuFunction>
class CpuGpuTask {
public:
    CpuGpuTask(CpuFunction cpu, GpuFunction gpu)
        : cpu_(std::move(cpu)), gpu_(std::move(gpu)) {
        options_.intent = ExecutionIntent::CpuOrGpu;
    }

    CpuGpuTask& name(std::string value) {
        options_.name = std::move(value);
        return *this;
    }

    CpuGpuTask& priority(TaskPriority value) noexcept {
        options_.priority = value;
        gpu_config_.priority = static_cast<int>(value);
        return *this;
    }

    CpuGpuTask& preferred_executor(std::string value) {
        options_.preferred_executor = std::move(value);
        return *this;
    }

    CpuGpuTask& fallback(FallbackPolicy value) noexcept {
        options_.fallback = value;
        return *this;
    }

    CpuGpuTask& deadline(std::chrono::steady_clock::time_point value) noexcept {
        options_.deadline = value;
        return *this;
    }

    CpuGpuTask& data_size(size_t value) noexcept {
        characteristics_.data_size_bytes = value;
        return *this;
    }

    CpuGpuTask& compute_intensity(float value) noexcept {
        characteristics_.compute_intensity = value;
        return *this;
    }

    CpuGpuTask& prefer_gpu(bool value = true) noexcept {
        characteristics_.prefer_gpu = value;
        return *this;
    }

    CpuGpuTask& gpu_config(gpu::GpuTaskConfig value) noexcept {
        gpu_config_ = std::move(value);
        return *this;
    }

    const TaskOptions& options() const noexcept { return options_; }
    const gpu::TaskCharacteristics& characteristics() const noexcept { return characteristics_; }
    const gpu::GpuTaskConfig& gpu_config() const noexcept { return gpu_config_; }

    CpuFunction&& take_cpu() noexcept { return std::move(cpu_); }
    GpuFunction&& take_gpu() noexcept { return std::move(gpu_); }

private:
    CpuFunction cpu_;
    GpuFunction gpu_;
    TaskOptions options_;
    gpu::TaskCharacteristics characteristics_;
    gpu::GpuTaskConfig gpu_config_;
};

template <typename CpuFunction, typename GpuFunction>
auto cpu_gpu_task(CpuFunction&& cpu, GpuFunction&& gpu)
    -> CpuGpuTask<std::decay_t<CpuFunction>, std::decay_t<GpuFunction>> {
    using CpuGpuTaskType = CpuGpuTask<std::decay_t<CpuFunction>, std::decay_t<GpuFunction>>;
    return CpuGpuTaskType(std::forward<CpuFunction>(cpu), std::forward<GpuFunction>(gpu));
}

}  // namespace kairo
