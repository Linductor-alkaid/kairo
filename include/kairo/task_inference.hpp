#pragma once

#include "scheduler.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace kairo {

/**
 * @brief 任务特征自动推断结果（0.7.1 §3.0.2）。
 *
 * 推断是**建议优先于切换**：结果只注解到 RoutingDecision（诊断位 +
 * inferred_reason + detail 建议），从不改变路由——submit_auto 的
 * std::future 契约只有默认池能履行，lockfree/realtime 后端经
 * dispatch_auto 的显式意图表达。误报的代价因此只是日志噪音。
 */
struct TaskCharacteristics {
    bool is_blocking_io = false;       // 名称模式（*_io_* / *_read_* / *_write_*）
    bool needs_low_latency = false;    // 名称模式（*_realtime_* / *_control_*）
    bool is_periodic = false;          // 同名任务完成间隔稳定（σ/μ < 10%）
    size_t estimated_duration_ns = 0;  // 执行时长 EWMA（无样本为 0）
    size_t samples = 0;                // 该名称的已完成样本数
};

namespace detail {

/**
 * @brief 按任务名称的有界推断历史表（0.7.1 §3.0.2）。
 *
 * 样本源：执行期反馈（report_scheduling_feedback 转发；反馈包装在
 * 启用推断时自动附加，与 wants_feedback() 解耦）。键空间有界
 * （kMaxTrackedNames，满后新名称样本丢弃计数——"未知即宽容"，与
 * FeedbackAggregator 同款策略）。互斥锁保护：record 在 worker 线程、
 * analyze 在提交线程。
 *
 * 推断规则（确定性，全部只建议不切换）：
 * - blocking/低延迟：纯名称模式（误报仅产生建议噪音，风险为零）；
 *   estimated_duration 来自 EWMA（无样本为 0，不参与判定）；
 * - periodic：≥ kMinIntervalSamples 个完成间隔且
 *   stddev/mean < 0.1（完成间隔近似提交间隔，文档化偏差）。
 */
class TaskInferenceTable {
public:
    static constexpr size_t kMaxTrackedNames = 64;
    static constexpr uint64_t kMinIntervalSamples = 4;   // 至少 4 个间隔
    static constexpr double kPeriodicRegularRatio = 0.1; // σ/μ < 10%

    /** @brief 记录一次完成（now_ns 为完成时点，steady 纳秒）。 */
    void record(const SchedulingFeedback& feedback, int64_t now_ns) noexcept;

    /** @brief 推断（无历史时仅名称模式可命中）。 */
    TaskCharacteristics analyze(const std::string& task_name,
                                int64_t now_ns) const;

    /** @brief 名称是否有历史样本（推断被显式 intent 覆盖的计数依据）。 */
    bool knows(const std::string& task_name) const;

    /** @brief 因键空间满而丢弃样本的名称数。 */
    uint64_t dropped_names() const noexcept;

private:
    struct Entry {
        double duration_ewma_ns = 0;
        uint64_t samples = 0;
        int64_t last_completion_ns = 0;
        double interval_ewma_ns = 0;
        double interval_ew_variance = 0;
        uint64_t interval_samples = 0;
    };

    static bool name_matches(const std::string& name,
                             std::initializer_list<const char*> patterns);

    mutable std::mutex mutex_;
    std::unordered_map<std::string, Entry> entries_;
    uint64_t dropped_names_ = 0;
};

}  // namespace detail

}  // namespace kairo
