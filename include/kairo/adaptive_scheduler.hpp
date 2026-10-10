#pragma once

#include "feedback_aggregator.hpp"
#include "scheduler.hpp"
#include "scheduling_pipeline.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace kairo {

// ============================================================================
// 0.7.0 M3：AdaptiveScheduler MVP（roadmap §2.4）。
//
// 范围刻意收窄为三类可解释的决策：
//
// 1. CPU/GPU 选择（CpuOrGpu 意图）：以内嵌 FeedbackAggregator 的端到端
//    时延 EWMA（queue wait + execution duration，按 (backend, executor,
//    qos) 分键）取代静态启发式。双侧样本数达到 min_samples 才采信历史，
//    否则回退 0.6.1 启发式；切换需优于在用侧 (1 - hysteresis_margin)
//    以上，防止震荡。命中历史时 reason = AdaptiveHistory 且 diagnostics
//    含 RoutingDiagnostics::AdaptiveHistory，detail 携带两侧 EWMA 数值。
//    学习粒度为 per-(backend, executor, qos)：同 QoS 内异构任务的时延
//    混在同一键下，MVP 不做按任务类型细分。显式用户约束优先于历史：
//    FallbackPolicy::RequireRequestedBackend 的请求不做历史翻转（与
//    "显式 priority 永不被覆盖"同一原则）。
// 2. QoS 感知降载：按 QoS 类合并 queue wait 直方图求 p99，连续
//    shed_breach_windows 个合并窗口超过目标后开闸，对"严格低于"超阈
//    QoS 的提交返回结构化拒绝（reason = LoadShedding）；p99 低于
//    目标 × recovery_ratio 后关闸。窗口样本不足 min_window_samples 时
//    按"未知即宽容"跳过评估（不开闸、不提升）。与 max_in_flight_tasks
//    硬上界共存：只做更早、更有区分度的拒绝。
// 3. QoS→priority 有界提升：长期（promotion_windows 个连续窗口）超阈
//    的 QoS 类，其默认排队优先级 +1 级（封顶 CRITICAL）；用户显式设置
//    的 priority 永远原样生效。经 IScheduler::effective_priority_for()
//    生效，被调整的提交计数到 SchedulingMetrics::priority_promoted_count。
//
// 原则对齐（roadmap §0）：自适应是 opt-in（经 set_scheduler() 注入，
// DefaultScheduler 不受影响）；每个决策可从 RoutingDecision、
// SchedulingMetrics 与本类诊断接口复原；不触碰 NUMA、跨 pool 迁移与
// 基于利用率的线程数调整（明确不做）。
// ============================================================================

/**
 * @brief AdaptiveScheduler 配置。构造时校验并夹取；config() 返回生效值。
 */
struct AdaptiveSchedulerConfig {
    // ---- CPU/GPU 历史选择 ----
    /** 双侧（CPU/GPU 键）最少样本数（attempts）；任一侧不足即回退启发式。
     *  夹取到 [1, 1e6]。 */
    size_t min_samples = 8;
    /** 滞回边际：历史选择与在用侧不同时，挑战侧端到端 EWMA 须低于在用侧
     *  (1 - margin) 倍才切换。夹取到 [0.05, 0.9]。 */
    double hysteresis_margin = 0.25;

    // ---- QoS 感知降载 / QoS→priority 提升 ----
    /** QoS 感知降载总开关（默认开启——注入 AdaptiveScheduler 即选择了
     *  自适应降载；目标全为 0 时等价关闭）。 */
    bool load_shedding_enabled = true;
    /** 各 QoS 类的 queue wait p99 目标（纳秒），按 QosClass 枚举序索引：
     *  [BestEffort, Standard, Interactive, Critical]。0 = 该类不评估
     *  （不降载、不提升，"未知即宽容"的显式表达）。负值按 0 处理。 */
    std::array<int64_t, 4> queue_wait_p99_target_ns = {
        0, 100'000'000, 10'000'000, 2'000'000};
    /** 连续超阈窗口数达到该值才开闸降载。夹取到 [1, 1e6]。 */
    size_t shed_breach_windows = 3;
    /** 关闸滞回：连续恢复窗口（p99 < 目标 × recovery_ratio）达到该值才
     *  关闸；与开闸条件对称，防止在阈值附近反复开关。夹取到 [1, 1e6]。 */
    size_t shed_close_windows = 5;
    /** QoS→priority 提升总开关。 */
    bool priority_promotion_enabled = true;
    /** 连续超阈窗口数达到该值才提升默认优先级（"长期"语义，须 ≥
     *  shed_breach_windows 才有区分意义；构造时若更小则取
     *  shed_breach_windows）。上限 1e6。 */
    size_t promotion_windows = 6;
    /** 取消提升的滞回：连续恢复窗口达到该值才取消（与提升条件对称）。
     *  构造时若更小则取 promotion_windows。 */
    size_t promotion_close_windows = 10;
    /** 评估窗口最少样本数：某 QoS 类在窗口内样本不足时跳过该类评估
     *  并清零连击计数。夹取到 [1, 1e6]。 */
    size_t min_window_samples = 32;
    /** 恢复比：p99 低于 目标 × recovery_ratio 后关闸/取消提升（滞回，
     *  防止在阈值附近反复开关）。夹取到 [0.1, 0.95]。 */
    double recovery_ratio = 0.5;

