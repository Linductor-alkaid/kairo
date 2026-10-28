#pragma once

#include "types.hpp"
#include "scheduling.hpp"
#include "task_result.hpp"
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
    AffinityMismatch,   // 请求核集合与目标后端绑核不相交（advisory）
                        // → status == AcceptedDegraded 且 diagnostics 含
                        //    RoutingDiagnostics::AffinityMismatch
    // ---- 0.7.0 AdaptiveScheduler（M3）----
    LoadShedding        // QoS 感知降载：观测到更高级别 QoS 的 queue wait
                        // p99 持续超过目标 → 对严格更低 QoS 的提交
                        // 提前结构化拒绝 → status == Rejected
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
/** 0.7.0 M3：决策由执行历史反馈驱动（reason == AdaptiveHistory 时
 *  恒置位；detail 携带两侧 EWMA 数值，可复原决策依据）。 */
constexpr uint32_t AdaptiveHistory = 1u << 2;
/** 0.7.0 M3：拒绝由 QoS 降载状态机触发（reason == LoadShedding）。 */
constexpr uint32_t LoadShedding = 1u << 3;
/** 0.7.1 §3.0.1：本次路由消费的能力快照来自 TTL 缓存而非实时采集
 *  （配对的 RoutingDecision::capability_from_cache 布尔字段；位与字段
 *  同置，诊断位便于掩码过滤）。 */
constexpr uint32_t CapabilityCacheHit = 1u << 7;
/** 0.7.1 §3.0.2：决策携带任务特征自动推断的建议（建议优先于切换，
 *  路由不被推断改变；inferred_reason 说明推断依据）。 */
constexpr uint32_t AutoInferredIntent = 1u << 5;
/** 0.7.1 §3.0.3：有界 dispatch 的执行器由调度器按负载自动选择
 *  （用户未显式指定 preferred_executor 时；detail 携带各候选负载）。 */
constexpr uint32_t AutoExecutorSelection = 1u << 6;
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
    case RoutingReason::LoadShedding:
        return "LoadShedding";
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
    // 0.7.1 §3.2：开始执行时已错过 deadline 的处置策略；仅与 deadline
    // 同时声明时生效（无 deadline 时忽略）。见 DeadlinePolicy。
    DeadlinePolicy deadline_policy = DeadlinePolicy::RunAnyway;
    QosClass qos = QosClass::Standard;
    AffinityHint affinity;
    ResourceRequirements resources;
    // 分主体在途份额键（分桶 admission，issue #222）。仅在接纳点
    // （try_admit_submission）消费，不进入内部 Task 结构（与
    // affinity/resources 同层）。空 = 不分桶，仅全局 max_in_flight_tasks
    // 约束。一期仅 submit_auto(TaskBuilder) 与
    // submit_after*_with_handle(TaskBuilder) 携带该键进入接纳点。
    std::string subject;
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
    // 0.7.1 §3.0.1：路由消费的能力快照来自 TTL 缓存（与
    // RoutingDiagnostics::CapabilityCacheHit 同置）。快照内容弱一致：
    // 相对实时采集至多滞后 capability_cache_ttl（默认 100ms），后端
    // 注册/启停由 manager state_epoch 主动失效。
    bool capability_from_cache = false;
    // 0.7.1 §3.0.2：自动推断依据（与 RoutingDiagnostics::AutoInferredIntent
    // 同置；如 "blocking_io_pattern" / "low_latency_pattern" /
    // "periodic_pattern"）。空 = 无推断。
    std::string inferred_reason;
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
 * @brief 带返回值的 CPU/GPU 自动任务执行报告（0.7.3 §5.1）。
 *
 * result 承载任务值（异常经 get() 重抛）；decision 为实际选择路径
 * （回退时记录回退后的最终路径，decision 恒与实际执行一致）。拒绝时
 * result 以 runtime_error 异常就绪、decision 如实反映拒绝原因——
 * 不抛出，报告即终态。
 */
template<typename R>
struct ExecutionReport {
    TaskResult<R> result;
    RoutingDecision decision;
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

