#include <gtest/gtest.h>
#include "kairo/lockfree_task_executor.hpp"
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <set>
#include <mutex>

using namespace kairo;

/**
 * @brief MPSC 并发安全性测试
 *
 * 测试多个生产者线程并发提交任务，验证：
 * 1. 所有任务都被正确执行
 * 2. 没有任务丢失
 * 3. 没有任务重复执行
 * 4. 线程安全性
 */
class MPSCConcurrencyTest : public ::testing::Test {
protected:
    void SetUp() override {
        executor_ = std::make_unique<LockFreeTaskExecutor>(4096);
        executor_->start();
    }

    void TearDown() override {
        executor_->stop();
        executor_.reset();
    }

    std::unique_ptr<LockFreeTaskExecutor> executor_;
};

// 测试：多生产者并发提交任务，验证所有任务都被执行
TEST_F(MPSCConcurrencyTest, MultipleProducersSingleConsumer) {
    const int num_producers = 8;
    const int tasks_per_producer = 1000;
    const int total_tasks = num_producers * tasks_per_producer;

    std::atomic<int> executed_count{0};
    std::mutex result_mutex;
    std::set<int> executed_ids;

    std::vector<std::thread> producers;

    // 启动多个生产者线程
    for (int p = 0; p < num_producers; ++p) {
        producers.emplace_back([&, p]() {
            for (int i = 0; i < tasks_per_producer; ++i) {
                int task_id = p * tasks_per_producer + i;

                // 重试直到成功提交。计数与集合必须处于同一同步域：先持锁
                // insert，再在临界区内递增计数（release）。主线程以 acquire
                // 读到 total 时，happens-after 所有 insert，随后无锁读取
                // executed_ids 才是无竞争的（此前计数先于 insert 递增，
                // 最后一个任务可能尚未 insert，主线程即无锁读集合）。
                while (!executor_->push_task([&, task_id]() {
                    std::lock_guard<std::mutex> lock(result_mutex);
                    executed_ids.insert(task_id);
                    executed_count.fetch_add(1, std::memory_order_release);
                })) {
                    std::this_thread::yield();
                }
            }
        });
    }

    // 等待所有生产者完成
    for (auto& t : producers) {
        t.join();
    }

    // 等待所有任务执行完成。acquire 读与任务内 insert 后的 release 递增
    // 配对，建立 happens-before（见上方 lambda 注释）。
    auto start = std::chrono::steady_clock::now();
    while (executed_count.load(std::memory_order_acquire) < total_tasks) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        // 超时检查（5秒）
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed > std::chrono::seconds(5)) {
            FAIL() << "Timeout waiting for tasks to complete. Executed: "
                   << executed_count.load() << "/" << total_tasks;
        }
    }

    // 验证结果
    EXPECT_EQ(executed_count.load(), total_tasks);
    EXPECT_EQ(executed_ids.size(), total_tasks);

    // 验证所有任务ID都存在
    for (int i = 0; i < total_tasks; ++i) {
        EXPECT_TRUE(executed_ids.count(i) > 0) << "Task " << i << " was not executed";
    }
}

// 测试：高竞争场景下的正确性
TEST_F(MPSCConcurrencyTest, HighContentionCorrectness) {
    const int num_producers = 16;
    const int tasks_per_producer = 500;
    const int total_tasks = num_producers * tasks_per_producer;

    std::atomic<int> sum{0};
    std::vector<std::thread> producers;

    // 每个生产者提交任务，任务内容是累加自己的ID
    for (int p = 0; p < num_producers; ++p) {
        producers.emplace_back([&, p]() {
            for (int i = 0; i < tasks_per_producer; ++i) {
                while (!executor_->push_task([&, p]() {
                    sum.fetch_add(p, std::memory_order_relaxed);
                })) {
                    std::this_thread::yield();
                }
            }
        });
    }

    for (auto& t : producers) {
        t.join();
    }

    // 等待所有任务完成
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // 验证总和是否正确
    // 期望值 = sum(0..15) * 500 = 120 * 500 = 60000
    int expected_sum = 0;
    for (int p = 0; p < num_producers; ++p) {
        expected_sum += p * tasks_per_producer;
    }

    EXPECT_EQ(sum.load(), expected_sum);
}

