#pragma once

#include "types.hpp"
#include "scheduling.hpp"
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
 *
 * 0.6.1 语义：`RoutingDecision::status` 是结果的权威判据；reason 表达
 * 导致该结果的主要原因。新增的 `DeadlineExpired` / `AffinityMismatch`
 * 使拒绝与降级原因可被程序化判别，不再依赖解析 detail 字符串。
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
    Rejected,
    // ---- 0.6.1 Scheduling Runtime 结构化诊断 ----
    DeadlineExpired,    // 提交时点 deadline 已过 → status == Rejected
    AffinityMismatch    // 请求核集合与目标后端绑核不相交（advisory）
                        // → status == AcceptedDegraded 且 diagnostics 含
                        //    RoutingDiagnostics::AffinityMismatch
};

/**
 * @brief 路由决策的结果状态（0.6.1，权威判据）。
 *
 * 消费方应依据 status 而非枚举 reason 组合判断任务是否被接受；
 * reason 只解释"为什么"。历史上需要枚举 reason 的判断
 * （Rejected / BackendUnavailable / BackendNotRunning / CapacityPressure）
 * 统一收敛为 `status == Rejected`。
 */
enum class RoutingStatus : uint8_t {
    Accepted,          // 按决策投递，无降级诊断
    AcceptedDegraded,  // 接受但携带结构化降级诊断（AffinityMismatch /
                       // FallbackPolicy 回退）
    Rejected           // 拒绝；任务不会以该决策投递（future 以异常就绪 /
                       // DispatchResult::accepted == false）
};

/**
 * @brief 路由决策的结构化诊断位（bitmask，可叠加）。
 *
 * `reason` 只能表达单一主要原因；diagnostics 保留并发存在的次要
 * 诊断。0.6.1 定义两个位。
 */
namespace RoutingDiagnostics {
constexpr uint32_t None = 0;
/** 请求核集合与目标后端 bound_cpus 不相交（advisory，任务仍被接受）。 */
constexpr uint32_t AffinityMismatch = 1u << 0;
/** 拒绝由声明的 ResourceRequirements 触发（设备不符 / 内存超量）。 */
constexpr uint32_t ResourceInfeasible = 1u << 1;
}  // namespace RoutingDiagnostics

/** @brief RoutingStatus 的稳定名称（诊断与测试输出）。 */
inline const char* routing_status_to_string(RoutingStatus status) noexcept {
    switch (status) {
    case RoutingStatus::Accepted:
        return "Accepted";
    case RoutingStatus::AcceptedDegraded:
        return "AcceptedDegraded";
    case RoutingStatus::Rejected:
    default:
        return "Rejected";
    }
}

/** @brief RoutingReason 的稳定名称（诊断与测试输出）。 */
inline const char* routing_reason_to_string(RoutingReason reason) noexcept {
    switch (reason) {
    case RoutingReason::DefaultPolicy:
        return "DefaultPolicy";
    case RoutingReason::ExplicitIntent:
        return "ExplicitIntent";
    case RoutingReason::PreferredExecutor:
        return "PreferredExecutor";
    case RoutingReason::GpuHeuristic:
        return "GpuHeuristic";
    case RoutingReason::AdaptiveHistory:
        return "AdaptiveHistory";
    case RoutingReason::BackendUnavailable:
        return "BackendUnavailable";
    case RoutingReason::BackendNotRunning:
        return "BackendNotRunning";
    case RoutingReason::CapacityPressure:
        return "CapacityPressure";
    case RoutingReason::FallbackPolicy:
        return "FallbackPolicy";
    case RoutingReason::DeadlineExpired:
        return "DeadlineExpired";
    case RoutingReason::AffinityMismatch:
        return "AffinityMismatch";
    case RoutingReason::Rejected:
    default:
        return "Rejected";
    }
}

/**
 * @brief 自动路由的不可变输入选项。
 *
 * `deadline` 是真实调度输入：同优先级内按 EDF 排序，执行时已错过将记录
 * DeadlineMissed 诊断（不表示中断已开始执行的任务，也不改变
 * ThreadPoolConfig::task_timeout_ms 的软超时语义）。
 * `qos` 未显式设置 priority 时决定默认排队优先级；`affinity`/`resources`
 * 是声明式约束，由调度器在路由/准入时与后端能力核对。
 */
struct TaskOptions {
    std::string name;
    TaskPriority priority = TaskPriority::NORMAL;
    bool priority_set = false;  // 显式设置过 priority 时 QoS 不再映射默认值
    ExecutionIntent intent = ExecutionIntent::Auto;
    std::optional<std::string> preferred_executor;
    FallbackPolicy fallback = FallbackPolicy::NoFallback;
    std::optional<std::chrono::steady_clock::time_point> deadline;
    QosClass qos = QosClass::Standard;
    AffinityHint affinity;
    ResourceRequirements resources;
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

    // ---- 0.6.0 Scheduling Runtime：调度模型检查用的能力维度 ----
    std::vector<int> bound_cpus;        // worker 绑核集合；空 = 未知/未约束
    int gpu_device = -1;                // GPU 设备 ID；非 GPU 后端为 -1
    size_t gpu_memory_total_bytes = 0;  // GPU 总内存；0 = 未知
    size_t gpu_memory_free_bytes = 0;   // GPU 当前可用内存；0 = 未知
};

/** @brief Explanation of one automatic routing decision. */
struct RoutingDecision {
    std::string task_name;
    ExecutionIntent requested_intent = ExecutionIntent::Auto;
    ExecutionBackend selected_backend = ExecutionBackend::DefaultAsync;
    std::string selected_executor_name;
    RoutingReason reason = RoutingReason::DefaultPolicy;
    // 0.6.1：结果的权威判据（accepted / degraded / rejected）。所有产出
    // RoutingDecision 的代码路径都必须让它与实际投递结果一致。
    RoutingStatus status = RoutingStatus::Accepted;
    // 0.6.1：结构化次要诊断（RoutingDiagnostics 位组合）。
    uint32_t diagnostics = RoutingDiagnostics::None;
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
        options_.priority_set = true;
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

    TaskBuilder& qos(QosClass value) noexcept {
        options_.qos = value;
        return *this;
    }

    TaskBuilder& affinity(AffinityHint value) noexcept {
        options_.affinity = std::move(value);
        return *this;
    }

    TaskBuilder& resources(ResourceRequirements value) noexcept {
        options_.resources = value;
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

/** @brief QoS 类别对应的默认排队优先级（用户未显式设 priority 时生效）。 */
inline TaskPriority default_priority_for_qos(QosClass qos) noexcept {
    switch (qos) {
    case QosClass::BestEffort:
        return TaskPriority::LOW;
    case QosClass::Interactive:
        return TaskPriority::HIGH;
    case QosClass::Critical:
        return TaskPriority::CRITICAL;
    case QosClass::Standard:
    default:
        return TaskPriority::NORMAL;
    }
}

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
        options_.priority_set = true;
        gpu_config_.priority = static_cast<int>(value);
        return *this;
    }

    CpuGpuTask& qos(QosClass value) noexcept {
        options_.qos = value;
        if (!options_.priority_set) {
            gpu_config_.priority = static_cast<int>(default_priority_for_qos(value));
        }
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

    CpuGpuTask& affinity(AffinityHint value) noexcept {
        options_.affinity = std::move(value);
        return *this;
    }

    CpuGpuTask& resources(ResourceRequirements value) noexcept {
        options_.resources = value;
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
