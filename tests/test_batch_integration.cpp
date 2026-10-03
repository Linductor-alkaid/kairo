/**
 * 批量操作集成测试
 */

#include "kairo/lockfree_task_executor.hpp"
#include <gtest/gtest.h>
#include <atomic>
#include <vector>

using namespace kairo;

TEST(BatchIntegration, PushTasksBatch) {
    LockFreeTaskExecutor executor(1024);
    ASSERT_TRUE(executor.start());

    std::atomic<int> counter{0};
    const size_t batch_size = 100;

    std::vector<std::function<void()>> tasks(batch_size);
    for (size_t i = 0; i < batch_size; ++i) {
        tasks[i] = [&counter]() { counter.fetch_add(1); };
    }

    size_t pushed;
    ASSERT_TRUE(executor.push_tasks_batch(tasks.data(), batch_size, pushed));
    EXPECT_EQ(pushed, batch_size);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(counter.load(), batch_size);
    EXPECT_EQ(executor.processed_count(), batch_size);
}

TEST(BatchIntegration, PushBatchRejectsWhenInsufficientCapacity) {
    LockFreeTaskExecutor executor(16);

    std::vector<std::function<void()>> prefill(5);
    for (auto& task : prefill) {
        task = []() {};
    }

    size_t pushed = 0;
    ASSERT_TRUE(executor.push_tasks_batch(prefill.data(), prefill.size(), pushed));
    ASSERT_EQ(pushed, prefill.size());

    std::vector<std::function<void()>> tasks(11);
    for (auto& task : tasks) {
        task = []() {};
    }

    pushed = 0;
    EXPECT_FALSE(executor.push_tasks_batch(tasks.data(), tasks.size(), pushed));
    EXPECT_EQ(pushed, 0u);
    EXPECT_EQ(executor.pending_count(), prefill.size());
}

TEST(BatchIntegration, WorkerUsesBatchPop) {
    LockFreeTaskExecutor executor(1024);
    ASSERT_TRUE(executor.start());

    std::atomic<int> counter{0};
    const size_t total_tasks = 1000;

    for (size_t i = 0; i < total_tasks; ++i) {
        ASSERT_TRUE(executor.push_task([&counter]() { counter.fetch_add(1); }));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(counter.load(), total_tasks);
    EXPECT_EQ(executor.processed_count(), total_tasks);
}
