/**
 * 批量任务提交性能测试 - 真实场景
 *
 * 使用有实际工作负载的任务，更真实地测试批量提交的优势
 */

#include <kairo/executor.hpp>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <vector>
#include <atomic>

using namespace kairo;
using namespace std::chrono;

std::atomic<uint64_t> work_counter{0};

// 模拟实际工作
void do_work() {
    uint64_t sum = 0;
    for (int i = 0; i < 100; ++i) {
        sum += i;
    }
    work_counter.fetch_add(sum, std::memory_order_relaxed);
}

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
    work_counter.store(0);

    auto start = steady_clock::now();

    std::vector<std::future<void>> futures;
    for (int i = 0; i < num_tasks; ++i) {
        futures.push_back(executor.submit(do_work));
    }

    // 等待所有任务完成
    for (auto& f : futures) {
        f.wait();
    }

    auto end = steady_clock::now();
    double duration_ms = duration_cast<milliseconds>(end - start).count();

    executor.shutdown();

    double throughput = num_tasks * 1000.0 / duration_ms;

    return {"Loop submit()", num_tasks, duration_ms, throughput, 1.0};
}

// 优化：批量提交
BenchmarkResult benchmark_batch_submit(int num_tasks, double baseline_throughput) {
    Executor executor;
    work_counter.store(0);

    auto start = steady_clock::now();

    // 准备任务列表
    std::vector<std::function<void()>> tasks;
    tasks.reserve(num_tasks);
    for (int i = 0; i < num_tasks; ++i) {
        tasks.push_back(do_work);
    }

    auto futures = executor.submit_batch(tasks);

    // 等待所有任务完成
    for (auto& f : futures) {
        f.wait();
    }

    auto end = steady_clock::now();
    double duration_ms = duration_cast<milliseconds>(end - start).count();

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
    std::cout << "批量任务提交性能测试 - 真实工作负载\n";
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
    std::cout << "工作计数器: " << work_counter.load() << "\n";
    print_separator();

    return 0;
}
