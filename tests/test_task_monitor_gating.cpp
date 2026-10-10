// CR-071/CR-163 分片化 + 热路径快速门的语义锁定测试。
//
// 快速门前提（task_monitor.cpp）：统计采样率 0 时 set_sampling_rate 清空
// task_id→type 映射；in-flight 采样率 0 或容量 0 时对应配置变更清空
// in-flight 快照。本文件锁定这些语义变化与既有契约：
//  - 采样率 0 → 不产生统计，get_all_statistics 为空；恢复 1.0 后正常；
//  - in-flight 采样率 0 → 快照清空且后续 queued 不再入快照；恢复 1.0 正常；
//  - in-flight 容量 0 → 快照空且后续 queued 被准入门丢弃；
//  - CR-075：缩容驱逐计 evicted，不计 dropped（incomplete 只反映准入丢弃）；
//  - 全采样并发下聚合精确（total 按 type 精确、success+fail=total、
//    结束后 in-flight 计数归零）。
#include <gtest/gtest.h>
#include "kairo/monitor/task_monitor.hpp"
#include <string>
#include <thread>
#include <vector>

using namespace kairo;
using namespace kairo::monitor;

namespace {
std::string task_id(int thread, int index) {
    return "w" + std::to_string(thread) + "_" + std::to_string(index);
}
}  // namespace

TEST(TaskMonitorGatingTest, StatsSamplingZeroProducesNoStatistics) {
    TaskMonitor monitor;
    monitor.set_sampling_rate(0.0);

    for (int i = 0; i < 100; ++i) {
        const std::string id = task_id(0, i);
        monitor.record_task_start(id, "type_a");
        monitor.record_task_complete(id, true, 1000);
        if (i % 10 == 0) {
            monitor.record_task_timeout(id);
        }
    }

    const auto stats = monitor.get_statistics("type_a");
    EXPECT_EQ(stats.total_count, 0);
    EXPECT_EQ(stats.success_count, 0);
    EXPECT_EQ(stats.fail_count, 0);
    EXPECT_EQ(stats.timeout_count, 0);
    EXPECT_EQ(stats.total_execution_time_ns, 0);
    EXPECT_EQ(stats.max_execution_time_ns, 0);
    EXPECT_EQ(stats.min_execution_time_ns, 0);
    EXPECT_TRUE(monitor.get_all_statistics().empty());
}

TEST(TaskMonitorGatingTest, StatsSamplingZeroDropsUnsettledSamples) {
    TaskMonitor monitor;
    // 先全采样开始一个任务（id 进入映射，尚未结算），再关闭统计采样。
    monitor.record_task_start("pending", "type_a");
    monitor.set_sampling_rate(0.0);
    // 已开始未结算的样本随清空丢弃：complete 不产生统计（语义 =
    // "关闭统计采样"，CHANGELOG 0.7.0）。
    monitor.record_task_complete("pending", true, 100);
    monitor.record_task_timeout("pending");

    EXPECT_EQ(monitor.get_statistics("type_a").total_count, 0);
    EXPECT_TRUE(monitor.get_all_statistics().empty());
}

TEST(TaskMonitorGatingTest, StatsSamplingRestoreResumesStatistics) {
    TaskMonitor monitor;
    monitor.set_sampling_rate(0.0);
    monitor.record_task_start("gated", "type_a");
    monitor.record_task_complete("gated", true, 5);
    EXPECT_EQ(monitor.get_statistics("type_a").total_count, 0);

    monitor.set_sampling_rate(1.0);
    EXPECT_DOUBLE_EQ(monitor.get_sampling_rate(), 1.0);

    monitor.record_task_start("live", "type_a");
    monitor.record_task_complete("live", true, 7);
    monitor.record_task_start("failed", "type_b");
    monitor.record_task_complete("failed", false, 11);

    const auto a = monitor.get_statistics("type_a");
    EXPECT_EQ(a.total_count, 1);
    EXPECT_EQ(a.success_count, 1);
    EXPECT_EQ(a.fail_count, 0);
    EXPECT_EQ(a.total_execution_time_ns, 7);

    const auto b = monitor.get_statistics("type_b");
    EXPECT_EQ(b.total_count, 1);
    EXPECT_EQ(b.fail_count, 1);
    EXPECT_EQ(b.success_count, 0);

    auto all = monitor.get_all_statistics();
    EXPECT_EQ(all.size(), 2u);
    EXPECT_NE(all.find("type_a"), all.end());
    EXPECT_NE(all.find("type_b"), all.end());
}

