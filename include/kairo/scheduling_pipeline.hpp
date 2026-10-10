#pragma once

#include "task_options.hpp"
#include "task_router.hpp"

#include <new>
#include <optional>
#include <tuple>
#include <utility>

namespace kairo::detail {

// 调度 pipeline 各阶段共享的能力快照查找辅助（与 0.6.1 语义冻结）。
// find_capability / gpu_submittable 服务于意图路由决策树与
// dispatch_auto 路径；find_gpu_capability 服务于资源约束过滤（注意
// 差异：无偏好时取首个 running 的 GPU，不要求唯一，也不要求
// registered/supports_gpu_kernel）。

inline const ExecutorCapability* find_capability(
    const std::vector<ExecutorCapability>& capabilities,
    ExecutionBackend backend,
    const std::string& name = {}) {
    const auto iterator = std::find_if(capabilities.begin(), capabilities.end(),
        [&](const ExecutorCapability& capability) {
            return capability.backend == backend &&
                   (name.empty() || capability.name == name);
        });
    return iterator == capabilities.end() ? nullptr : &*iterator;
}

inline bool gpu_submittable(const ExecutorCapability* capability) {
    return capability && capability->registered && capability->running &&
           capability->supports_gpu_kernel &&
           (capability->capacity_hint == 0 ||
            capability->pending_work < capability->capacity_hint);
}

inline const ExecutorCapability* find_gpu_capability(
    const std::vector<ExecutorCapability>& capabilities,
    const std::optional<std::string>& preferred) {
    if (preferred) {
        for (const auto& capability : capabilities) {
            if (capability.backend == ExecutionBackend::Gpu &&
                capability.name == *preferred) {
                return &capability;
            }
        }
        return nullptr;
    }
    for (const auto& capability : capabilities) {
        if (capability.backend == ExecutionBackend::Gpu && capability.running) {
            return &capability;
        }
    }
    return nullptr;
}

}  // namespace kairo::detail

namespace kairo::scheduling {

// ============================================================================
// M1 pipeline 化的四个阶段：约束过滤 → 候选生成 → 评分/排序 → 选择。
//
// 阶段组件是公开的可组合 API；热路径实现统一放在
// src/kairo/scheduling_pipeline.cpp（SchedulingPipeline<IdentityScoring>
// 的显式实例化）。
//
// 两条实测得出的热路径设计约束（roadmap §2.2 验收线：route 纯策略
// 路径 ≤ 0.6.1 + 5%，基线 ~36ns，即只有 ~1.8ns 预算）：
//
// 1. 候选生成直写最终决策存储（primary），Plan 只携带元数据：0.6.1
//    决策树至多产生一个候选，任何"候选列表 → 选择时二次物化"的中间
//    结构都会引入一次决策搬运 + 拆卸（实测 +10ns 级）。多候选排序
//    （后续 AdaptiveScoring）通过扩展 CandidatePlan 接入。
// 2. 阶段实现集中在一个编译单元并以 always_inline 折叠：未折叠的
//    阶段边界实测各值 ~10ns（栈帧 + 栈保护 + sret）。
//
// 因此：直接组合各阶段组件适用于非热路径；热路径请经
// SchedulingPipeline::route()（或 DefaultScheduler）。
// ============================================================================

// 热路径折叠策略：阶段函数体量中等，编译器在大函数增长限制下会放弃
// 常规内联，因此热路径经由的内部实现函数（scheduling_pipeline.cpp 内
// file-static）施加 always_inline——语义不变：无递归、无取地址逃逸。
// 公开的阶段成员函数本身是普通外联定义（可从任意 TU 链接、可组合），
// 内部委派同一实现函数；经 DefaultScheduler / SchedulingPipeline::route
// 的热路径只触碰内部实现函数，保持与 0.6.1 相同的单一扁平函数形状。
// MSVC 退化为普通 inline，由 /Ob2 best-effort 折叠。
#if defined(__GNUC__) || defined(__clang__)
#define KAIRO_SCHED_HOT __attribute__((always_inline)) inline
#else
#define KAIRO_SCHED_HOT inline
#endif

/**
 * @brief 阶段一：deadline 约束过滤。
 *
 * 提交时点已过期的任务没有调偿价值，明确拒绝而不是让它排队后必然错过
 * （deadline-aware admission 的第一层）。0.6.1 拒绝语义锁定为
 * "严格已过"（now > deadline）；恰好等于 deadline 的提交仍被接受。
 *
 * @return true 表示约束通过；false 时 rejection 被填充为完整拒绝决策
 * （make_rejection 语义：task_name 使用 options.name 原文、无
 * "facade_submit_auto" 兜底、selected_executor_name 为空——约束拒绝
 * 发生在候选生成之前，尚无投递目标）。通过路径不写 rejection。
 */
class DeadlineConstraintFilter {
public:
    bool apply(const TaskRouter::Request& request,
               const std::vector<ExecutorCapability>& capabilities,
               RoutingDecision& rejection) const;
};

/**
 * @brief 阶段一：GPU 资源可行性过滤（feasibility hint，非 reservation）。
 *
 * 仅当 CpuOrGpu 相关请求声明了 gpu_device / memory_bytes 时核对能力
 * 快照：设备号不符 → BackendUnavailable，内存超过可用量 →
 * CapacityPressure，均携带 ResourceInfeasible 诊断位。capability 未知
 * （gpu_device < 0 / 内存总量 0）时按 §6.4 弱一致模型跳过检查。
 * 无匹配 GPU 能力时放行，交给候选生成阶段复用意图路由的既有语义。
 */
class GpuResourceConstraintFilter {
public:
    bool apply(const TaskRouter::Request& request,
               const std::vector<ExecutorCapability>& capabilities,
               RoutingDecision& rejection) const;
};

/**
 * @brief 约束过滤的编译期组合链：按声明顺序依次应用，首个拒绝短路。
 *
 * 通过 `ConstraintFilterChain<F1, F2, ...>` 组合任意过滤器；在
 * scheduling_pipeline.cpp 的 pipeline 实例化单元内折叠为顺序分支。
 */
template <typename... Filters>
class ConstraintFilterChain {
public:
    ConstraintFilterChain() = default;
    // 空包时与默认构造冲突，requires 子句禁用（组合链至少一个过滤器）。
    explicit ConstraintFilterChain(Filters... filters)
        requires (sizeof...(Filters) > 0)
        : filters_(std::move(filters)...) {}

