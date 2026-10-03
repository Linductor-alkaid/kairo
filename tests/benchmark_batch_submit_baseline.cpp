/**
 * 批量任务提交性能基准测试
 *
 * 目标：
 * 1. 建立当前单个任务提交的性能基线
 * 2. 为批量提交 API 优化提供对比数据
 */

#include <kairo/executor.hpp>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <vector>
#include <numeric>
#include <thread>

using namespace kairo;
using namespace std::chrono;

struct BenchmarkResult {
    std::string test_name;
    int num_tasks;
    double duration_ms;
    double throughput;
    double avg_latency_us;
};

// 基准测试：单个任务循环提交
BenchmarkResult benchmark_single_submit(int num_tasks) {
    Executor executor;
    // 懒启动：第一次 submit 会自动初始化

    auto start = steady_clock::now();

    for (int i = 0; i < num_tasks; ++i) {
        executor.submit([]() {
            // 空任务
        });
    }

    auto end = steady_clock::now();
    double duration_ms = duration_cast<milliseconds>(end - start).count();

    // 等待所有任务完成
    std::this_thread::sleep_for(milliseconds(100));
    executor.shutdown();

    double throughput = num_tasks * 1000.0 / duration_ms;
    double avg_latency_us = duration_ms * 1000.0 / num_tasks;

    return {"Single Submit (Loop)", num_tasks, duration_ms, throughput, avg_latency_us};
}

// 基准测试：多线程并发提交
BenchmarkResult benchmark_concurrent_submit(int num_tasks, int num_threads) {
    Executor executor;
    // 懒启动：第一次 submit 会自动初始化

    int tasks_per_thread = num_tasks / num_threads;
    std::vector<std::thread> threads;

    auto start = steady_clock::now();

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&executor, tasks_per_thread]() {
            for (int i = 0; i < tasks_per_thread; ++i) {
                executor.submit([]() {
                    // 空任务
                });
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    auto end = steady_clock::now();
    double duration_ms = duration_cast<milliseconds>(end - start).count();

    // 等待所有任务完成
    std::this_thread::sleep_for(milliseconds(100));
    executor.shutdown();

    double throughput = num_tasks * 1000.0 / duration_ms;
    double avg_latency_us = duration_ms * 1000.0 / num_tasks;

    return {"Concurrent Submit (" + std::to_string(num_threads) + " threads)",
            num_tasks, duration_ms, throughput, avg_latency_us};
}

void print_result(const BenchmarkResult& result) {
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "\n测试: " << result.test_name << "\n";
    std::cout << "  任务数: " << result.num_tasks << "\n";
    std::cout << "  耗时: " << result.duration_ms << " ms\n";
    std::cout << "  吞吐量: " << result.throughput << " tasks/sec\n";
    std::cout << "  平均延迟: " << result.avg_latency_us << " μs\n";
}

void print_separator() {
    std::cout << std::string(80, '=') << "\n";
}

int main() {
    std::cout << "\n";
    print_separator();
    std::cout << "批量任务提交性能基线测试\n";
    print_separator();

    const int num_tasks = 10000;

    std::cout << "\n1. 单线程提交基线\n";
    print_separator();
    auto result1 = benchmark_single_submit(num_tasks);
    print_result(result1);

    std::cout << "\n2. 多线程并发提交\n";
    print_separator();
    auto result2 = benchmark_concurrent_submit(num_tasks, 2);
    print_result(result2);

    auto result3 = benchmark_concurrent_submit(num_tasks, 4);
    print_result(result3);

    auto result4 = benchmark_concurrent_submit(num_tasks, 8);
    print_result(result4);

    std::cout << "\n";
    print_separator();
    std::cout << "性能基线总结\n";
    print_separator();
    std::cout << std::fixed << std::setprecision(0);
    std::cout << "单线程吞吐量: " << result1.throughput << " tasks/sec\n";
    std::cout << "多线程吞吐量 (4线程): " << result3.throughput << " tasks/sec\n";
    std::cout << "平均延迟: " << result1.avg_latency_us << " μs\n";
    std::cout << "\n说明：批量提交 API 不承诺固定加速比，请以当前 benchmark 结果为准\n";
    std::cout << "参考吞吐量: " << result1.throughput << " tasks/sec\n";
    print_separator();

    return 0;
}
