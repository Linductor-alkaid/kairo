#pragma once

#include "task_options.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace kairo {

struct SchedulingFeedback;

namespace scheduling {

// ---- 0.7.0 M2：反馈聚合层（roadmap §2.3）----
//
// on_task_completed() 在 worker 线程同步调用，聚合因此满足三条硬约束：
// 不阻塞、不分配、不抛异常。样本按 (backend, executor_name, qos) 分键，
// 以固定数量的 per-worker 分片写入（thread→shard 经进程级 thread_local
// 槽位稳定映射；worker 数超过分片数时退化为共享分片，仍然无锁）。读方
// （后续 M3 AdaptiveScheduler 的评分阶段）只读周期性合并出的不可变快照，
// 经 RCU 风格指针交换发布，读路径无锁。
//
// 键空间有界：每分片固定槽位上限（见 FeedbackAggregatorConfig::max_keys）。
// 未见过的键先到先得占槽；表满后新键的样本被丢弃并计入 dropped_samples，
// 语义即 §6.4 的"未知即宽容"——聚合永远不改变任务结果。
//
// 一致性口径：计数（attempts/failures/deadline_misses/直方图）自聚合器
// 构造起累计；EWMA 是唯一带遗忘的量（整数定点，ewma_alpha_permille 平滑）。
// 快照为弱一致（分片间不同步），满足诊断与自适应决策的精度要求。

/** @brief 单个直方图桶：上界与计数。最后一桶上界为 INT64_MAX（溢出桶）。 */
struct FeedbackHistogramBucket {
    int64_t upper_bound_ns = 0;
    uint64_t count = 0;
};

/** @brief 一条时延直方图（队列等待或执行时长），桶界来自聚合器配置。 */
struct FeedbackHistogram {
    std::vector<FeedbackHistogramBucket> buckets;

    /** @brief 全部桶计数之和（该键聚合到的样本总数，单侧时延维度）。 */
    uint64_t total() const noexcept {
        uint64_t sum = 0;
        for (const FeedbackHistogramBucket& bucket : buckets) {
            sum += bucket.count;
        }
        return sum;
    }
};

/** @brief 聚合键：(backend, executor_name, qos)。executor_name 超过
 *  kFeedbackExecutorNameCapacity-1 字节时截断（截断后相同的键会合并，
 *  属"未知即宽容"语义的可接受弱化）。 */
struct FeedbackKey {
    ExecutionBackend backend = ExecutionBackend::DefaultAsync;
    std::string executor_name;  // 例："default"
    QosClass qos = QosClass::Standard;