    KAIRO_SCHED_HOT bool apply(const TaskRouter::Request& request,
                               const std::vector<ExecutorCapability>& capabilities,
                               RoutingDecision& rejection) const {
        return apply_impl(std::index_sequence_for<Filters...>{},
                          request, capabilities, rejection);
    }

private:
    template <std::size_t... I>
    KAIRO_SCHED_HOT bool apply_impl(std::index_sequence<I...>,
                                    const TaskRouter::Request& request,
                                    const std::vector<ExecutorCapability>& capabilities,
                                    RoutingDecision& rejection) const {
        // && 折叠保证从左到右短路求值：首个拒绝后不再运行后续过滤器。
        return ((std::get<I>(filters_).apply(request, capabilities, rejection)) && ...);
    }

    std::tuple<Filters...> filters_;
};

/**
 * @brief 候选生成阶段的选择计划。
 *
 * 0.6.1 决策树至多产生一个候选：count == 1 表示 primary 决策存储中
 * 已是最终候选（生成阶段直写，选择阶段零搬运）；count == 0 表示拒绝，
 * 拒绝决策在拒绝路径惰性构造（make_refusal）。拒绝决策保留 0.6.1
 * TaskRouter::route 拒绝路径的字段形态：task_name 有
 * "facade_submit_auto" 兜底、selected_executor_name 固定 "default"。
 *
 * refusal 用原始字节存储而非 std::optional：libstdc++ 的 optional
 * 默认构造会被编译器合并为整块 memset（实测 144B rep stos ≈ +7ns），
 * 而接受路径只需要两个标志字节。后续版本的评分阶段（AdaptiveScoring）
 * 在此扩展多候选排序载体（候选即完整决策载荷，按分数重排后由选择
 * 阶段物化最优者）。
 */
struct CandidatePlan {
    unsigned char count = 0;  // 0 = 拒绝（refusal 生效）；1 = primary 生效

    CandidatePlan() noexcept = default;
    ~CandidatePlan() {
        if (refusal_engaged) {
            refusal().~RoutingDecision();
        }
    }
    CandidatePlan(const CandidatePlan&) = delete;
    CandidatePlan& operator=(const CandidatePlan&) = delete;
    CandidatePlan(CandidatePlan&&) = delete;
    CandidatePlan& operator=(CandidatePlan&&) = delete;

