/**
 * 批量任务提交性能测试 - 高并发场景
 *
 * 测试场景：多个线程同时提交大量任务
 * 这是批量提交 API 真正发挥优势的场景
 */

#include <kairo/executor.hpp>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <vector>
#include <thread>
#include <atomic>

using namespace kairo;
using namespace std::chrono;

struct BenchmarkResult {
    std::string test_name;
    int num_threads;
    int tasks_per_thread;
    int total_tasks;
    double duration_ms;
    double throughput;
    double speedup;
};

// 基线：多线程循环调用 submit
BenchmarkResult benchmark_concurrent_loop_submit(int num_threads, int tasks_per_thread) {
    Executor executor;
    std::vector<std::thread> threads;
    std::atomic<int> ready_count{0};
    std::atomic<bool> start_flag{false};

    auto start_time = steady_clock::now();

    // 创建线程，等待同步启动
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, tasks_per_thread]() {
            ready_count.fetch_add(1);
            // 等待所有线程就绪
            while (!start_flag.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            // 循环提交任务
            std::vector<std::future<void>> futures;
            for (int i = 0; i < tasks_per_thread; ++i) {
                futures.push_back(executor.submit([]() {
                    // 空任务
                }));
            }

            // 等待任务完成
            for (auto& f : futures) {
                f.wait();
            }
        });
    }

    // 等待所有线程就绪
    while (ready_count.load() < num_threads) {
        std::this_thread::yield();
    }

    // 同步启动所有线程
    start_flag.store(true, std::memory_order_release);

    // 等待所有线程完成
    for (auto& t : threads) {
        t.join();
    }

    auto end_time = steady_clock::now();
    double duration_ms = duration_cast<milliseconds>(end_time - start_time).count();

    executor.shutdown();

    int total_tasks = num_threads * tasks_per_thread;
    double throughput = total_tasks * 1000.0 / duration_ms;

    return {"Concurrent Loop submit()", num_threads, tasks_per_thread,
            total_tasks, duration_ms, throughput, 1.0};
}

// 优化：多线程批量提交
BenchmarkResult benchmark_concurrent_batch_submit(int num_threads, int tasks_per_thread,
                                                   double baseline_throughput) {
    Executor executor;
    std::vector<std::thread> threads;
    std::atomic<int> ready_count{0};
    std::atomic<bool> start_flag{false};

    auto start_time = steady_clock::now();

    // 创建线程，等待同步启动
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, tasks_per_thread]() {
            ready_count.fetch_add(1);
            // 等待所有线程就绪
            while (!start_flag.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            // 准备任务列表
            std::vector<std::function<void()>> tasks;
            tasks.reserve(tasks_per_thread);
            for (int i = 0; i < tasks_per_thread; ++i) {
                tasks.push_back([]() {
                    // 空任务
                });
            }

            // 批量提交
            auto futures = executor.submit_batch(tasks);

            // 等待任务完成
            for (auto& f : futures) {
                f.wait();
            }
        });
    }

    // 等待所有线程就绪
    while (ready_count.load() < num_threads) {
        std::this_thread::yield();
    }

    // 同步启动所有线程
    start_flag.store(true, std::memory_order_release);

    // 等待所有线程完成
    for (auto& t : threads) {
        t.join();
    }

    auto end_time = steady_clock::now();
    double duration_ms = duration_cast<milliseconds>(end_time - start_time).count();

    executor.shutdown();

    int total_tasks = num_threads * tasks_per_thread;
    double throughput = total_tasks * 1000.0 / duration_ms;
    double speedup = throughput / baseline_throughput;

    return {"Concurrent submit_batch()", num_threads, tasks_per_thread,
            total_tasks, duration_ms, throughput, speedup};
}

void print_result(const BenchmarkResult& result) {
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "\n" << result.test_name << "\n";
    std::cout << "  线程数: " << result.num_threads << "\n";
    std::cout << "  每线程任务数: " << result.tasks_per_thread << "\n";
    std::cout << "  总任务数: " << result.total_tasks << "\n";
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
    std::cout << "批量任务提交性能测试 - 高并发场景\n";
    std::cout << "测试场景：多线程同时提交任务（锁竞争激烈）\n";
    print_separator();

    // 测试配置：线程数 x 每线程任务数
    const std::vector<std::pair<int, int>> test_configs = {
        {2, 5000},    // 2 线程，每线程 5000 任务
        {4, 2500},    // 4 线程，每线程 2500 任务
        {8, 1250},    // 8 线程，每线程 1250 任务
        {16, 625},    // 16 线程，每线程 625 任务
        {16, 5000},   // benchmark_submit_high_concurrency
        {32, 312},    // 32 线程，每线程 312 任务（高并发）
    };

    for (const auto& [num_threads, tasks_per_thread] : test_configs) {
        std::cout << "\n测试配置: " << num_threads << " 线程 x "
                  << tasks_per_thread << " 任务/线程\n";
        print_separator();

        // 基线测试
        auto baseline = benchmark_concurrent_loop_submit(num_threads, tasks_per_thread);
        print_result(baseline);

        // 批量提交测试
        auto batch = benchmark_concurrent_batch_submit(num_threads, tasks_per_thread,
                                                       baseline.throughput);
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
    std::cout << "\n说明：\n";
    std::cout << "- 高并发场景下，批量提交可能减少提交路径开销\n";
    std::cout << "- 任务准备成本、future 管理和调度竞争也可能抵消收益\n";
    std::cout << "- 请以当前机器和目标 workload 的 benchmark 结果为准\n";
    print_separator();

    return 0;
}