    /** 分主体在途份额键（分桶 admission，issue #222）；空 = 不分桶。 */
    TaskBuilder& subject(std::string value) {
        options_.subject = std::move(value);
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

    TaskBuilder& deadline_policy(DeadlinePolicy value) noexcept {
        options_.deadline_policy = value;
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
 * @brief 分主体在途份额的分桶键标签（`submit_auto` 裸形态带键入口）。
 *
 * 用法：`ex.submit_auto(kairo::subject("tenant-A"), [&]{ ... })`——经
 * `TaskBuilder::subject()` 同一接纳点消费，语义与带键 TaskBuilder 形态
 * 一致（docs/design/subject_bucket_admission.md 修订 R-1）。
 *
 * 构造函数**刻意 explicit**：若允许 `const char*` 隐式转换，
 * `submit_auto("key", fn)` 会因泛型重载 `submit_auto(F&&, Args&&...)`
 * 对首参的精确匹配而在重载决议中胜出——键被当成 callable 静默错绑。
 * explicit 使该误用直接编译失败，强制经 `subject()` 工厂表达。
 */
struct SubjectKey {
    std::string key;

    explicit SubjectKey(std::string k) : key(std::move(k)) {}
};

/** @brief SubjectKey 工厂：`submit_auto(kairo::subject("key"), fn)`。 */
inline SubjectKey subject(std::string key) {
    return SubjectKey(std::move(key));
}

/**
 * @brief CPU/GPU 自动路由任务的双路径 callable。
 *
 * CPU 与 GPU 路径彼此独立：CPU callable 不接收 stream，GPU callable 可接收
 * `void* stream` 或不接收参数。`R`（0.7.3 §5.1）为返回值类型，默认 void
 * （既有两参拼写与语义逐位不变）；R 非 void 时 CPU callable 需返回可
 * 转换到 R 的值，GPU 路径的取值由显式 `ResultAdapter<R>` 提供（见
 * result_adapter() 与 docs/design/scheduling_runtime.md §17.1）。
 */
template <typename CpuFunction, typename GpuFunction, typename R = void>
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

    /**
     * @brief R 非 void 时 GPU 路径的显式取值适配器（0.7.3 §5.1）。
     *
     * gpu_extract 在 GPU worker 线程、GPU callable 返回之后调用（async
     * 配置 = launch 完成点，sync = 设备同步完成点）；设备搬运与同步责任
     * 在用户侧。路由选中 GPU 且未提供适配器时提交被结构化拒绝（不静默
     * 降级 void）。
     */
    CpuGpuTask& result_adapter(ResultAdapter<R> adapter) {
        result_adapter_ = std::move(adapter);
        return *this;
    }

    const TaskOptions& options() const noexcept { return options_; }
    const gpu::TaskCharacteristics& characteristics() const noexcept { return characteristics_; }
    const gpu::GpuTaskConfig& gpu_config() const noexcept { return gpu_config_; }

    /** @brief 显式取值适配器；nullopt = 未提供。 */
    const std::optional<ResultAdapter<R>>& result_adapter() const noexcept {
        return result_adapter_;
    }

    /** @brief 可变访问（facade 提交时 move 出适配器）。 */
    std::optional<ResultAdapter<R>>& result_adapter() noexcept {
        return result_adapter_;
    }

    CpuFunction&& take_cpu() noexcept { return std::move(cpu_); }
    GpuFunction&& take_gpu() noexcept { return std::move(gpu_); }

private:
    CpuFunction cpu_;
    GpuFunction gpu_;
    TaskOptions options_;
    gpu::TaskCharacteristics characteristics_;
    gpu::GpuTaskConfig gpu_config_;
    std::optional<ResultAdapter<R>> result_adapter_;
};

template <typename R = void, typename CpuFunction, typename GpuFunction>
auto cpu_gpu_task(CpuFunction&& cpu, GpuFunction&& gpu)
    -> CpuGpuTask<std::decay_t<CpuFunction>, std::decay_t<GpuFunction>, R> {
    using CpuGpuTaskType =
        CpuGpuTask<std::decay_t<CpuFunction>, std::decay_t<GpuFunction>, R>;
    return CpuGpuTaskType(std::forward<CpuFunction>(cpu), std::forward<GpuFunction>(gpu));
}

}  // namespace kairo
