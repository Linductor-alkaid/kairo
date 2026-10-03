#include <gtest/gtest.h>
#include "kairo/monitor/task_monitor.hpp"
#include "kairo/lockfree_task_executor.hpp"
#include <thread>
#include <chrono>
#include <atomic>

using namespace kairo;
using namespace kairo::monitor;

namespace {
struct ReservationHook {
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
};

void pause_reservation(void* context) {
    auto* hook = static_cast<ReservationHook*>(context);
    hook->entered.store(true, std::memory_order_release);
    while (!hook->release.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}
} // namespace

TEST(MonitoringSamplingTest, DefaultFullSampling) {
    TaskMonitor monitor;

    for (int i = 0; i < 100; ++i) {
        monitor.record_task_start("task_" + std::to_string(i), "test");
        monitor.record_task_complete("task_" + std::to_string(i), true, 1000);
    }

    auto stats = monitor.get_statistics("test");
    EXPECT_EQ(stats.total_count, 100);
    EXPECT_EQ(stats.success_count, 100);
}

TEST(MonitoringSamplingTest, OnePctSampling) {
    TaskMonitor monitor;
    monitor.set_sampling_rate(0.01);

    EXPECT_DOUBLE_EQ(monitor.get_sampling_rate(), 0.01);

    for (int i = 0; i < 10000; ++i) {
        monitor.record_task_start("task_" + std::to_string(i), "test");
        monitor.record_task_complete("task_" + std::to_string(i), true, 1000);
    }

    auto stats = monitor.get_statistics("test");
    EXPECT_GT(stats.total_count, 50);
    EXPECT_LT(stats.total_count, 150);
}

TEST(MonitoringSamplingTest, ZeroSampling) {
    TaskMonitor monitor;
    monitor.set_sampling_rate(0.0);

    for (int i = 0; i < 100; ++i) {
        monitor.record_task_start("task_" + std::to_string(i), "test");
        monitor.record_task_complete("task_" + std::to_string(i), true, 1000);
    }

    auto stats = monitor.get_statistics("test");
    EXPECT_EQ(stats.total_count, 0);
}

TEST(LockFreeQueueStatsTest, BasicStats) {
    LockFreeTaskExecutor executor(1024, 2, true);
    executor.start();

    std::atomic<int> counter{0};
    for (int i = 0; i < 100; ++i) {
        executor.push_task([&counter]() { counter++; });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto stats = executor.get_queue_stats();
    EXPECT_EQ(stats.total_pushes, 100);
    EXPECT_GE(stats.total_pops, 90);
    EXPECT_GE(stats.success_rate, 0.99);

    executor.stop();
}

TEST(LockFreeQueueStatsTest, BatchStats) {
    LockFreeTaskExecutor executor(1024, 2, true);
    executor.start();

    std::function<void()> tasks[50];
    for (int i = 0; i < 50; ++i) {
        tasks[i] = []() {};
    }

    size_t pushed;
    executor.push_tasks_batch(tasks, 50, pushed);
    EXPECT_EQ(pushed, 50);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto stats = executor.get_queue_stats();
    // P-260623-004 fixed push_tasks_batch to actually call queue_->push_batch()
    // (the previous implementation looped queue_->push() per item, which kept
    // batch_pushes at 0 and broke monitoring that keyed on it). With the fix:
    //   - batch_pushes increments by 1 per push_tasks_batch() call
    //   - total_pushes still reflects the number of wrappers handed to the queue
    //   - batch_pops is independent of push path; worker_thread uses pop_batch
    EXPECT_EQ(stats.total_pushes, 50);
    EXPECT_EQ(stats.batch_pushes, 1u);
    // worker_thread still uses pop_batch, so batch_pops > 0.
    EXPECT_GE(stats.batch_pops, 1);

    executor.stop();
}

TEST(LockFreeQueueStatsTest, ReservationCancellationAccounting) {
    LockFreeTaskExecutor executor(2, 2, true);
    ReservationHook hook;
    executor.set_before_publish_hook(pause_reservation, &hook);

    std::atomic<bool> submitted{true};
    std::thread producer([&]() {
        submitted.store(executor.push_task([]() {}), std::memory_order_release);
    });
    while (!hook.entered.load(std::memory_order_acquire)) std::this_thread::yield();

    auto reserved = executor.get_queue_stats();
    EXPECT_EQ(reserved.reserved_count, 1u);
    EXPECT_EQ(reserved.reservation_wait_yields, 64u);

    EXPECT_FALSE(executor.push_task([]() {}));
    auto rejected = executor.get_queue_stats();
    EXPECT_GE(rejected.queue_full_rejections, 1u);
    EXPECT_EQ(rejected.fail_reason, LockFreeTaskExecutor::QueueFailReason::QueueFull);

    ASSERT_TRUE(executor.start());
    while (executor.get_queue_stats().cancelled_reservation_count == 0) {
        std::this_thread::yield();
    }
    hook.release.store(true, std::memory_order_release);
    producer.join();

    auto resolved = executor.get_queue_stats();
    EXPECT_FALSE(submitted.load(std::memory_order_acquire));
    EXPECT_EQ(resolved.cancelled_reservation_count, 1u);
    EXPECT_EQ(resolved.reservation_count,
              resolved.total_pushes + resolved.cancelled_reservation_count);
    EXPECT_EQ(resolved.fail_reason,
              LockFreeTaskExecutor::QueueFailReason::ReservationCancelled);

    executor.stop();
}