    // ---- 内嵌反馈聚合器（决策输入）----
    /** merge_interval 决定评估节奏：route() 经 refresh_if_stale() 触发
     *  合并，每个 interval 至多重评估一次派生状态。 */
    scheduling::FeedbackAggregatorConfig aggregator;
};

/**
 * @brief 反馈驱动的自适应调度器（0.7.0 M3 MVP）。
 *
 * 结构：复用 M1 的 SchedulingPipeline<IdentityScoring> 产出 0.6.1 基线
 * 决策，再按内嵌聚合器的快照做三类后处理（CPU/GPU 覆盖 → QoS 降载）。
 * 提交侧读路径无锁（快照 RCU 读 + 两个掩码原子读）；派生状态（降载/
 * 提升状态机）在检测到新合并快照时由本线程在互斥锁内惰性重评估，每个
 * merge_interval 至多一次。
 *
 * 线程安全：全部方法线程安全；route() / effective_priority_for() 可多
 * 提交线程并发调用，on_task_completed() 在 worker 线程同步调用（无锁、
 * 不阻塞、不抛异常）。反馈经 on_task_completed() 进入内嵌聚合器——与
 * Executor 门面的诊断聚合器（feedback_aggregator.hpp）相互独立。
 */
class AdaptiveScheduler final : public IScheduler {
public:
    /** 每类 QoS 的状态位数（QosClass 枚举基数）。 */
    static constexpr size_t kQosClassCount = 4;

    explicit AdaptiveScheduler(AdaptiveSchedulerConfig config = {});
    ~AdaptiveScheduler() override;
    AdaptiveScheduler(const AdaptiveScheduler&) = delete;
    AdaptiveScheduler& operator=(const AdaptiveScheduler&) = delete;

    RoutingDecision route(const TaskRouter::Request& request,
                          const std::vector<ExecutorCapability>& capabilities) override;

    /** @brief worker 线程终态反馈：无锁写入内嵌聚合器。 */
    void on_task_completed(const SchedulingFeedback& feedback) override;

    /** @brief 需要执行期反馈（测量包装由 facade 按此值附加）。 */
    bool wants_feedback() const noexcept override { return true; }

    /**
     * @brief QoS 默认排队优先级咨询：类处于提升态时返回 +1 级
     *  （封顶 CRITICAL），否则原样返回。显式 priority（priority_set）
     *  不受影响，原样返回。
     */
    int effective_priority_for(const TaskOptions& options,
                               int default_priority) const override;

    /** @brief 生效配置（含构造期夹取结果）。运行期只读。 */
    const AdaptiveSchedulerConfig& config() const noexcept { return config_; }

    // ---- 诊断只读接口（决策复原的第二通道）----

    /** @brief 该 QoS 类的 queue wait p99 是否正处于超阈降载态
     *  （其效果是拒绝严格更低 QoS 的提交）。 */
    bool load_shed_active(QosClass breached_class) const noexcept;

    /** @brief 该 QoS 类的默认排队优先级当前是否被提升（+1 级）。 */
    bool priority_promoted(QosClass qos) const noexcept;

    /** @brief 内嵌聚合器快照（refresh_if_stale 语义；诊断用）。 */
    std::shared_ptr<const scheduling::FeedbackSnapshot> feedback_snapshot() const;

    /** @brief 人读状态行（降载/提升类清单 + 合并轮次），用于日志与基准。 */
    std::string format_state_text() const;

private:
    /** 派生状态与评估状态机（pimpl：互斥锁 + 连击计数 + 发布掩码）。 */
    struct State;

    void evaluate_if_new_snapshot() const;
    void adapt_cpu_or_gpu(const TaskRouter::Request& request,
                          const std::vector<ExecutorCapability>& capabilities,
                          RoutingDecision& decision) const;
    void maybe_shed(const TaskRouter::Request& request,
                    RoutingDecision& decision) const;

    AdaptiveSchedulerConfig config_;
    scheduling::FeedbackAggregator aggregator_;
    std::unique_ptr<State> state_;
    scheduling::SchedulingPipeline<scheduling::IdentityScoring> pipeline_;
};

}  // namespace kairo