TEST(TaskMonitorGatingTest, InFlightSamplingZeroClearsSnapshot) {
    TaskMonitor monitor;
    monitor.record_task_queued("a", "type_a", "default");
    monitor.record_task_queued("b", "type_a", "default");
    ASSERT_EQ(monitor.get_in_flight_count(), 2u);
    ASSERT_EQ(monitor.get_in_flight_tasks().size(), 2u);

    monitor.set_in_flight_sampling_rate(0.0);
    EXPECT_EQ(monitor.get_in_flight_count(), 0u);
    EXPECT_TRUE(monitor.get_in_flight_tasks().empty());

    // 门关闭后 queued 走无锁早退，不再进入快照。
    monitor.record_task_queued("c", "type_a", "default");
    monitor.record_task_pending("d", "type_a", "default");
    EXPECT_EQ(monitor.get_in_flight_count(), 0u);
    EXPECT_TRUE(monitor.get_in_flight_tasks().empty());

    monitor.set_in_flight_sampling_rate(1.0);
    EXPECT_DOUBLE_EQ(monitor.get_in_flight_sampling_rate(), 1.0);
    monitor.record_task_queued("e", "type_a", "default");
    EXPECT_EQ(monitor.get_in_flight_count(), 1u);
    const auto tasks = monitor.get_in_flight_tasks();
    ASSERT_EQ(tasks.size(), 1u);
    EXPECT_EQ(tasks[0].task_id, "e");
    EXPECT_EQ(tasks[0].state, TaskLifecycleState::Queued);
}

TEST(TaskMonitorGatingTest, InFlightCapacityZeroDisablesRetention) {
    TaskMonitor monitor;
    monitor.record_task_queued("a", "type_a", "default");
    ASSERT_EQ(monitor.get_in_flight_count(), 1u);

    monitor.set_in_flight_capacity(0);
    EXPECT_EQ(monitor.get_in_flight_capacity(), 0u);
    EXPECT_EQ(monitor.get_in_flight_count(), 0u);
    EXPECT_TRUE(monitor.get_in_flight_tasks().empty());

    // 容量 0 时 queued 走准入早退，不入快照。
    monitor.record_task_queued("b", "type_a", "default");
    EXPECT_EQ(monitor.get_in_flight_count(), 0u);
    EXPECT_TRUE(monitor.get_in_flight_tasks().empty());
}

TEST(TaskMonitorGatingTest, ShrinkEvictsAndCountsEvictedNotDropped) {
    TaskMonitor monitor;
    monitor.set_in_flight_capacity(128);
    for (int i = 0; i < 32; ++i) {
        monitor.record_task_queued(task_id(0, i), "type_a", "default");
    }
    ASSERT_EQ(monitor.get_in_flight_count(), 32u);
    ASSERT_EQ(monitor.get_in_flight_dropped_count(), 0u);

    monitor.set_in_flight_capacity(8);
    EXPECT_EQ(monitor.get_in_flight_count(), 8u);
    EXPECT_EQ(monitor.get_in_flight_capacity(), 8u);
    // CR-075：缩容驱逐单独计数，不与运行期准入丢弃混淆。
    EXPECT_EQ(monitor.get_in_flight_evicted_count(), 24u);
    EXPECT_EQ(monitor.get_in_flight_dropped_count(), 0u);
    EXPECT_FALSE(monitor.in_flight_diagnostics_incomplete());
    EXPECT_EQ(monitor.get_in_flight_diagnostics().tasks.size(), 8u);
}

TEST(TaskMonitorGatingTest, RuntimeAdmissionDropKeepsEvictedUntouched) {
    TaskMonitor monitor;
    monitor.set_in_flight_capacity(4);
    for (int i = 0; i < 10; ++i) {
        monitor.record_task_queued(task_id(0, i), "type_a", "default");
    }
    EXPECT_EQ(monitor.get_in_flight_count(), 4u);
    EXPECT_EQ(monitor.get_in_flight_dropped_count(), 6u);
    EXPECT_EQ(monitor.get_in_flight_evicted_count(), 0u);
    EXPECT_TRUE(monitor.in_flight_diagnostics_incomplete());
}

TEST(TaskMonitorGatingTest, ConcurrentFullSamplingExactAggregates) {
    TaskMonitor monitor;
    constexpr int kThreads = 8;
    constexpr int kTasksPerThread = 5000;
    constexpr int kTypeCount = 4;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int w = 0; w < kThreads; ++w) {
        threads.emplace_back([&monitor, w]() {
            const std::string type =
                "type_" + std::to_string(w % kTypeCount);
            for (int i = 0; i < kTasksPerThread; ++i) {
                const std::string id = task_id(w, i);
                monitor.record_task_start(id, type);
                monitor.record_task_complete(id, (i % 2) == 0, 100 + i);
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }

    auto all = monitor.get_all_statistics();
    ASSERT_EQ(all.size(), static_cast<size_t>(kTypeCount));
    int64_t grand_total = 0;
    for (int t = 0; t < kTypeCount; ++t) {
        // 使用该 type 的线程恰好为 w ≡ t (mod 4) 的两个线程。
        const std::string type = "type_" + std::to_string(t);
        const auto stats = monitor.get_statistics(type);
        EXPECT_EQ(stats.total_count, 2 * kTasksPerThread) << type;
        EXPECT_EQ(stats.success_count, kTasksPerThread) << type;
        EXPECT_EQ(stats.fail_count, kTasksPerThread) << type;
        EXPECT_EQ(stats.success_count + stats.fail_count, stats.total_count)
            << type;
        grand_total += stats.total_count;
    }
    EXPECT_EQ(grand_total, static_cast<int64_t>(kThreads) * kTasksPerThread);
    EXPECT_EQ(monitor.get_in_flight_count(), 0u);
}