    friend bool operator==(const FeedbackKey&, const FeedbackKey&) = default;
};

/** @brief 单键聚合结果（分片合并后的诊断视图）。 */
struct FeedbackEntry {
    FeedbackKey key;
    uint64_t attempts = 0;        // 成功 + 失败样本数
    uint64_t failures = 0;        // success == false 的样本数
    uint64_t deadline_misses = 0; // 开始执行时已错过的样本数
    double failure_rate = 0.0;    // failures / attempts（attempts 为 0 时为 0）
    int64_t ewma_queue_wait_ns = 0;
    int64_t ewma_execution_duration_ns = 0;
    FeedbackHistogram queue_wait;
    FeedbackHistogram execution_duration;
};

/** @brief 一次合并发布的不可变快照（RCU 读方持有的对象）。
 *
 * 合并为全量重算：跨分片计数相加，EWMA 按分片样本数加权平均。
 * entries 按 (backend, qos, executor_name) 排序，输出确定性；entries
 * 只包含至少有一条样本的键——预注册（register_key）但尚未观测到样本
 * 的键在首个样本到来前不出现，消费方因此可假设 entry.attempts > 0
 * （自适应评分不得把 ewma=0 的空键误读为"快"）。
 */
struct FeedbackSnapshot {
    std::chrono::steady_clock::time_point merged_at{};
    uint64_t merge_count = 0;       // 自构造起第几次合并发布
    uint64_t dropped_samples = 0;   // 键表溢出丢弃的样本数（累计）
    uint64_t total_attempts = 0;
    uint64_t total_failures = 0;
    std::vector<FeedbackEntry> entries;
};

/** @brief FeedbackAggregator 配置。构造时校验并夹取；config() 返回
 *  生效配置（桶界含非法回退与截断后的结果）。运行期只读。 */
struct FeedbackAggregatorConfig {
    /** 每分片键槽数（即全局不同键上限）。夹取到 [1, 64]。 */
    size_t max_keys = 16;
    /** EWMA 平滑系数（千分比）。夹取到 [1, 1000]；默认 125 = 1/8。 */
    unsigned ewma_alpha_permille = 125;
    /** refresh_if_stale() 的合并门限；0 表示每次调用都重合并。 */
    std::chrono::milliseconds merge_interval{100};
    /** 队列等待直方图桶上界（纳秒，升序，2..15 个）。 */
    std::vector<int64_t> queue_wait_bucket_bounds_ns = {
        1'000, 10'000, 100'000, 1'000'000, 10'000'000,
        100'000'000, 1'000'000'000, 10'000'000'000};
    /** 执行时长直方图桶上界（纳秒，升序，2..15 个）。 */
    std::vector<int64_t> execution_bucket_bounds_ns = {
        1'000, 10'000, 100'000, 1'000'000, 10'000'000,
        100'000'000, 1'000'000'000, 10'000'000'000};
};

/**
 * @brief 执行期反馈聚合器（0.7.0 M2）。
 *
 * 写路径：record() 由 worker 线程经 report_scheduling_feedback() 喂入，
 * 无锁、无分配、noexcept；样本落入调用线程的分片，先按该分片已发布槽位
 * 匹配键（acquire 读槽位掩码 + 不可变键比较），未命中走登记互斥锁慢路径
 * （建槽或丢弃计数）。
 *
 * 读路径：snapshot() 无锁取当前快照（永不为空，构造即发布空快照）；
 * refresh()/refresh_if_stale() 在互斥锁内重合并并以原子 shared_ptr
 * 发布（RCU 风格指针交换），旧快照由 shared_ptr 引用计数回收，在读者
 * 手中保持有效。
 *
 * 线程安全：全部方法线程安全且 const 方法可并发调用；record() 与
 * register_key()、refresh() 并发安全。本类不拥有任何线程——周期性合并
 * 的驱动方由集成方决定（诊断读方调 refresh_if_stale()，M3 的 AdaptiveScheduler
 * 在其评分阶段自行决定刷新节奏）。
 */
class FeedbackAggregator {
public:
    /** 每分片键槽数硬上限（槽位掩码为 uint64 位图）。 */
    static constexpr size_t kMaxKeys = 64;
    /** executor_name 槽内定长容量（含 '\0'）。 */
    static constexpr size_t kExecutorNameCapacity = 48;
    /** 直方图桶数硬上限（桶界 ≤ 15 个 + 1 溢出桶）。 */
    static constexpr size_t kMaxHistogramBuckets = 16;
    /** record() 对样本值（queue_wait/execution_duration）的夹取上界（ns），
     *  保证 EWMA 定点乘法不溢出。 */
    static constexpr int64_t kMaxSampleNs = 1'000'000'000'000'000; // 1e15

    explicit FeedbackAggregator(FeedbackAggregatorConfig config = {});
    ~FeedbackAggregator();
    FeedbackAggregator(const FeedbackAggregator&) = delete;
    FeedbackAggregator& operator=(const FeedbackAggregator&) = delete;

    /**
     * @brief 记录一条执行期反馈（worker 热路径）。
     *
     * 无锁、无分配、不抛异常；负样本值夹取为 0，超过 kMaxSampleNs 的值
     * 夹取到上界。键未注册且表满时样本被丢弃（dropped_samples 计数）。
     */
    void record(const SchedulingFeedback& feedback) noexcept;

    /**
     * @brief 预注册键（在所有分片建槽），使后续 record() 稳定走快路径。
     *
     * 幂等：已存在的键不重复建槽。全或无：任一分片无空槽且键未存在时
     * 返回 false 且不做任何修改。
     */
    bool register_key(const FeedbackKey& key);

    /** @brief 当前已合并快照（无锁；永不为空）。 */
    std::shared_ptr<const FeedbackSnapshot> snapshot() const noexcept;

    /** @brief 立即重合并并发布，返回新快照。 */
    std::shared_ptr<const FeedbackSnapshot> refresh() const;

    /** @brief 距上次合并超过 merge_interval 才重合并；否则返回当前快照。 */
    std::shared_ptr<const FeedbackSnapshot> refresh_if_stale() const;

    /** @brief 快照文本（key=value 行式，与 get_snapshot_text() 风格一致）。 */
    static std::string format_text(const FeedbackSnapshot& snapshot);

    const FeedbackAggregatorConfig& config() const noexcept { return config_; }

private:
    struct Shards;  // 分片数组（pimpl：分片内含槽位表与原子统计）

    FeedbackAggregatorConfig config_;
    std::unique_ptr<Shards> shards_;
    // 以下状态在 const 读方法（refresh 路径）中变更：
    mutable std::atomic<std::shared_ptr<const FeedbackSnapshot>> snapshot_;
    mutable std::atomic<int64_t> last_merge_steady_ns_{0};
    mutable std::mutex merge_mutex_;  // 串行化合并与键登记（读路径无锁，不受影响）
    mutable std::atomic<uint64_t> merge_count_{0};
};

}  // namespace scheduling
}  // namespace kairo
