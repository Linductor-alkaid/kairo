/**
 * 批量任务提交性能测试 - 对比 future vs no-future
 *
 * 测试场景：高并发多线程提交
 * 对比返回 future 和不返回 future 的性能差异
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
BenchmarkResult benchmark_loop_submit(int num_threads, int tasks_per_thread) {
    Executor executor;
    std::vector<std::thread> threads;
    std::atomic<bool> start_flag{false};

    auto start_time = steady_clock::now();

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&]() {
            while (!start_flag.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            for (int i = 0; i < tasks_per_thread; ++i) {
                executor.submit([]() {});
            }
        });
    }

    start_flag.store(true, std::memory_order_release);

    for (auto& t : threads) {
        t.join();
    }

    auto end_time = steady_clock::now();
    double duration_ms = duration_cast<milliseconds>(end_time - start_time).count();

    (void)executor.wait_for_completion(std::chrono::seconds{300});
    executor.shutdown(false);

    int total_tasks = num_threads * tasks_per_thread;
    double throughput = total_tasks * 1000.0 / duration_ms;

    return {"Loop submit()", num_threads, tasks_per_thread,
            total_tasks, duration_ms, throughput, 1.0};
}

// 批量提交（返回 future）
BenchmarkResult benchmark_batch_with_future(int num_threads, int tasks_per_thread,
                                            double baseline_throughput) {
    Executor executor;
    std::vector<std::thread> threads;
    std::atomic<bool> start_flag{false};

    auto start_time = steady_clock::now();

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&]() {
            while (!start_flag.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            std::vector<std::function<void()>> tasks;
            tasks.reserve(tasks_per_thread);
            for (int i = 0; i < tasks_per_thread; ++i) {
                tasks.push_back([]() {});
            }

            auto futures = executor.submit_batch(tasks);
        });
    }

    start_flag.store(true, std::memory_order_release);

    for (auto& t : threads) {
        t.join();
    }

    auto end_time = steady_clock::now();
    double duration_ms = duration_cast<milliseconds>(end_time - start_time).count();

    (void)executor.wait_for_completion(std::chrono::seconds{300});
    executor.shutdown(false);

    int total_tasks = num_threads * tasks_per_thread;
    double throughput = total_tasks * 1000.0 / duration_ms;
    double speedup = throughput / baseline_throughput;

    return {"submit_batch() [with future]", num_threads, tasks_per_thread,
            total_tasks, duration_ms, throughput, speedup};
}

// 批量提交（不返回 future）
BenchmarkResult benchmark_batch_no_future(int num_threads, int tasks_per_thread,
                                          double baseline_throughput) {
    Executor executor;
    std::vector<std::thread> threads;
    std::atomic<bool> start_flag{false};

    auto start_time = steady_clock::now();

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&]() {
            while (!start_flag.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            std::vector<std::function<void()>> tasks;
            tasks.reserve(tasks_per_thread);
            for (int i = 0; i < tasks_per_thread; ++i) {
                tasks.push_back([]() {});
            }

            executor.submit_batch_no_future(tasks);
        });
    }

    start_flag.store(true, std::memory_order_release);

    for (auto& t : threads) {
        t.join();
    }

    auto end_time = steady_clock::now();
    double duration_ms = duration_cast<milliseconds>(end_time - start_time).count();

    (void)executor.wait_for_completion(std::chrono::seconds{300});
    executor.shutdown(false);

    int total_tasks = num_threads * tasks_per_thread;
    double throughput = total_tasks * 1000.0 / duration_ms;
    double speedup = throughput / baseline_throughput;

    return {"submit_batch_no_future()", num_threads, tasks_per_thread,
            total_tasks, duration_ms, throughput, speedup};
}

void print_result(const BenchmarkResult& result) {
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "\n" << result.test_name << "\n";
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
    std::cout << "批量任务提交性能对比 - Future vs No-Future\n";
    print_separator();

    const std::vector<std::pair<int, int>> test_configs = {
        {4, 2500},
        {8, 1250},
        {16, 625},
        {32, 312},
    };

    for (const auto& [num_threads, tasks_per_thread] : test_configs) {
        std::cout << "\n配置: " << num_threads << " 线程 x "
                  << tasks_per_thread << " 任务/线程\n";
        print_separator();

        auto baseline = benchmark_loop_submit(num_threads, tasks_per_thread);
        print_result(baseline);

        auto batch_future = benchmark_batch_with_future(num_threads, tasks_per_thread,
                                                        baseline.throughput);
        print_result(batch_future);

        auto batch_no_future = benchmark_batch_no_future(num_threads, tasks_per_thread,
                                                         baseline.throughput);
        print_result(batch_no_future);

        std::cout << "\n对比:\n";
        std::cout << "  有 future:  " << batch_future.speedup << "x\n";
        std::cout << "  无 future:  " << batch_no_future.speedup << "x\n";

        if (batch_no_future.speedup > batch_future.speedup) {
            double improvement = (batch_no_future.speedup / batch_future.speedup - 1.0) * 100;
            std::cout << "  ✓ 无 future 版本提升 " << improvement << "%\n";
        } else {
            std::cout << "  无 future 版本未快于有 future 版本\n";
        }
    }

    std::cout << "\n";
    print_separator();
    std::cout << "测试完成\n";
    print_separator();

    return 0;
}
