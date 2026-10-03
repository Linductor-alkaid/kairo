/**
 * 批量任务提交示例
 *
 * 演示如何使用 submit_batch() 和 submit_batch_no_future() 高效提交大量任务
 */

#include <kairo/executor.hpp>
#include <iostream>
#include <vector>
#include <chrono>
#include <atomic>

using namespace kairo;
using namespace std::chrono;

// 模拟数据处理任务
void process_data(int id) {
    // 模拟一些计算
    volatile int sum = 0;
    for (int i = 0; i < 100; ++i) {
        sum += i;
    }
}

// 示例 1：基本批量提交（不需要 future）
void example_batch_no_future() {
    std::cout << "\n=== 示例 1：批量提交（无 future）===\n";

    Executor executor;
    std::atomic<int> completed{0};

    // 准备任务列表
    std::vector<std::function<void()>> tasks;
    tasks.reserve(1000);

    for (int i = 0; i < 1000; ++i) {
        tasks.push_back([i, &completed]() {
            process_data(i);
            completed.fetch_add(1, std::memory_order_relaxed);
        });
    }

    std::cout << "提交 1000 个任务...\n";
    auto start = steady_clock::now();

    // 批量提交（高性能，不返回 future）
    executor.submit_batch_no_future(tasks);

    auto end = steady_clock::now();
    auto duration_us = duration_cast<microseconds>(end - start).count();

    std::cout << "提交耗时: " << duration_us << " μs\n";

    // 等待任务完成
    (void)executor.wait_for_completion(std::chrono::seconds{300});

    std::cout << "已完成任务数: " << completed.load() << "\n";
}

// 示例 2：批量提交并等待完成（需要 future）
void example_batch_with_future() {
    std::cout << "\n=== 示例 2：批量提交（有 future）===\n";

    Executor executor;

    // 准备任务列表
    std::vector<std::function<void()>> tasks;
    tasks.reserve(500);

    for (int i = 0; i < 500; ++i) {
        tasks.push_back([i]() {
            process_data(i);
        });
    }

    std::cout << "提交 500 个任务...\n";
    auto start = steady_clock::now();

    // 批量提交并获取 future
    auto futures = executor.submit_batch(tasks);

    auto end = steady_clock::now();
    auto duration_us = duration_cast<microseconds>(end - start).count();

    std::cout << "提交耗时: " << duration_us << " μs\n";
    std::cout << "等待所有任务完成...\n";

    // 等待所有任务完成
    for (auto& f : futures) {
        f.wait();
    }

    std::cout << "所有任务已完成\n";
}

// 示例 3：性能对比（循环 submit vs 批量提交）
void example_performance_comparison() {
    std::cout << "\n=== 示例 3：性能对比 ===\n";

    const int NUM_TASKS = 1000;

    // 方法 1：循环 submit
    {
        Executor executor;
        std::atomic<int> counter{0};

        auto start = steady_clock::now();
        for (int i = 0; i < NUM_TASKS; ++i) {
            executor.submit([&counter]() {
                counter.fetch_add(1, std::memory_order_relaxed);
            });
        }
        auto end = steady_clock::now();

        (void)executor.wait_for_completion(std::chrono::seconds{300});

        auto duration_us = duration_cast<microseconds>(end - start).count();
        std::cout << "循环 submit:           " << duration_us << " μs\n";
    }

    // 方法 2：批量提交（无 future）
    {
        Executor executor;
        std::atomic<int> counter{0};

        std::vector<std::function<void()>> tasks;
        tasks.reserve(NUM_TASKS);
        for (int i = 0; i < NUM_TASKS; ++i) {
            tasks.push_back([&counter]() {
                counter.fetch_add(1, std::memory_order_relaxed);
            });
        }

        auto start = steady_clock::now();
        executor.submit_batch_no_future(tasks);
        auto end = steady_clock::now();

        (void)executor.wait_for_completion(std::chrono::seconds{300});

        auto duration_us = duration_cast<microseconds>(end - start).count();
        std::cout << "submit_batch_no_future: " << duration_us << " μs\n";
    }
}

// 示例 4：实际应用场景 - 批量数据处理
void example_batch_data_processing() {
    std::cout << "\n=== 示例 4：批量数据处理 ===\n";

    Executor executor;

    // 模拟大量数据
    std::vector<int> data(2000);
    for (int i = 0; i < 2000; ++i) {
        data[i] = i;
    }

    // 结果存储
    std::vector<int> results(2000);

    // 准备处理任务
    std::vector<std::function<void()>> tasks;
    tasks.reserve(data.size());

    for (size_t i = 0; i < data.size(); ++i) {
        tasks.push_back([i, &data, &results]() {
            // 模拟数据处理：平方运算
            results[i] = data[i] * data[i];
        });
    }

    std::cout << "批量处理 " << data.size() << " 个数据项...\n";
    auto start = steady_clock::now();

    executor.submit_batch_no_future(tasks);
    (void)executor.wait_for_completion(std::chrono::seconds{300});

    auto end = steady_clock::now();
    auto duration_ms = duration_cast<milliseconds>(end - start).count();

    std::cout << "处理完成，耗时: " << duration_ms << " ms\n";
    std::cout << "示例结果: results[10] = " << results[10] << " (期望: 100)\n";
}

// 示例 5：错误示范 - 多线程并发批量提交（不推荐）
void example_anti_pattern() {
    std::cout << "\n=== 示例 5：不推荐的用法（多线程并发批量提交）===\n";

    Executor executor;

    std::cout << "⚠️  多线程并发批量提交性能不佳，建议使用循环 submit()\n";
    std::cout << "原因：每个线程准备任务列表的开销会抵消批量提交的收益\n";

    // 不推荐的做法
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&executor, t]() {
            std::vector<std::function<void()>> tasks;
            for (int i = 0; i < 250; ++i) {
                tasks.push_back([t, i]() {
                    process_data(t * 250 + i);
                });
            }
            executor.submit_batch_no_future(tasks);
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    (void)executor.wait_for_completion(std::chrono::seconds{300});
    std::cout << "任务完成（但性能不如循环 submit）\n";
}

int main() {
    std::cout << "批量任务提交示例\n";
    std::cout << "==========================================\n";

    example_batch_no_future();
    example_batch_with_future();
    example_performance_comparison();
    example_batch_data_processing();
    example_anti_pattern();

    std::cout << "\n==========================================\n";
    std::cout << "所有示例完成\n\n";

    std::cout << "💡 使用建议：\n";
    std::cout << "  ✅ 单线程提交大量任务（500+）使用 submit_batch_no_future()\n";
    std::cout << "  ✅ 需要等待任务完成时使用 submit_batch()\n";
    std::cout << "  ⚠️  多线程并发提交建议使用循环 submit()\n";

    return 0;
}
