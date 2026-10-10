// 0.7.0 M2 FeedbackAggregator 行为测试
// (docs/design/scheduling_runtime.md §7；独立验证代理编写，不修改库代码)。
//
// 覆盖：
//  A. EWMA 数学：默认 alpha=125 的整数定点语义（手工推演逐样本期望值；
//     单线程单键下快照加权平均退化为 ewma 本身）。
//  B. 直方图桶定位：自定义桶界的边界值与两侧值（恰等于上界归上桶、
//     溢出桶 upper_bound=INT64_MAX）、桶 upper_bound/count 逐一断言、
//     queue_wait 与 execution_duration 两个直方图相互独立。
//  C. 计数与失败率：混合 success/fail/deadline_missed 样本；
//     预注册未喂样的键（attempts=0）不产生 entry。
//  D. 键语义：不同 (backend, executor_name, qos) 独立 entry、
//     entries 排序确定性、executor_name 超 47 字节截断与合并。
//  E. 键空间有界：max_keys=2 时第 3 键丢弃并计 dropped_samples、
//     已有键不受影响；register_key 全或无 + 幂等 + 预注册快路径等价。
//  F. 分片并发：8 线程 × N 条 record（同键/多键两组）无丢失更新。
//  G. RCU 快照：构造即发布、指针随 refresh 变化、merge_count 单调、
//     旧 shared_ptr 不可变性、refresh_if_stale 门限语义。
//  H. record 契约：noexcept 静态断言；负值/超大值夹取语义。
//  I. 配置归一：max_keys/alpha 夹取、非升序桶界回退默认 8 界、
//     超 15 界截断、config() 返回夹取后的标量。
//  J. format_text 行式输出契约。
//  K. Executor 集成：注入 wants_feedback 调度器后样本流入、
//     与 feedback_reported_count 一致；默认调度器零流入；
//     get_snapshot_text 前缀契约与 scheduling_feedback 段。

#include <gtest/gtest.h>

#include <kairo/executor.hpp>
#include <kairo/feedback_aggregator.hpp>
#include <kairo/scheduler.hpp>
#include <kairo/scheduling.hpp>
#include <kairo/task_options.hpp>

#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace kairo;
namespace sched = kairo::scheduling;

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;
using sched::FeedbackAggregator;
using sched::FeedbackAggregatorConfig;
using sched::FeedbackEntry;
using sched::FeedbackKey;
using sched::FeedbackSnapshot;

constexpr int64_t kInt64Max = std::numeric_limits<int64_t>::max();

// FeedbackAggregatorConfig 默认桶界（与头文件默认值一致，供回退断言复用）。
constexpr int64_t kDefaultBounds[8] = {
    1'000, 10'000, 100'000, 1'000'000, 10'000'000,
    100'000'000, 1'000'000'000, 10'000'000'000};

SchedulingFeedback make_feedback(
    QosClass qos = QosClass::Standard,
    int64_t queue_wait_ns = 50'000,
    int64_t duration_ns = 1'000'000,
    bool success = true,
    bool deadline_missed = false,
    ExecutionBackend backend = ExecutionBackend::DefaultAsync,
    const std::string& executor_name = "default") {
    SchedulingFeedback feedback;
    feedback.task_id = "test";
    feedback.qos = qos;
    feedback.success = success;
    feedback.backend = backend;
    feedback.executor_name = executor_name;
    feedback.queue_wait_ns = queue_wait_ns;
    feedback.execution_duration_ns = duration_ns;
    feedback.deadline_missed = deadline_missed;
    return feedback;
}

// 汇总一条 entry 某一直方图全部桶的计数（独立验证辅助，不用库内实现）。
uint64_t histogram_total(const sched::FeedbackHistogram& histogram) {
    uint64_t total = 0;
    for (const auto& bucket : histogram.buckets) {
        total += bucket.count;
    }
    return total;
}

// 按 (backend, qos, executor_name) 查找 entry；不存在返回 nullptr。
const FeedbackEntry* find_entry(const FeedbackSnapshot& snapshot,
                                ExecutionBackend backend, QosClass qos,
                                const std::string& name) {
    for (const auto& entry : snapshot.entries) {
        if (entry.key.backend == backend && entry.key.qos == qos &&
            entry.key.executor_name == name) {
            return &entry;
        }
    }
    return nullptr;
}

// 与契约测试同款：带线程数的默认池初始化。
void init_executor(Executor& executor, size_t threads) {
    ExecutorConfig config;
    config.min_threads = threads;
    config.max_threads = threads;
    ASSERT_TRUE(executor.initialize(config));
}

// route() 委托 DefaultScheduler 的 feedback 调度器（wants_feedback=true，
// 不自留样本——聚合流量全部经 Executor::report_scheduling_feedback 进入
// Executor 内置聚合器）。
class FeedbackRouteScheduler final : public IScheduler {
public:
    bool wants_feedback() const noexcept override { return true; }

    RoutingDecision route(const TaskRouter::Request& request,
                          const std::vector<ExecutorCapability>& capabilities) override {
        return default_.route(request, capabilities);
    }

    void on_task_completed(const SchedulingFeedback&) override {}

private:
    DefaultScheduler default_;
};

}  // namespace

// ---------------------------------------------------------------------------
// A. EWMA 数学（默认 alpha=125：ewma += (sample - ewma) * 125 / 1000，
//    整数除法向零截断；首样本从 0 出发）
// ---------------------------------------------------------------------------

