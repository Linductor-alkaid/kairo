#pragma once

#include "task_options.hpp"
#include "task_router.hpp"

#include <string>
#include <vector>

namespace kairo {

/**
 * @brief 调度器的任务终态反馈（低频，诊断/学习用途）。
 */
struct SchedulingFeedback {
    std::string task_id;
    QosClass qos = QosClass::Standard;
    bool success = false;
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
