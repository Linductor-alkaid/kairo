#pragma once

#include "task_options.hpp"
#include "task_router.hpp"

#include <string>
#include <vector>

namespace kairo {

/**
 * @brief 调度器的任务终态反馈（0.6.1 measurement contract）。
 *
 * 0.6.1 只建立 measurement 与 feedback contract，不实现 adaptive
 * scheduling：DefaultScheduler 不消费反馈（wants_feedback() == false），
 * 也不根据反馈调整策略。
 *
 * 覆盖范围：实际开始执行的任务（默认异步池路径）。提交前被拒绝 /
 * admission 拒绝 / 排队期超时的提交不会产生反馈——这些终态由
 * RoutingDecision、SchedulingMetrics 与 failure 体系观测。
 *
 * 线程约定：on_task_completed() 在执行任务的 worker 线程上同步调用；
 * 实现必须快速返回且不得抛出异常（抛出会被隔离，但会丢失该条反馈）。
 */
struct SchedulingFeedback {
    std::string task_id;
    QosClass qos = QosClass::Standard;
    bool success = false;

    // ---- 0.6.1 执行期测量 ----
    ExecutionBackend backend = ExecutionBackend::DefaultAsync;  // 最终使用的后端
    std::string executor_name;                 // 最终使用的执行器名（"default"）
    int64_t queue_wait_ns = 0;                 // 提交 → 开始执行（steady 时钟）
    int64_t execution_duration_ns = 0;         // 开始执行 → 终态（steady 时钟）
    bool had_deadline = false;                 // 声明过 deadline（区分"未声明"
                                                // 与"声明且未错过"）
    bool deadline_missed = false;              // 开始执行时已错过（仍执行）
    FailureKind failure_kind = FailureKind::None;  // success == false 时的失败
                                                    // 分类；success 时为 None
};

/**
 * @brief Scheduling Runtime 的调度器抽象（0.6.0）。
 *
 * 调度器只产出决策（准入 + 投递位置），不执行投递；五种后端提交协议
 * （future / DispatchResult / WorkerHandle / push_task / push_realtime_task）
 * 仍由 Executor 适配层执行。这样保持既有提交协议边界（见
 * docs/design/unified_facade_and_auto_routing.md），同时把调度策略
 * 从 facade 内联代码中解耦为可注入组件。
 *
 * 线程安全约定：route() 会在多提交线程并发调用，实现必须自身线程安全；
 * set_scheduler() 须在首次提交前完成（运行中替换需调用方自行同步）。
 */
class IScheduler {
public:
    virtual ~IScheduler() = default;

    /**
     * @brief 为一次提交产出路由决策。
     *
     * request.cpu_gpu_task == true 时 capabilities 为全部后端能力快照
     * （惰性采集，CR-106）；否则为空（纯策略判定，不读能力表）。
     */
    virtual RoutingDecision route(const TaskRouter::Request& request,
                                  const std::vector<ExecutorCapability>& capabilities) = 0;

    /** @brief 任务终态反馈。默认实现无操作。 */
    virtual void on_task_completed(const SchedulingFeedback& feedback) {
        (void)feedback;
    }

    /**
     * @brief 是否需要执行期反馈（0.6.1 热路径开关）。
     *
     * 返回 false（默认）时，submit_auto 不为任务附加测量包装，反馈通道
     * 零开销；返回 true 时每个实际执行的任务会以 2 次 steady 时钟采样 +
     * 一次 on_task_completed() 调用为代价产生 SchedulingFeedback。
     * Executor 在 set_scheduler() 时缓存该值；须在首次提交前设置。
     */
    virtual bool wants_feedback() const noexcept { return false; }
};

/**
 * @brief 默认调度器：意图路由 + 0.6.0 调度模型约束。
 *
 * 在 TaskRouter 意图路由之上叠加 SchedulingSpec 约束检查：
 * - deadline：提交时已过期 → 拒绝（RoutingReason::Rejected）；
 * - resources：声明的 GPU 设备与目标执行器不符 → 拒绝
 *   （BackendUnavailable）；声明内存超过设备可用量 → 拒绝
 *   （CapacityPressure；内存总量未知时跳过检查）；
 * - affinity：请求 CPU 集合与目标后端绑核不相交时不拒绝，
 *   但在 decision.detail 中给出 AffinityMismatch 警告（advisory）。
 */
class DefaultScheduler final : public IScheduler {
public:
    RoutingDecision route(const TaskRouter::Request& request,
                          const std::vector<ExecutorCapability>& capabilities) override;

private:
    TaskRouter router_;
};

}  // namespace kairo
