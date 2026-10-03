/**
 * 简化的批量提交性能测试
 * 对比：循环 submit vs submit_batch vs submit_batch_no_future
 */

#include <kairo/executor.hpp>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <atomic>

using namespace kairo;
using namespace std::chrono;

void test_loop_submit(int num_tasks) {
    Executor executor;
    std::atomic<int> counter{0};

    auto start = steady_clock::now();
    for (int i = 0; i < num_tasks; ++i) {
        executor.submit([&counter]() {
            counter.fetch_add(1, std::memory_order_relaxed);
        });
    }
    auto end = steady_clock::now();

    executor.wait_for_completion();
    executor.shutdown(false);

    auto duration_us = duration_cast<microseconds>(end - start).count();
    double throughput = num_tasks * 1000000.0 / duration_us;

    std::cout << "1. 循环 submit()\n";
    std::cout << "   耗时: " << duration_us << " μs\n";
    std::cout << "   吞吐量: " << std::fixed << std::setprecision(0)
              << throughput << " tasks/sec\n";
    std::cout << "   计数器: " << counter.load() << "\n\n";
}

void test_batch_with_future(int num_tasks) {
    Executor executor;
    std::atomic<int> counter{0};

    std::vector<std::function<void()>> tasks;
    tasks.reserve(num_tasks);
    for (int i = 0; i < num_tasks; ++i) {
        tasks.push_back([&counter]() {
            counter.fetch_add(1, std::memory_order_relaxed);
        });
    }

    auto start = steady_clock::now();
    auto futures = executor.submit_batch(tasks);
    auto end = steady_clock::now();

    executor.wait_for_completion();
    executor.shutdown(false);

    auto duration_us = duration_cast<microseconds>(end - start).count();
    double throughput = num_tasks * 1000000.0 / duration_us;

    std::cout << "2. submit_batch() [有 future]\n";
    std::cout << "   耗时: " << duration_us << " μs\n";
    std::cout << "   吞吐量: " << std::fixed << std::setprecision(0)
              << throughput << " tasks/sec\n";
    std::cout << "   计数器: " << counter.load() << "\n\n";
}

void test_batch_no_future(int num_tasks) {
    Executor executor;
    std::atomic<int> counter{0};

    std::vector<std::function<void()>> tasks;
    tasks.reserve(num_tasks);
    for (int i = 0; i < num_tasks; ++i) {
        tasks.push_back([&counter]() {
            counter.fetch_add(1, std::memory_order_relaxed);
        });
    }

    auto start = steady_clock::now();
    executor.submit_batch_no_future(tasks);
    auto end = steady_clock::now();

    executor.wait_for_completion();
    executor.shutdown(false);

    auto duration_us = duration_cast<microseconds>(end - start).count();
    double throughput = num_tasks * 1000000.0 / duration_us;

    std::cout << "3. submit_batch_no_future() [无 future]\n";
    std::cout << "   耗时: " << duration_us << " μs\n";
    std::cout << "   吞吐量: " << std::fixed << std::setprecision(0)
              << throughput << " tasks/sec\n";
    std::cout << "   计数器: " << counter.load() << "\n\n";
}

int main() {
    const int NUM_TASKS = 10000;

    std::cout << "\n批量提交性能对比（" << NUM_TASKS << " 个任务）\n";
    std::cout << std::string(60, '=') << "\n\n";

    test_loop_submit(NUM_TASKS);
    test_batch_with_future(NUM_TASKS);
    test_batch_no_future(NUM_TASKS);

    std::cout << std::string(60, '=') << "\n";
    std::cout << "测试完成\n\n";

    return 0;
}
