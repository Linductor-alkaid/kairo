/**
 * 批量任务提交性能对比测试
 *
 * 对比：
 * 1. 循环调用 submit()（基线）
 * 2. 使用 submit_batch()（优化）
 */

#include <kairo/executor.hpp>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <vector>

using namespace kairo;
using namespace std::chrono;

struct BenchmarkResult {
    std::string test_name;
    int num_tasks;
    double duration_ms;
    double throughput;
    double speedup;
};

// 基线：循环调用 submit
BenchmarkResult benchmark_loop_submit(int num_tasks) {
    Executor executor;

    auto start = steady_clock::now();

    for (int i = 0; i < num_tasks; ++i) {
        executor.submit([]() {
            // 空任务
        });
    }

    auto end = steady_clock::now();
    double duration_ms = duration_cast<milliseconds>(end - start).count();

    std::this_thread::sleep_for(milliseconds(100));
    executor.shutdown();

    double throughput = num_tasks * 1000.0 / duration_ms;

    return {"Loop submit()", num_tasks, duration_ms, throughput, 1.0};
}

// 优化：批量提交
BenchmarkResult benchmark_batch_submit(int num_tasks, double baseline_throughput) {
    Executor executor;

    auto start = steady_clock::now();

    // 准备任务列表（包含在计时内，公平对比）
    std::vector<std::function<void()>> tasks;
    tasks.reserve(num_tasks);
    for (int i = 0; i < num_tasks; ++i) {
        tasks.push_back([]() {
            // 空任务
        });
    }

    auto futures = executor.submit_batch(tasks);

    auto end = steady_clock::now();
    double duration_ms = duration_cast<milliseconds>(end - start).count();

    std::this_thread::sleep_for(milliseconds(100));
    executor.shutdown();

    double throughput = num_tasks * 1000.0 / duration_ms;
    double speedup = throughput / baseline_throughput;

    return {"submit_batch()", num_tasks, duration_ms, throughput, speedup};
}

void print_result(const BenchmarkResult& result) {
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "\n" << result.test_name << "\n";
    std::cout << "  任务数: " << result.num_tasks << "\n";
    std::cout << "  耗时: " << result.duration_ms << " ms\n";
    std::cout << "  吞吐量: " << result.throughput << " tasks/sec\n";
    if (result.speedup > 1.0) {
        std::cout << "  加速比: " << result.speedup << "x\n";
    }
}

void print_separator() {
    std::cout << std::string(80, '=') << "\n";
}

int main() {
    std::cout << "\n";
    print_separator();
    std::cout << "批量任务提交性能对比测试\n";
    print_separator();

    const std::vector<int> task_counts = {1000, 5000, 10000, 50000};

    for (int num_tasks : task_counts) {
        std::cout << "\n测试规模: " << num_tasks << " 个任务\n";
        print_separator();

        // 基线测试
        auto baseline = benchmark_loop_submit(num_tasks);
        print_result(baseline);

        // 批量提交测试
        auto batch = benchmark_batch_submit(num_tasks, baseline.throughput);
        print_result(batch);

        std::cout << "\n性能对比: ";
        if (batch.speedup >= 1.0) {
            std::cout << "批量提交更快 (" << batch.speedup << "x)\n";
        } else {
            std::cout << "循环 submit 更快 (" << batch.speedup << "x)\n";
        }
    }

    std::cout << "\n";
    print_separator();
    std::cout << "测试完成\n";
    print_separator();

    return 0;
}
