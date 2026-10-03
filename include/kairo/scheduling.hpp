#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

namespace kairo {

/**
 * @brief 任务服务质量类别（0.6.0 Scheduling Runtime）。
 *
 * QoS 是任务向调度器声明的服务期望，决定默认池内的排队优先级映射
 * （default_priority_for_qos()，见 task_options.hpp）。它不改变执行
 * 模型——硬实时周期执行仍须通过 RealtimeQueue 意图与专用实时线程表达
 * （见 ExecutionIntent）。
 */
enum class QosClass : uint8_t {
    BestEffort,   // 后台/可延迟工作；映射 LOW。严格优先级下可能被持续
                  // 的高优先级负载饿死（CR-024 契约），适合可丢弃负载
    Standard,     // 默认；映射 NORMAL
    Interactive,  // 延迟敏感；映射 HIGH
    HardRealtime  // 映射 CRITICAL 排队优先级；但池内任务不会被抢占，
                  // 需要周期确定性时必须改用 RealtimeQueue 意图
};

/** @brief QoS 类别的稳定名称（诊断与快照输出）。 */
const char* qos_class_to_string(QosClass qos) noexcept;

/**
 * @brief per-task CPU 亲和性提示。
 *
 * 这是任务级的 advisory 约束：调度器在路由时检查请求的 CPU 集合与
 * 目标后端配置的 worker 绑核是否相交，并在 RoutingDecision 中给出
 * 结论。它不会为单个任务重新绑定 OS 线程亲和性——线程绑核是后端
 * 启动期属性（ThreadPoolConfig::cpu_affinity 等）。
 */
struct AffinityHint {
    std::vector<int> cpus;     // 空 = 无约束
    bool exclusive = false;    // 请求独占核（advisory；诊断可见）
};

/**
 * @brief 任务资源需求声明。
 *
 * 声明式描述任务对设备资源的要求，供调度器在路由/准入时与后端
 * 能力快照核对。不满足时任务被拒绝并携带原因，不会隐式降级。
 */
struct ResourceRequirements {
    size_t memory_bytes = 0;   // 0 = 不声明；用于 GPU 内存可行性检查
    int gpu_device = -1;       // -1 = 不指定；>=0 时要求 GPU 执行器使用该设备
};

/**
 * @brief 任务的完整调度规格。
 *
 * TaskOptions 持有该规格；调度器（IScheduler 实现）据此产出路由与
 * 准入决策。deadline 是真实调度输入：同优先级内按 EDF 排序，执行时
 * 已错过将记录 DeadlineMissed 诊断（不中断已开始执行的任务）。
 */
struct SchedulingSpec {
    std::optional<std::chrono::steady_clock::time_point> deadline;
    QosClass qos = QosClass::Standard;
    AffinityHint affinity;
    ResourceRequirements resources;
};

/**
 * @brief 随提交协议进入执行器的调度元数据（EDF 排序 + QoS 观测）。
 *
 * 只含默认池排序与统计所需的最小负载；affinity/resource 消费在
 * 路由层（DefaultScheduler），不进入执行器内部任务结构。
 */
struct TaskSchedulingMeta {
    int64_t deadline_ns = 0;  // steady 纳秒；0 = 无 deadline
    QosClass qos = QosClass::Standard;
};

}  // namespace kairo