// 测试：队列满时的行为
TEST_F(MPSCConcurrencyTest, QueueFullBehavior) {
    // 创建小容量队列
    auto small_executor = std::make_unique<LockFreeTaskExecutor>(64);
    small_executor->start();

    std::atomic<bool> consumer_blocked{true};
    std::atomic<int> successful_pushes{0};
    std::atomic<int> failed_pushes{0};

    // 提交阻塞任务填满队列
    for (int i = 0; i < 64; ++i) {
        small_executor->push_task([&]() {
            while (consumer_blocked.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 多个生产者尝试提交任务
    const int num_producers = 4;
    const int attempts_per_producer = 100;
    std::vector<std::thread> producers;

    for (int p = 0; p < num_producers; ++p) {
        producers.emplace_back([&]() {
            for (int i = 0; i < attempts_per_producer; ++i) {
                if (small_executor->push_task([]() {})) {
                    successful_pushes.fetch_add(1);
                } else {
                    failed_pushes.fetch_add(1);
                }
                std::this_thread::yield();
            }
        });
    }

    for (auto& t : producers) {
        t.join();
    }

    // 应该有失败的提交
    EXPECT_GT(failed_pushes.load(), 0);

    // 解除阻塞
    consumer_blocked.store(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    small_executor->stop();
}

// 测试：数据竞争检测（使用 ThreadSanitizer 运行）
TEST_F(MPSCConcurrencyTest, DataRaceDetection) {
    const int num_producers = 8;
    const int tasks_per_producer = 1000;
    const int total_tasks = num_producers * tasks_per_producer;

    std::vector<int> shared_data(total_tasks, 0);
    std::atomic<int> index{0};
    // 完成计数：任务内写 shared_data 后 release 递增，主线程 acquire 读到
    // total 后再读 shared_data，否则固定 sleep 与任务尾部写入并发（本测试
    // 自身的竞争，会掩盖被测组件的真实问题）。
    std::atomic<int> completed{0};

    std::vector<std::thread> producers;

    for (int p = 0; p < num_producers; ++p) {
        producers.emplace_back([&]() {
            for (int i = 0; i < tasks_per_producer; ++i) {
                while (!executor_->push_task([&]() {
                    int idx = index.fetch_add(1, std::memory_order_relaxed);
                    if (idx < static_cast<int>(shared_data.size())) {
                        shared_data[idx] = 1;
                    }
                    completed.fetch_add(1, std::memory_order_release);
                })) {
                    std::this_thread::yield();
                }
            }
        });
    }

    for (auto& t : producers) {
        t.join();
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (completed.load(std::memory_order_acquire) < total_tasks) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        ASSERT_TRUE(std::chrono::steady_clock::now() < deadline)
            << "Timeout waiting for tasks. Completed: "
            << completed.load(std::memory_order_acquire) << "/" << total_tasks;
    }

    // 验证所有元素都被设置
    int count = 0;
    for (int val : shared_data) {
        if (val == 1) count++;
    }
    EXPECT_EQ(count, shared_data.size());
}

// 测试：生产者动态加入和退出
TEST_F(MPSCConcurrencyTest, DynamicProducers) {
    std::atomic<int> total_executed{0};
    std::atomic<bool> keep_running{true};

    auto producer_func = [&]() {
        int local_count = 0;
        while (keep_running.load() && local_count < 100) {
            if (executor_->push_task([&]() {
                total_executed.fetch_add(1);
            })) {
                local_count++;
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            } else {
                std::this_thread::yield();
            }
        }
    };

    // 启动初始生产者
    std::vector<std::thread> producers;
    for (int i = 0; i < 4; ++i) {
        producers.emplace_back(producer_func);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 动态添加更多生产者
    for (int i = 0; i < 4; ++i) {
        producers.emplace_back(producer_func);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 停止所有生产者
    keep_running.store(false);
    for (auto& t : producers) {
        t.join();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 验证有任务被执行
    EXPECT_GT(total_executed.load(), 0);
}

// 测试：极限压力测试
TEST_F(MPSCConcurrencyTest, StressTest) {
    const int num_producers = 32;
    const int duration_ms = 1000;

    std::atomic<uint64_t> total_pushed{0};
    std::atomic<uint64_t> total_executed{0};
    std::atomic<bool> keep_running{true};

    std::vector<std::thread> producers;

    for (int p = 0; p < num_producers; ++p) {
        producers.emplace_back([&]() {
            while (keep_running.load()) {
                if (executor_->push_task([&]() {
                    total_executed.fetch_add(1, std::memory_order_relaxed);
                })) {
                    total_pushed.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));
    keep_running.store(false);

    for (auto& t : producers) {
        t.join();
    }

    // 等待所有任务执行完成
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    uint64_t pushed = total_pushed.load();
    uint64_t executed = total_executed.load();

    EXPECT_EQ(pushed, executed);
    EXPECT_GT(pushed, 0);

    std::cout << "Stress test results:\n"
              << "  Producers: " << num_producers << "\n"
              << "  Duration: " << duration_ms << "ms\n"
              << "  Total tasks: " << pushed << "\n"
              << "  Throughput: " << (pushed * 1000 / duration_ms) << " tasks/sec\n";
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