    /** @brief 拒绝路径：在惰性存储中原地构造拒绝决策。 */
    RoutingDecision& make_refusal() {
        refusal_engaged = 1;
        return *::new (static_cast<void*>(refusal_storage)) RoutingDecision();
    }

    /** @brief 选择阶段：移出预组装的拒绝决策（仅 count == 0 时合法）。 */
    RoutingDecision&& take_refusal() noexcept {
        return std::move(refusal());
    }

private:
    RoutingDecision& refusal() noexcept {
        return *std::launder(reinterpret_cast<RoutingDecision*>(refusal_storage));
    }

    unsigned char refusal_engaged = 0;
    alignas(RoutingDecision)
        unsigned char refusal_storage[sizeof(RoutingDecision)];
};

/**
 * @brief 阶段二（0.6.1 默认）：意图路由候选生成。
 *
 * 承载原 TaskRouter::route 的完整决策树：意图分流（Auto / GeneralCpu /
 * 需要 typed API 的意图）、GPU 可达性（registered/running/capacity）、
 * FallbackPolicy（RequireRequestedBackend / AllowCpu）、
 * gpu_selected 启发式覆盖。候选决策直接写入 primary（至多 1 个），
 * reason/status/detail 与 0.6.1 逐项一致。
 */
class IntentCandidateGenerator {
public:
    void generate(const TaskRouter::Request& request,
                  const std::vector<ExecutorCapability>& capabilities,
                  RoutingDecision& primary, CandidatePlan& plan) const;
};

/**
 * @brief 阶段三（0.6.1 默认）：空评分实现。
 *
 * DefaultScheduler 是确定性的：候选生成序即最终序，评分阶段为空。
 * AdaptiveScheduler（后续版本）以自己的评分阶段替换本组件，
 * 复用过滤与候选生成，实现"利用观测的调度"。
 */
class IdentityScoring {
public:
    void rank(CandidatePlan& plan, const TaskRouter::Request& request) const
        noexcept {
        (void)plan;
        (void)request;
    }
};

/**
 * @brief 阶段四：按计划物化最终决策。
 *
 * count == 1 时 decision 已是最终候选（生成阶段直写）；count == 0 时
 * 将预组装的拒绝决策移入 decision。选择阶段消费计划。
 */
void select_first(CandidatePlan& plan, RoutingDecision& decision);

/**
 * @brief 选择后处理：affinity advisory 诊断。
 *
 * 请求核集合与目标后端绑核不相交时任务仍被接受（绑核是后端启动期
 * 属性），但记录结构化诊断：diagnostics |= AffinityMismatch +
 * status = AcceptedDegraded。已拒绝的决策不再附加 affinity 警告
 * （拒绝原因保持单一明确）；已降级的决策（如 fallback）仍记录诊断位，
 * 但 reason 保留首个降级原因（FallbackPolicy），不覆盖。
 */
void apply_affinity_advisory(RoutingDecision& decision,
                             const TaskRouter::Request& request,
                             const std::vector<ExecutorCapability>& capabilities);

/**
 * @brief 四阶段调度 pipeline（M1 结构化）。
 *
 *   约束过滤 → 候选生成 → 评分/排序 → 选择
 *
 * `IScheduler` 对外接口不变；`DefaultScheduler` 以
 * `SchedulingPipeline<IdentityScoring>` 实例运行（在
 * scheduling_pipeline.cpp 中显式实例化，与 0.6.1 结果逐项一致）。
 * 自定义评分阶段：库内类型（如后续的 AdaptiveScoring）在同一编译单元
 * 追加显式实例化；用户自定义评分请直接组合上方各阶段组件。
 */
template <typename ScoringStage = IdentityScoring>
class SchedulingPipeline {
public:
    SchedulingPipeline() = default;
    explicit SchedulingPipeline(ScoringStage scoring)
        : scoring_(std::move(scoring)) {}

    RoutingDecision route(const TaskRouter::Request& request,
                          const std::vector<ExecutorCapability>& capabilities) const;

private:
    IntentCandidateGenerator candidate_generator_{};
    ScoringStage scoring_{};
};

extern template class SchedulingPipeline<IdentityScoring>;

}  // namespace kairo::scheduling