TEST(EwmaMath, DefaultAlphaIntegerRecurrenceHandDerived) {
    FeedbackAggregator aggregator;  // 默认 alpha = 125
    ASSERT_EQ(aggregator.config().ewma_alpha_permille, 125u);

    // queue_wait 序列 1000, 2000, 3000 的手工推演：
    //   s1: 0 + (1000 - 0) * 125 / 1000        = 125
    //   s2: 125 + (2000 - 125) * 125 / 1000    = 125 + 234 (234375/1000 截断)
    //       = 359
    //   s3: 359 + (3000 - 359) * 125 / 1000    = 359 + 330 (330125/1000 截断)
    //       = 689
    // execution 序列 10M, 20M, 30M 的手工推演：
    //   s1: 0 + 10'000'000 * 125 / 1000        = 1'250'000
    //   s2: 1'250'000 + 18'750'000 * 125 / 1000 = 1'250'000 + 2'343'750
    //       = 3'593'750
    //   s3: 3'593'750 + 26'406'250 * 125 / 1000 = 3'593'750 + 3'300'781
    //       = 6'894'531
    const int64_t expected_queue_wait[] = {125, 359, 689};
    const int64_t expected_execution[] = {
        1'250'000, 3'593'750, 6'894'531};
    const int64_t queue_wait_samples[] = {1'000, 2'000, 3'000};
    const int64_t execution_samples[] = {
        10'000'000, 20'000'000, 30'000'000};

    for (int i = 0; i < 3; ++i) {
        aggregator.record(make_feedback(QosClass::Standard,
                                        queue_wait_samples[i],
                                        execution_samples[i]));
        const auto snapshot = aggregator.refresh();
        ASSERT_EQ(snapshot->entries.size(), 1u);
        EXPECT_EQ(snapshot->entries[0].attempts, static_cast<uint64_t>(i + 1))
            << "sample #" << i;
        // 单线程单键 → 单分片，加权平均 = 分片 ewma 本身（精确相等）。
        EXPECT_EQ(snapshot->entries[0].ewma_queue_wait_ns,
                  expected_queue_wait[i])
            << "queue_wait ewma after sample #" << i;
        EXPECT_EQ(snapshot->entries[0].ewma_execution_duration_ns,
                  expected_execution[i])
            << "execution ewma after sample #" << i;
    }
}

TEST(EwmaMath, WeightedAverageAcrossShardsStaysWithinSampleRange) {
    // 跨分片（多线程同键）无法精确推演，但加权平均必须落在样本范围内，
    // 且按样本数加权（此处各分片样本数相同 → 介于两样本值之间）。
    FeedbackAggregator aggregator;
    constexpr int kThreads = 4;
    constexpr int kPerThread = 500;
    std::vector<std::thread> workers;
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&aggregator, t] {
            // 偶数线程喂 100ns，奇数线程喂 900ns → 加权平均应恰在两者之间。
            const int64_t value = (t % 2 == 0) ? 100 : 900;
            for (int i = 0; i < kPerThread; ++i) {
                aggregator.record(make_feedback(QosClass::Standard, value, 0));
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    const auto snapshot = aggregator.refresh();
    ASSERT_EQ(snapshot->entries.size(), 1u);
    EXPECT_EQ(snapshot->entries[0].attempts,
              static_cast<uint64_t>(kThreads * kPerThread));
    EXPECT_GT(snapshot->entries[0].ewma_queue_wait_ns, 100)
        << "weighted ewma must exceed the low sample";
    EXPECT_LT(snapshot->entries[0].ewma_queue_wait_ns, 900)
        << "weighted ewma must stay below the high sample";
}

// ---------------------------------------------------------------------------
// B. 直方图桶定位
// ---------------------------------------------------------------------------

TEST(HistogramBucketing, HistogramTotalSumsBucketCounts) {
    // D5 回归守卫：FeedbackHistogram::total() 头文件内联实现（此前仅有
    // 声明、无定义，ODR 使用即链接失败）。
    sched::FeedbackHistogram histogram;
    EXPECT_EQ(histogram.total(), 0u) << "empty histogram totals to zero";
    histogram.buckets.push_back({100, 3});
    histogram.buckets.push_back({kInt64Max, 5});
    EXPECT_EQ(histogram.total(), 8u);
}

TEST(HistogramBucketing, BoundaryValuesAndOverflowBucket) {
    FeedbackAggregatorConfig config;
    config.queue_wait_bucket_bounds_ns = {100, 1'000, 10'000};
    config.execution_bucket_bounds_ns = {500, 5'000};
    FeedbackAggregator aggregator(config);

    // queue_wait 落桶期望（恰等于上界归上桶；超过末界进溢出桶）：
    //   50 → 桶0(≤100)   100 → 桶0(=bounds[0])
    //   101 → 桶1        1000 → 桶1(=bounds[1])
    //   1001 → 桶2       10000 → 桶2(=bounds[2])
    //   10001 → 桶3(溢出)
    const int64_t queue_wait_values[] = {50, 100, 101, 1'000,
                                         1'001, 10'000, 10'001};
    for (const int64_t value : queue_wait_values) {
        aggregator.record(make_feedback(QosClass::Standard, value, 600));
    }
    // execution 独立落桶：600 → 桶1(500 < x ≤ 5000)，每条都一样。
    const auto snapshot = aggregator.refresh();
    ASSERT_EQ(snapshot->entries.size(), 1u);
    const FeedbackEntry& entry = snapshot->entries[0];

    ASSERT_EQ(entry.queue_wait.buckets.size(), 4u);
    EXPECT_EQ(entry.queue_wait.buckets[0].upper_bound_ns, 100);
    EXPECT_EQ(entry.queue_wait.buckets[1].upper_bound_ns, 1'000);
    EXPECT_EQ(entry.queue_wait.buckets[2].upper_bound_ns, 10'000);
    EXPECT_EQ(entry.queue_wait.buckets[3].upper_bound_ns, kInt64Max)
        << "overflow bucket upper bound must be INT64_MAX";
    EXPECT_EQ(entry.queue_wait.buckets[0].count, 2u);  // 50, 100
    EXPECT_EQ(entry.queue_wait.buckets[1].count, 2u);  // 101, 1000
    EXPECT_EQ(entry.queue_wait.buckets[2].count, 2u);  // 1001, 10000
    EXPECT_EQ(entry.queue_wait.buckets[3].count, 1u);  // 10001
    EXPECT_EQ(histogram_total(entry.queue_wait), 7u);

    // 两个直方图相互独立：execution 桶界不同，600 恒落桶1。
    ASSERT_EQ(entry.execution_duration.buckets.size(), 3u);
    EXPECT_EQ(entry.execution_duration.buckets[0].upper_bound_ns, 500);
    EXPECT_EQ(entry.execution_duration.buckets[1].upper_bound_ns, 5'000);
    EXPECT_EQ(entry.execution_duration.buckets[2].upper_bound_ns, kInt64Max);
    EXPECT_EQ(entry.execution_duration.buckets[0].count, 0u);
    EXPECT_EQ(entry.execution_duration.buckets[1].count, 7u);
    EXPECT_EQ(entry.execution_duration.buckets[2].count, 0u);
}

TEST(HistogramBucketing, HistogramsTrackIndependentDimensions) {
    // 同一批样本里 queue_wait 与 execution_duration 分别按各自桶界统计。
    FeedbackAggregatorConfig config;
    config.queue_wait_bucket_bounds_ns = {1'000, 2'000};
    config.execution_bucket_bounds_ns = {100, 200};
    FeedbackAggregator aggregator(config);

    aggregator.record(make_feedback(QosClass::Standard, 500, 150));
    aggregator.record(make_feedback(QosClass::Standard, 1'500, 250));
    aggregator.record(make_feedback(QosClass::Standard, 2'500, 50));
    const auto snapshot = aggregator.refresh();
    ASSERT_EQ(snapshot->entries.size(), 1u);
    const FeedbackEntry& entry = snapshot->entries[0];

    ASSERT_EQ(entry.queue_wait.buckets.size(), 3u);
    EXPECT_EQ(entry.queue_wait.buckets[0].count, 1u);  // 500
    EXPECT_EQ(entry.queue_wait.buckets[1].count, 1u);  // 1500
    EXPECT_EQ(entry.queue_wait.buckets[2].count, 1u);  // 2500（溢出）

    ASSERT_EQ(entry.execution_duration.buckets.size(), 3u);
    // 50 → 桶0；150 → 桶1（100 < x ≤ 200）；250 → 桶2（溢出）。
    EXPECT_EQ(entry.execution_duration.buckets[0].count, 1u);
    EXPECT_EQ(entry.execution_duration.buckets[1].count, 1u);
    EXPECT_EQ(entry.execution_duration.buckets[2].count, 1u);
}

// ---------------------------------------------------------------------------
// C. 计数与失败率
// ---------------------------------------------------------------------------

TEST(CountersAndFailureRate, MixedSuccessFailureDeadlineSamples) {
    FeedbackAggregator aggregator;
    // 8 条：6 成功 + 2 失败；其中 4 条 deadline_missed（2 失败 2 成功）。
    aggregator.record(make_feedback(QosClass::Standard, 100, 200, true, false));
    aggregator.record(make_feedback(QosClass::Standard, 100, 200, true, false));
    aggregator.record(make_feedback(QosClass::Standard, 100, 200, true, true));
    aggregator.record(make_feedback(QosClass::Standard, 100, 200, true, false));
    aggregator.record(make_feedback(QosClass::Standard, 100, 200, true, false));
    aggregator.record(make_feedback(QosClass::Standard, 100, 200, true, true));
    aggregator.record(make_feedback(QosClass::Standard, 100, 200, false, true));
    aggregator.record(make_feedback(QosClass::Standard, 100, 200, false, true));

    const auto snapshot = aggregator.refresh();
    ASSERT_EQ(snapshot->entries.size(), 1u);
    const FeedbackEntry& entry = snapshot->entries[0];
    EXPECT_EQ(entry.attempts, 8u);
    EXPECT_EQ(entry.failures, 2u);
    EXPECT_EQ(entry.deadline_misses, 4u);
    EXPECT_NEAR(entry.failure_rate, 0.25, 1e-12);
    EXPECT_EQ(snapshot->total_attempts, 8u);
    EXPECT_EQ(snapshot->total_failures, 2u)
        << "total_failures must accumulate across shards (D2 regression guard)";
}

TEST(CountersAndFailureRate, ZeroAttemptKeyAbsentFromEntries) {
    // D4 契约（feedback_aggregator.hpp FeedbackSnapshot doc / 设计文档 §7）：
    // entries 只包含至少有一条样本的键——预注册但尚未观测到样本的键在
    // 首个样本到来前不出现，消费方可假设 entry.attempts > 0。
    FeedbackAggregator aggregator;
    const FeedbackKey key{ExecutionBackend::DefaultAsync, "preregistered",
                          QosClass::Interactive};
    ASSERT_TRUE(aggregator.register_key(key));

    const auto before = aggregator.refresh();
    EXPECT_EQ(before->entries.size(), 0u)
        << "registered key with zero samples must not appear in entries";
    EXPECT_EQ(before->total_attempts, 0u);

    aggregator.record(make_feedback(QosClass::Interactive, 100, 200, true,
                                    false, ExecutionBackend::DefaultAsync,
                                    "preregistered"));
    const auto after = aggregator.refresh();
    ASSERT_EQ(after->entries.size(), 1u);
    const auto* entry =
        find_entry(*after, key.backend, key.qos, key.executor_name);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->attempts, 1u);
}

TEST(CountersAndFailureRate, FailureSamplesStillFeedEwmaAndHistograms) {
    // 失败样本同样计入 attempts/ewma/直方图（反馈是执行期测量，与成败无关）。
    FeedbackAggregator aggregator;
    aggregator.record(make_feedback(QosClass::Standard, 1'000, 1'000, false));
    const auto snapshot = aggregator.refresh();
    ASSERT_EQ(snapshot->entries.size(), 1u);
    EXPECT_EQ(snapshot->entries[0].attempts, 1u);
    EXPECT_EQ(snapshot->entries[0].failures, 1u);
    EXPECT_EQ(snapshot->entries[0].ewma_queue_wait_ns, 125);
    EXPECT_EQ(histogram_total(snapshot->entries[0].queue_wait), 1u);
}

// ---------------------------------------------------------------------------
// D. 键语义
// ---------------------------------------------------------------------------

TEST(KeySemantics, DistinctKeysProduceDistinctEntries) {
    FeedbackAggregator aggregator;
    aggregator.record(make_feedback(QosClass::Standard, 100, 100, true, false,
                                    ExecutionBackend::DefaultAsync, "default"));
    aggregator.record(make_feedback(QosClass::Interactive, 100, 100, true,
                                    false, ExecutionBackend::DefaultAsync,
                                    "default"));
    aggregator.record(make_feedback(QosClass::Standard, 100, 100, true, false,
                                    ExecutionBackend::Gpu, "gpu0"));
    aggregator.record(make_feedback(QosClass::Standard, 100, 100, true, false,
                                    ExecutionBackend::DefaultAsync, "io"));
    const auto snapshot = aggregator.refresh();
    ASSERT_EQ(snapshot->entries.size(), 4u);
    for (const auto* entry : {
             find_entry(*snapshot, ExecutionBackend::DefaultAsync,
                        QosClass::Standard, "default"),
             find_entry(*snapshot, ExecutionBackend::DefaultAsync,
                        QosClass::Interactive, "default"),
             find_entry(*snapshot, ExecutionBackend::Gpu, QosClass::Standard,
                        "gpu0"),
             find_entry(*snapshot, ExecutionBackend::DefaultAsync,
                        QosClass::Standard, "io")}) {
        ASSERT_NE(entry, nullptr);
        EXPECT_EQ(entry->attempts, 1u);
    }
}

TEST(KeySemantics, EntriesSortedByBackendQosThenName) {
    FeedbackAggregator aggregator;
    // 刻意乱序喂入：排序键为 (backend, qos, executor_name)。
    aggregator.record(make_feedback(QosClass::Standard, 1, 1, true, false,
                                    ExecutionBackend::Gpu, "aaa"));
    aggregator.record(make_feedback(QosClass::Standard, 1, 1, true, false,
                                    ExecutionBackend::DefaultAsync, "alpha"));
    aggregator.record(make_feedback(QosClass::BestEffort, 1, 1, true, false,
                                    ExecutionBackend::DefaultAsync, "zulu"));
    aggregator.record(make_feedback(QosClass::Critical, 1, 1, true, false,
                                    ExecutionBackend::DefaultAsync, "aaa"));
    const auto snapshot = aggregator.refresh();
    ASSERT_EQ(snapshot->entries.size(), 4u);
    // qos 升序优先于 name：BestEffort(0) < Standard(1) < Critical(3)。
    EXPECT_EQ(snapshot->entries[0].key,
              (FeedbackKey{ExecutionBackend::DefaultAsync, "zulu",
                           QosClass::BestEffort}));
    EXPECT_EQ(snapshot->entries[1].key,
              (FeedbackKey{ExecutionBackend::DefaultAsync, "alpha",
                           QosClass::Standard}));
    EXPECT_EQ(snapshot->entries[2].key,
              (FeedbackKey{ExecutionBackend::DefaultAsync, "aaa",
                           QosClass::Critical}));
    EXPECT_EQ(snapshot->entries[3].key,
              (FeedbackKey{ExecutionBackend::Gpu, "aaa",
                           QosClass::Standard}));
    // 二次合并输出确定性（同一分片状态下排序稳定）。
    const auto again = aggregator.refresh();
    ASSERT_EQ(again->entries.size(), snapshot->entries.size());
    for (size_t i = 0; i < again->entries.size(); ++i) {
        EXPECT_EQ(again->entries[i].key, snapshot->entries[i].key) << i;
    }
}

TEST(KeySemantics, OverlongExecutorNameTruncatedTo47Bytes) {
    FeedbackAggregator aggregator;
    // 恰好 47 字节：不截断。
    const std::string exact(47, 'a');
    aggregator.record(make_feedback(QosClass::Standard, 1, 1, true, false,
                                    ExecutionBackend::DefaultAsync, exact));
    // 60 字节：截断为前 47 字节。
    const std::string overlong(60, 'a');
    aggregator.record(make_feedback(QosClass::Standard, 1, 1, true, false,
                                    ExecutionBackend::DefaultAsync, overlong));
    const auto snapshot = aggregator.refresh();
    ASSERT_EQ(snapshot->entries.size(), 1u)
        << "keys identical after truncation must merge into one entry";
    EXPECT_EQ(snapshot->entries[0].key.executor_name, exact);
    EXPECT_EQ(snapshot->entries[0].key.executor_name.size(), 47u);
    // 文档语义：截断后相同的键合并——两条样本都应计入同一 entry。
    EXPECT_EQ(snapshot->entries[0].attempts, 2u)
        << "both samples of the same truncated key must be aggregated";
    EXPECT_EQ(snapshot->dropped_samples, 0u)
        << "truncated keys must not exhaust the key table";
}

TEST(KeySemantics, RepeatedOverlongNameSamplesKeepAggregating) {
    // 回归守卫（D3 修复）：同一条超长 executor_name 反复喂入时必须持续
    // 聚合进同一 entry（docs/design/scheduling_runtime.md §7 "超长截断，
    // 截断后相同的键合并"；键比较长度须夹取到 47，默认 max_keys=16）。
    FeedbackAggregator aggregator;
    const std::string name(60, 'x');
    for (int i = 0; i < 40; ++i) {
        aggregator.record(make_feedback(QosClass::Standard, 1, 1, true, false,
                                        ExecutionBackend::DefaultAsync, name));
    }
    const auto snapshot = aggregator.refresh();
    ASSERT_EQ(snapshot->entries.size(), 1u);
    EXPECT_EQ(snapshot->entries[0].key.executor_name.size(), 47u);
    EXPECT_EQ(snapshot->entries[0].attempts, 40u)
        << "all samples of one logical key must aggregate";
    EXPECT_EQ(snapshot->dropped_samples, 0u)
        << "repeated samples of the same key must not be dropped";
}

// ---------------------------------------------------------------------------
// E. 键空间有界
// ---------------------------------------------------------------------------

TEST(BoundedKeyspace, ThirdKeyDroppedAndCounted) {
    FeedbackAggregatorConfig config;
    config.max_keys = 2;
    FeedbackAggregator aggregator(config);

    // 预注册 A/B 占满键表（register_key 作用于全部分片）。
    const FeedbackKey key_a{ExecutionBackend::DefaultAsync, "A",
                            QosClass::Standard};
    const FeedbackKey key_b{ExecutionBackend::DefaultAsync, "B",
                            QosClass::Interactive};
    const FeedbackKey key_c{ExecutionBackend::DefaultAsync, "C",
                            QosClass::Critical};
    ASSERT_TRUE(aggregator.register_key(key_a));
    ASSERT_TRUE(aggregator.register_key(key_b));

    // A/B 正常聚合。
    aggregator.record(make_feedback(QosClass::Standard, 1, 1, true, false,
                                    ExecutionBackend::DefaultAsync, "A"));
    aggregator.record(make_feedback(QosClass::Interactive, 1, 1, true, false,
                                    ExecutionBackend::DefaultAsync, "B"));

    // 第 3 键：慢路径无空槽 → 丢弃并计数；不抛异常。
    for (int i = 0; i < 4; ++i) {
        EXPECT_NO_FATAL_FAILURE(aggregator.record(
            make_feedback(QosClass::Critical, 1, 1, true, false,
                          ExecutionBackend::DefaultAsync, "C")));
    }

    const auto snapshot = aggregator.refresh();
    EXPECT_EQ(snapshot->entries.size(), 2u)
        << "dropped key must not produce an entry";
    EXPECT_EQ(snapshot->dropped_samples, 4u);
    const auto* entry_a = find_entry(*snapshot, key_a.backend, key_a.qos, "A");
    const auto* entry_b = find_entry(*snapshot, key_b.backend, key_b.qos, "B");
    ASSERT_NE(entry_a, nullptr);
    ASSERT_NE(entry_b, nullptr);
    EXPECT_EQ(entry_a->attempts, 1u) << "existing key stats unaffected";
    EXPECT_EQ(entry_b->attempts, 1u) << "existing key stats unaffected";
}

TEST(BoundedKeyspace, RegisterKeyAllOrNothingAndIdempotent) {
    FeedbackAggregatorConfig config;
    config.max_keys = 2;
    FeedbackAggregator aggregator(config);
    const FeedbackKey key_a{ExecutionBackend::DefaultAsync, "A",
                            QosClass::Standard};
    const FeedbackKey key_b{ExecutionBackend::DefaultAsync, "B",
                            QosClass::Standard};
    const FeedbackKey key_c{ExecutionBackend::DefaultAsync, "C",
                            QosClass::Standard};

    ASSERT_TRUE(aggregator.register_key(key_a));
    ASSERT_TRUE(aggregator.register_key(key_b));
    // 表满后注册第 3 键 → false。
    EXPECT_FALSE(aggregator.register_key(key_c));
    // 幂等：已存在的键重复注册返回 true 且不新建槽。
    EXPECT_TRUE(aggregator.register_key(key_a));
    EXPECT_TRUE(aggregator.register_key(key_a));
    EXPECT_TRUE(aggregator.register_key(key_b));

    // 失败的 register_key 不改变丢弃计数；预注册但未喂样的键（D4 契约：
    // entries 只含至少有一条样本的键）不得出现。
    const auto snapshot = aggregator.refresh();
    EXPECT_EQ(snapshot->dropped_samples, 0u);
    EXPECT_EQ(find_entry(*snapshot, key_a.backend, key_a.qos, "A"), nullptr)
        << "pre-registered key without samples must not appear in entries";
    EXPECT_EQ(find_entry(*snapshot, key_b.backend, key_b.qos, "B"), nullptr);
    // 首个样本到来后 entry 才出现。
    aggregator.record(make_feedback(QosClass::Standard, 1, 1, true, false,
                                    ExecutionBackend::DefaultAsync, "A"));
    const auto after_sample = aggregator.refresh();
    const auto* entry_a =
        find_entry(*after_sample, key_a.backend, key_a.qos, "A");
    ASSERT_NE(entry_a, nullptr);
    EXPECT_EQ(entry_a->attempts, 1u);
}

TEST(BoundedKeyspace, PreRegisteredKeyRecordsViaFastPathEquivalently) {
    FeedbackAggregatorConfig config;
    config.max_keys = 2;
    FeedbackAggregator aggregator(config);
    const FeedbackKey key{ExecutionBackend::Gpu, "gpu0", QosClass::Standard};
    ASSERT_TRUE(aggregator.register_key(key));

    for (int i = 0; i < 5; ++i) {
        aggregator.record(make_feedback(QosClass::Standard, 2'000, 3'000,
                                        true, false, ExecutionBackend::Gpu,
                                        "gpu0"));
    }
    const auto snapshot = aggregator.refresh();
    const auto* entry = find_entry(*snapshot, key.backend, key.qos, "gpu0");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->attempts, 5u);
    EXPECT_EQ(snapshot->dropped_samples, 0u);
    // 功能等价：EWMA/直方图与未预注册路径一致。5 条 2000ns 样本的手工推演：
    //   250 → 468 → 659 → 826 → 972（每步 + (2000-ewma)*125/1000 截断）。
    EXPECT_EQ(entry->ewma_queue_wait_ns, 972);
    EXPECT_EQ(histogram_total(entry->queue_wait), 5u);
}

// ---------------------------------------------------------------------------
// F. 分片并发
// ---------------------------------------------------------------------------

TEST(ShardConcurrency, EightThreadsSameKeyNoLostUpdates) {
    FeedbackAggregator aggregator;
    constexpr int kThreads = 8;
    constexpr int kPerThread = 4'000;
    std::vector<std::thread> workers;
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&aggregator, t] {
            const bool success = (t % 2 == 0);
            const bool missed = (t % 4 == 0);
            for (int i = 0; i < kPerThread; ++i) {
                aggregator.record(
                    make_feedback(QosClass::Standard, 100, 200, success,
                                  missed));
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }

    const auto snapshot = aggregator.refresh();
    ASSERT_EQ(snapshot->entries.size(), 1u);
    const FeedbackEntry& entry = snapshot->entries[0];
    const uint64_t expected_total =
        static_cast<uint64_t>(kThreads) * kPerThread;
    EXPECT_EQ(entry.attempts, expected_total)
        << "no lost updates across shards";
    EXPECT_EQ(entry.failures, expected_total / 2);
    EXPECT_EQ(entry.deadline_misses, expected_total / 4);
    EXPECT_EQ(snapshot->total_attempts, expected_total);
    EXPECT_EQ(snapshot->total_failures, expected_total / 2);
    EXPECT_EQ(histogram_total(entry.queue_wait), expected_total);
    EXPECT_EQ(histogram_total(entry.execution_duration), expected_total);
}

TEST(ShardConcurrency, FourThreadsDistinctKeysPerThread) {
    FeedbackAggregator aggregator;
    constexpr int kThreads = 4;
    constexpr int kPerThread = 3'000;
    std::vector<std::thread> workers;
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&aggregator, t] {
            const auto qos = static_cast<QosClass>(t);
            for (int i = 0; i < kPerThread; ++i) {
                aggregator.record(make_feedback(qos, 100, 200));
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }

    const auto snapshot = aggregator.refresh();
    ASSERT_EQ(snapshot->entries.size(), 4u);
    for (int t = 0; t < kThreads; ++t) {
        const auto qos = static_cast<QosClass>(t);
        const auto* entry = find_entry(*snapshot, ExecutionBackend::DefaultAsync,
                                       qos, "default");
        ASSERT_NE(entry, nullptr) << "qos index " << t;
        EXPECT_EQ(entry->attempts, static_cast<uint64_t>(kPerThread))
            << "qos index " << t;
        EXPECT_EQ(histogram_total(entry->queue_wait),
                  static_cast<uint64_t>(kPerThread));
    }
    EXPECT_EQ(snapshot->total_attempts,
              static_cast<uint64_t>(kThreads) * kPerThread);
}

// ---------------------------------------------------------------------------
// G. RCU 快照
// ---------------------------------------------------------------------------

TEST(RcuSnapshotLifecycle, SnapshotNeverEmptyFromConstruction) {
    FeedbackAggregator aggregator;
    const auto snapshot = aggregator.snapshot();
    ASSERT_NE(snapshot, nullptr) << "snapshot() must never return null";
    EXPECT_GE(snapshot->merge_count, 1u)
        << "constructor publishes an initial (empty) merge";
    EXPECT_TRUE(snapshot->entries.empty());
    EXPECT_EQ(snapshot->total_attempts, 0u);
}

TEST(RcuSnapshotLifecycle, RefreshPublishesNewImmutableSnapshot) {
    FeedbackAggregator aggregator;
    const auto first = aggregator.snapshot();
    ASSERT_EQ(first->merge_count, 1u);

    aggregator.record(make_feedback());
    const auto second = aggregator.refresh();
    EXPECT_NE(first.get(), second.get())
        << "refresh must publish a fresh snapshot object";
    EXPECT_EQ(second->merge_count, 2u)
        << "merge_count increments monotonically";
    EXPECT_EQ(second->total_attempts, 1u);

    // 旧 shared_ptr 在新快照发布后仍持有旧数据（不可变性）。
    EXPECT_EQ(first->merge_count, 1u);
    EXPECT_EQ(first->total_attempts, 0u);
    EXPECT_TRUE(first->entries.empty());

    // 无新样本的重复 refresh 仍发布新对象（全量重算）且计数单调。
    const auto third = aggregator.refresh();
    EXPECT_NE(second.get(), third.get());
    EXPECT_EQ(third->merge_count, 3u);
    EXPECT_EQ(third->total_attempts, 1u);
    EXPECT_EQ(second->merge_count, 2u) << "older snapshots stay frozen";
}

TEST(RcuSnapshotLifecycle, RefreshIfStaleHonorsMergeInterval) {
    FeedbackAggregatorConfig config;
    config.merge_interval = std::chrono::hours(1);  // 测试期内永不"过期"
    FeedbackAggregator aggregator(config);

    aggregator.record(make_feedback());
    const auto before = aggregator.refresh();
    EXPECT_EQ(before->merge_count, 2u);

    // 未到 merge_interval：返回当前快照，merge_count 不变。
    const auto cached = aggregator.refresh_if_stale();
    EXPECT_EQ(cached->merge_count, 2u);
    EXPECT_EQ(cached.get(), before.get());

    // interval=0：每次调用都重合并。
    FeedbackAggregatorConfig always;
    always.merge_interval = std::chrono::milliseconds{0};
    FeedbackAggregator eager(always);
    const auto a = eager.refresh_if_stale();
    const auto b = eager.refresh_if_stale();
    EXPECT_EQ(a->merge_count, 2u);
    EXPECT_EQ(b->merge_count, 3u)
        << "zero interval must re-merge on every call";

    // interval 到期后：下一次 refresh_if_stale 重合并。
    FeedbackAggregatorConfig short_interval;
    short_interval.merge_interval = std::chrono::milliseconds{50};
    FeedbackAggregator expiring(short_interval);
    EXPECT_EQ(expiring.refresh_if_stale()->merge_count, 1u);
    std::this_thread::sleep_for(std::chrono::milliseconds{120});
    EXPECT_EQ(expiring.refresh_if_stale()->merge_count, 2u)
        << "stale aggregator must re-merge after the interval elapses";
}

// ---------------------------------------------------------------------------
// H. record 契约
// ---------------------------------------------------------------------------

TEST(RecordContract, RecordIsNoexcept) {
    static_assert(noexcept(std::declval<FeedbackAggregator&>().record(
                      std::declval<const SchedulingFeedback&>())),
                  "record() must be noexcept (worker hot path)");
}

TEST(RecordContract, NegativeSamplesClampToZero) {
    FeedbackAggregator aggregator;
    // 负样本夹 0：落桶 0，EWMA 保持 0，不崩溃。
    aggregator.record(make_feedback(QosClass::Standard, -7, -9));
    aggregator.record(make_feedback(QosClass::Standard,
                                    std::numeric_limits<int64_t>::min(),
                                    std::numeric_limits<int64_t>::min()));
    const auto snapshot = aggregator.refresh();
    ASSERT_EQ(snapshot->entries.size(), 1u);
    const FeedbackEntry& entry = snapshot->entries[0];
    EXPECT_EQ(entry.attempts, 2u);
    EXPECT_EQ(entry.ewma_queue_wait_ns, 0);
    EXPECT_EQ(entry.ewma_execution_duration_ns, 0);
    ASSERT_FALSE(entry.queue_wait.buckets.empty());
    EXPECT_EQ(entry.queue_wait.buckets[0].count, 2u);
    ASSERT_FALSE(entry.execution_duration.buckets.empty());
    EXPECT_EQ(entry.execution_duration.buckets[0].count, 2u);
}

TEST(RecordContract, OversizedSamplesClampToMaxAndOverflowBucket) {
    FeedbackAggregator aggregator;
    const int64_t max_sample = FeedbackAggregator::kMaxSampleNs;  // 1e15
    aggregator.record(make_feedback(QosClass::Standard, max_sample,
                                    std::numeric_limits<int64_t>::max()));
    const auto snapshot = aggregator.refresh();
    ASSERT_EQ(snapshot->entries.size(), 1u);
    const FeedbackEntry& entry = snapshot->entries[0];
    // 溢出桶（默认末界 1e10 < 1e15）。
    ASSERT_EQ(entry.queue_wait.buckets.size(), 9u);
    EXPECT_EQ(entry.queue_wait.buckets.back().count, 1u);
    EXPECT_EQ(entry.queue_wait.buckets.back().upper_bound_ns, kInt64Max);
    ASSERT_EQ(entry.execution_duration.buckets.size(), 9u);
    EXPECT_EQ(entry.execution_duration.buckets.back().count, 1u);
    // EWMA 有限且为整数定点精确值：0 + 1e15 * 125 / 1000 = 1.25e14。
    EXPECT_EQ(entry.ewma_queue_wait_ns, 125'000'000'000'000);
    EXPECT_EQ(entry.ewma_execution_duration_ns, 125'000'000'000'000);
}

// ---------------------------------------------------------------------------
// I. 配置归一
// ---------------------------------------------------------------------------

TEST(ConfigNormalization, MaxKeysAndAlphaClamped) {
    FeedbackAggregatorConfig zero_keys;
    zero_keys.max_keys = 0;
    FeedbackAggregator zero_aggregator(zero_keys);
    EXPECT_EQ(zero_aggregator.config().max_keys, 1u)
        << "max_keys=0 must clamp to 1";
    // 夹取后键表只有 1 槽：第 2 键丢弃。
    zero_aggregator.record(make_feedback(QosClass::BestEffort, 1, 1));
    zero_aggregator.record(make_feedback(QosClass::Critical, 1, 1));
    const auto snapshot = zero_aggregator.refresh();
    EXPECT_EQ(snapshot->dropped_samples, 1u);
    EXPECT_EQ(snapshot->entries.size(), 1u);

    FeedbackAggregatorConfig low_alpha;
    low_alpha.ewma_alpha_permille = 0;
    FeedbackAggregator low(low_alpha);
    EXPECT_EQ(low.config().ewma_alpha_permille, 1u);

    FeedbackAggregatorConfig high_alpha;
    high_alpha.ewma_alpha_permille = 1001;
    FeedbackAggregator high(high_alpha);
    EXPECT_EQ(high.config().ewma_alpha_permille, 1000u);
    // alpha=1000 → ewma 精确跟随最后一个样本。
    high.record(make_feedback(QosClass::Standard, 500, 0));
    high.record(make_feedback(QosClass::Standard, 1'500, 0));
    const auto high_snapshot = high.refresh();
    ASSERT_EQ(high_snapshot->entries.size(), 1u);
    EXPECT_EQ(high_snapshot->entries[0].ewma_queue_wait_ns, 1'500);
}

TEST(ConfigNormalization, NonAscendingBoundsFallBackToDefaults) {
    FeedbackAggregatorConfig config;
    config.queue_wait_bucket_bounds_ns = {5'000, 3'000};  // 非升序
    config.execution_bucket_bounds_ns = {700};            // 不足 2 个
    FeedbackAggregator aggregator(config);

    // D6：config() 返回生效（归一化回退）配置，而非原始输入。
    ASSERT_EQ(aggregator.config().queue_wait_bucket_bounds_ns.size(), 8u);
    ASSERT_EQ(aggregator.config().execution_bucket_bounds_ns.size(), 8u);
    for (size_t b = 0; b < 8; ++b) {
        EXPECT_EQ(aggregator.config().queue_wait_bucket_bounds_ns[b],
                  kDefaultBounds[b])
            << "config() queue_wait bound #" << b;
        EXPECT_EQ(aggregator.config().execution_bucket_bounds_ns[b],
                  kDefaultBounds[b])
            << "config() execution bound #" << b;
    }

    aggregator.record(make_feedback(QosClass::Standard, 500, 700));
    const auto snapshot = aggregator.refresh();
    ASSERT_EQ(snapshot->entries.size(), 1u);
    // 两个维度都回退默认 8 界（+溢出桶共 9 桶）。
    ASSERT_EQ(snapshot->entries[0].queue_wait.buckets.size(), 9u);
    ASSERT_EQ(snapshot->entries[0].execution_duration.buckets.size(), 9u);
    for (size_t b = 0; b < 8; ++b) {
        EXPECT_EQ(snapshot->entries[0].queue_wait.buckets[b].upper_bound_ns,
                  kDefaultBounds[b])
            << "queue_wait bound #" << b;
        EXPECT_EQ(
            snapshot->entries[0].execution_duration.buckets[b].upper_bound_ns,
            kDefaultBounds[b])
            << "execution bound #" << b;
    }
    // 默认界下落桶：500 ≤ 1000 → 桶0；700 ≤ 1000 → 桶0。
    EXPECT_EQ(snapshot->entries[0].queue_wait.buckets[0].count, 1u);
    EXPECT_EQ(snapshot->entries[0].execution_duration.buckets[0].count, 1u);
}

TEST(ConfigNormalization, MoreThanFifteenBoundsTruncated) {
    FeedbackAggregatorConfig config;
    std::vector<int64_t> bounds;
    for (int i = 1; i <= 16; ++i) {
        bounds.push_back(static_cast<int64_t>(i) * 1'000);  // 1k..16k 升序
    }
    config.queue_wait_bucket_bounds_ns = bounds;
    FeedbackAggregator aggregator(config);

    // D6：config() 反映截断后的生效界（16 → 15）。
    ASSERT_EQ(aggregator.config().queue_wait_bucket_bounds_ns.size(), 15u);
    EXPECT_EQ(aggregator.config().queue_wait_bucket_bounds_ns[14], 15'000);
    ASSERT_EQ(aggregator.config().execution_bucket_bounds_ns.size(), 8u)
        << "untouched dimension keeps its default bounds";

    aggregator.record(make_feedback(QosClass::Standard, 15'000, 1));
    aggregator.record(make_feedback(QosClass::Standard, 16'000, 1));
    const auto snapshot = aggregator.refresh();
    ASSERT_EQ(snapshot->entries.size(), 1u);
    // 16 界截断为 15 → 15 + 溢出桶 = 16 桶（kMaxHistogramBuckets）。
    ASSERT_EQ(snapshot->entries[0].queue_wait.buckets.size(),
              FeedbackAggregator::kMaxHistogramBuckets);
    EXPECT_EQ(snapshot->entries[0].queue_wait.buckets[14].upper_bound_ns,
              15'000)
        << "the 15th (last kept) bound must survive truncation";
    // 15000 ≤ bounds[14] → 桶14；16000 > bounds[14] → 溢出桶。
    EXPECT_EQ(snapshot->entries[0].queue_wait.buckets[14].count, 1u);
    EXPECT_EQ(snapshot->entries[0].queue_wait.buckets[15].count, 1u);
}

// ---------------------------------------------------------------------------
// J. format_text 行式输出
// ---------------------------------------------------------------------------

TEST(FeedbackTextFormat, KeyEqualsValueLinesForSnapshot) {
    FeedbackAggregatorConfig config;
    config.queue_wait_bucket_bounds_ns = {1'000, 10'000};
    config.execution_bucket_bounds_ns = {1'000, 10'000};
    FeedbackAggregator aggregator(config);

    aggregator.record(make_feedback(QosClass::Standard, 500, 2'000, true));
    aggregator.record(make_feedback(QosClass::Standard, 500, 2'000, true));
    aggregator.record(make_feedback(QosClass::Standard, 500, 2'000, false));
    aggregator.record(make_feedback(QosClass::Standard, 500, 2'000, true,
                                    true));
    const auto snapshot = aggregator.refresh();
    const std::string text = FeedbackAggregator::format_text(*snapshot);

    // 构造发布 merge_count=1，此处显式 refresh 后为 2。
    EXPECT_NE(text.find("scheduling_feedback.merge_count=2\n"), std::string::npos);
    EXPECT_NE(text.find("scheduling_feedback.dropped_samples=0\n"),
              std::string::npos);
    EXPECT_NE(text.find("scheduling_feedback.total_attempts=4\n"),
              std::string::npos);
    // total_failures 随快照文本输出（D2 回归守卫），失败率为 3 位小数。
    EXPECT_NE(text.find("scheduling_feedback.total_failures=1\n"),
              std::string::npos);
    EXPECT_NE(text.find("scheduling_feedback.entries.count=1\n"),
              std::string::npos);
    EXPECT_NE(text.find("scheduling_feedback.entries[0].backend=DefaultAsync\n"),
              std::string::npos);
    EXPECT_NE(
        text.find("scheduling_feedback.entries[0].executor_name=default\n"),
        std::string::npos);
    EXPECT_NE(text.find("scheduling_feedback.entries[0].qos=Standard\n"),
              std::string::npos);
    EXPECT_NE(text.find("scheduling_feedback.entries[0].attempts=4\n"),
              std::string::npos);
    EXPECT_NE(text.find("scheduling_feedback.entries[0].failures=1\n"),
              std::string::npos);
    EXPECT_NE(text.find("scheduling_feedback.entries[0].failure_rate=0.250\n"),
              std::string::npos)
        << "failure_rate must use 3 decimal places";
    EXPECT_NE(text.find("scheduling_feedback.entries[0].deadline_misses=1\n"),
              std::string::npos);
    // 4 条 500ns 样本的 EWMA 推演：62 → 116 → 164 → 206。
    EXPECT_NE(
        text.find("scheduling_feedback.entries[0].ewma_queue_wait_ns=206\n"),
        std::string::npos);
    // 非零桶才输出：queue_wait 全落桶0，桶1 不得出现。
    EXPECT_NE(
        text.find("scheduling_feedback.entries[0].queue_wait_hist[0]."
                  "upper_bound_ns=1000\n"),
        std::string::npos);
    EXPECT_NE(
        text.find("scheduling_feedback.entries[0].queue_wait_hist[0].count=4\n"),
        std::string::npos);
    EXPECT_EQ(text.find("queue_wait_hist[1]"), std::string::npos)
        << "zero-count buckets must not be emitted";
    EXPECT_NE(
        text.find("scheduling_feedback.entries[0].execution_hist[1].count=4\n"),
        std::string::npos);
    EXPECT_EQ(text.find("execution_hist[0]"), std::string::npos);
}

// ---------------------------------------------------------------------------
// K. Executor 集成
// ---------------------------------------------------------------------------

TEST(ExecutorFeedbackIntegration, InjectedSchedulerFeedsAggregator) {
    Executor executor;
    init_executor(executor, 4);
    executor.set_scheduler(std::make_unique<FeedbackRouteScheduler>());

    constexpr int kOkTasks = 6;
    constexpr int kThrowingTasks = 2;
    std::vector<std::future<void>> futures;
    for (int i = 0; i < kOkTasks; ++i) {
        futures.push_back(executor.submit_auto(kairo::task([] {})));
    }
    for (int i = 0; i < kThrowingTasks; ++i) {
        futures.push_back(executor.submit_auto(kairo::task([] {
            throw std::runtime_error("boom");
        })));
    }
    ASSERT_TRUE(executor.wait_for_completion_for(std::chrono::seconds(10)));
    // merge_interval 默认 100ms：等待过期，保证 get_feedback_snapshot 重合并。
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    const auto snapshot = executor.get_feedback_snapshot();
    ASSERT_EQ(snapshot.entries.size(), 1u)
        << "exactly one (DefaultAsync, default, Standard) entry expected";
    const FeedbackEntry& entry = snapshot.entries[0];
    EXPECT_EQ(entry.key.backend, ExecutionBackend::DefaultAsync);
    EXPECT_EQ(entry.key.executor_name, "default");
    EXPECT_EQ(entry.key.qos, QosClass::Standard);
    constexpr uint64_t kTotal = kOkTasks + kThrowingTasks;
    EXPECT_EQ(entry.attempts, kTotal);
    EXPECT_EQ(entry.failures, static_cast<uint64_t>(kThrowingTasks));
    EXPECT_EQ(entry.deadline_misses, 0u);
    EXPECT_NEAR(entry.failure_rate,
                static_cast<double>(kThrowingTasks) / static_cast<double>(kTotal),
                1e-9);
    EXPECT_EQ(histogram_total(entry.queue_wait), kTotal);
    EXPECT_EQ(histogram_total(entry.execution_duration), kTotal);

    // 与 0.6.1 feedback 计数一致。
    const auto metrics = executor.get_scheduling_metrics();
    EXPECT_EQ(metrics.feedback_reported_count, kTotal);
    EXPECT_EQ(snapshot.total_attempts, kTotal);
    EXPECT_EQ(snapshot.total_failures, static_cast<uint64_t>(kThrowingTasks))
        << "total_failures must accumulate (D2 regression guard)";

    // 快照文本：既有前缀契约 + scheduling_feedback 段。
    const std::string text = executor.get_snapshot_text();
    EXPECT_EQ(text.rfind("executor_snapshot\n", 0), 0)
        << "existing prefix contract must be preserved";
    EXPECT_NE(text.find("scheduling_feedback.entries.count=1\n"),
              std::string::npos);
    EXPECT_NE(text.find("scheduling_feedback.total_attempts=8\n"),
              std::string::npos);
    EXPECT_NE(text.find("scheduling_feedback.entries[0].failure_rate=0.250\n"),
              std::string::npos);
}

TEST(ExecutorFeedbackIntegration, DefaultSchedulerHasZeroFeedbackFlow) {
    // 不注入调度器（DefaultScheduler，wants_feedback=false）：测量包装不
    // 附加，聚合器零流入——默认路径零变化。
    Executor executor;
    init_executor(executor, 4);

    for (int i = 0; i < 5; ++i) {
        executor.submit_auto(kairo::task([] {}));
    }
    ASSERT_TRUE(executor.wait_for_completion_for(std::chrono::seconds(10)));

    const auto snapshot = executor.get_feedback_snapshot();
    EXPECT_TRUE(snapshot.entries.empty())
        << "no samples may flow in on the default scheduler path";
    EXPECT_EQ(snapshot.total_attempts, 0u);

    const auto metrics = executor.get_scheduling_metrics();
    EXPECT_EQ(metrics.feedback_reported_count, 0u);

    const std::string text = executor.get_snapshot_text();
    EXPECT_EQ(text.rfind("executor_snapshot\n", 0), 0);
    EXPECT_NE(text.find("scheduling_feedback.total_attempts=0\n"),
              std::string::npos);
    EXPECT_NE(text.find("scheduling_feedback.entries.count=0\n"),
              std::string::npos);
}
