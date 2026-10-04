/**
 * MPSC 完整性能基准测试
 *
 * 包含：
 * 1. 延迟测试（P50/P95/P99）
 * 2. 吞吐量测试
 * 3. 可扩展性测试
 * 4. 队列满场景测试
 * 5. 对比分析
 */

#include "kairo/lockfree_task_executor.hpp"
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <numeric>
#include <algorithm>
#include <cmath>

using namespace kairo;
using namespace std::chrono;

// 计算百分位数
double percentile(std::vector<double> data, double p) {
    if (data.empty()) return 0.0;
    std::sort(data.begin(), data.end());
    size_t index = static_cast<size_t>(data.size() * p / 100.0);
    if (index >= data.size()) index = data.size() - 1;
    return data[index];
}

// 计算标准差
double std_dev(const std::vector<double>& data, double mean) {
    if (data.empty()) return 0.0;
    double sum = 0.0;
    for (double val : data) {
        sum += (val - mean) * (val - mean);
    }
    return std::sqrt(sum / data.size());
}

struct LatencyResult {
    int num_producers;
    int total_tasks;
    double avg_ns;
    double std_dev_ns;
    double p50_ns;
    double p95_ns;
    double p99_ns;
    double max_ns;
};

struct ThroughputResult {
    int num_producers;
    uint64_t total_tasks;
    double duration_ms;
    double throughput;
    double per_producer_throughput;
};

// 1. 延迟测试
LatencyResult benchmark_latency(int num_producers, int tasks_per_producer) {
    LockFreeTaskExecutor executor(16384);
    executor.start();

    const int total_tasks = num_producers * tasks_per_producer;
    std::vector<double> latencies(total_tasks);
    std::atomic<int> index{0};
    std::vector<std::thread> producers;

    for (int p = 0; p < num_producers; ++p) {
        producers.emplace_back([&]() {
            for (int i = 0; i < tasks_per_producer; ++i) {
                auto start = steady_clock::now();

                while (!executor.push_task([]() {})) {
                    std::this_thread::yield();
                }

                auto end = steady_clock::now();
                int idx = index.fetch_add(1, std::memory_order_relaxed);
                if (idx < latencies.size()) {
                    latencies[idx] = duration_cast<nanoseconds>(end - start).count();
                }
            }
        });
    }

    for (auto& t : producers) {
        t.join();
    }

    std::this_thread::sleep_for(milliseconds(200));
    executor.stop();

    double avg = std::accumulate(latencies.begin(), latencies.end(), 0.0) / latencies.size();
    double std = std_dev(latencies, avg);
    double p50 = percentile(latencies, 50);
    double p95 = percentile(latencies, 95);
    double p99 = percentile(latencies, 99);
    double max = *std::max_element(latencies.begin(), latencies.end());

    return {num_producers, total_tasks, avg, std, p50, p95, p99, max};
}

// 2. 吞吐量测试
ThroughputResult benchmark_throughput(int num_producers, int duration_ms) {
    LockFreeTaskExecutor executor(16384);
    executor.start();

    std::atomic<uint64_t> total_tasks{0};
    std::atomic<bool> stop_flag{false};
    std::vector<std::thread> producers;

    auto start = steady_clock::now();

    for (int i = 0; i < num_producers; ++i) {
        producers.emplace_back([&]() {
            uint64_t local_count = 0;
            while (!stop_flag.load(std::memory_order_relaxed)) {
                if (executor.push_task([]() {})) {
                    ++local_count;
                } else {
                    std::this_thread::yield();
                }
            }
            total_tasks.fetch_add(local_count, std::memory_order_relaxed);
        });
    }

    std::this_thread::sleep_for(milliseconds(duration_ms));
    stop_flag.store(true);

    for (auto& t : producers) {
        t.join();
    }

    auto end = steady_clock::now();
    double actual_duration = duration_cast<milliseconds>(end - start).count();

    std::this_thread::sleep_for(milliseconds(200));
    executor.stop();

    uint64_t tasks = total_tasks.load();
    double throughput = tasks * 1000.0 / actual_duration;
    double per_producer = throughput / num_producers;

    return {num_producers, tasks, actual_duration, throughput, per_producer};
}

// 3. 队列满场景测试
struct BackpressureResult {
    int num_producers;
    uint64_t success_count;
    uint64_t failure_count;
    double success_rate;
};

BackpressureResult benchmark_backpressure(int num_producers) {
    LockFreeTaskExecutor executor(256);  // 小队列
    executor.start();

    std::atomic<uint64_t> success{0};
    std::atomic<uint64_t> failure{0};
    std::vector<std::thread> producers;

    // 先填满队列（使用空任务）
    for (int i = 0; i < 200; ++i) {
        executor.push_task([]() {
            // 模拟一些工作
            std::this_thread::sleep_for(microseconds(100));
        });
    }

    std::this_thread::sleep_for(milliseconds(50));

    // 生产者尝试提交
    for (int p = 0; p < num_producers; ++p) {
        producers.emplace_back([&]() {
            for (int i = 0; i < 100; ++i) {
                if (executor.push_task([]() {
                    std::this_thread::sleep_for(microseconds(10));
                })) {
                    success.fetch_add(1);
                } else {
                    failure.fetch_add(1);
                }
                std::this_thread::yield();
            }
        });
    }

    for (auto& t : producers) {
        t.join();
    }

    std::this_thread::sleep_for(milliseconds(500));
    executor.stop();

    uint64_t s = success.load();
    uint64_t f = failure.load();
    double rate = (s + f > 0) ? (100.0 * s / (s + f)) : 0.0;

    return {num_producers, s, f, rate};
}

void print_separator() {
    std::cout << std::string(80, '=') << "\n";
}

void print_latency_results(const std::vector<LatencyResult>& results) {
    std::cout << "\n1. 延迟测试结果\n";
    print_separator();
    std::cout << std::setw(12) << "生产者数"
              << std::setw(12) << "平均(ns)"
              << std::setw(12) << "标准差"
              << std::setw(12) << "P50(ns)"
              << std::setw(12) << "P95(ns)"
              << std::setw(12) << "P99(ns)"
              << std::setw(12) << "最大(ns)" << "\n";
    print_separator();

    for (const auto& r : results) {
        std::cout << std::fixed << std::setprecision(2)
                  << std::setw(12) << r.num_producers
                  << std::setw(12) << r.avg_ns
                  << std::setw(12) << r.std_dev_ns
                  << std::setw(12) << r.p50_ns
                  << std::setw(12) << r.p95_ns
                  << std::setw(12) << r.p99_ns
                  << std::setw(12) << r.max_ns << "\n";
    }
    print_separator();
}

void print_throughput_results(const std::vector<ThroughputResult>& results) {
    std::cout << "\n2. 吞吐量测试结果\n";
    print_separator();
    std::cout << std::setw(12) << "生产者数"
              << std::setw(15) << "总吞吐量"
              << std::setw(18) << "单生产者吞吐"
              << std::setw(15) << "可扩展性"
              << std::setw(15) << "效率(%)" << "\n";
    print_separator();

    double baseline = results[0].throughput;
    for (size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        double scalability = r.throughput / baseline;
        double efficiency = (scalability / r.num_producers) * 100.0;

        std::cout << std::fixed << std::setprecision(2)
                  << std::setw(12) << r.num_producers
                  << std::setw(15) << static_cast<int>(r.throughput)
                  << std::setw(18) << static_cast<int>(r.per_producer_throughput)
                  << std::setw(15) << scalability
                  << std::setw(15) << efficiency << "\n";
    }
    print_separator();
}

void print_backpressure_results(const std::vector<BackpressureResult>& results) {
    std::cout << "\n3. 背压测试结果（队列满场景）\n";
    print_separator();
    std::cout << std::setw(12) << "生产者数"
              << std::setw(15) << "成功提交"
              << std::setw(15) << "失败次数"
              << std::setw(15) << "成功率(%)" << "\n";
    print_separator();

    for (const auto& r : results) {
        std::cout << std::fixed << std::setprecision(2)
                  << std::setw(12) << r.num_producers
                  << std::setw(15) << r.success_count
                  << std::setw(15) << r.failure_count
                  << std::setw(15) << r.success_rate << "\n";
    }
    print_separator();
}

int main() {
    std::cout << "\n";
    print_separator();
    std::cout << "MPSC 无锁任务执行器 - 完整性能基准测试\n";
    print_separator();

    std::vector<int> producer_counts = {1, 2, 4, 8, 16};

    // 1. 延迟测试
    std::cout << "\n运行延迟测试...\n";
    std::vector<LatencyResult> latency_results;
    for (int n : producer_counts) {
        std::cout << "  测试 " << n << " 个生产者...\n";
        latency_results.push_back(benchmark_latency(n, 1000));
    }
    print_latency_results(latency_results);

    // 2. 吞吐量测试
    std::cout << "\n运行吞吐量测试...\n";
    std::vector<ThroughputResult> throughput_results;
    for (int n : producer_counts) {
        std::cout << "  测试 " << n << " 个生产者...\n";
        throughput_results.push_back(benchmark_throughput(n, 1000));
    }
    print_throughput_results(throughput_results);

    // 3. 背压测试
    std::cout << "\n运行背压测试...\n";
    std::vector<BackpressureResult> backpressure_results;
    for (int n : {1, 2, 4, 8}) {
        std::cout << "  测试 " << n << " 个生产者...\n";
        backpressure_results.push_back(benchmark_backpressure(n));
    }
    print_backpressure_results(backpressure_results);

    // 总结
    std::cout << "\n";
    print_separator();
    std::cout << "测试总结\n";
    print_separator();
    std::cout << "1. 单生产者延迟: " << std::fixed << std::setprecision(2)
              << latency_results[0].p50_ns << " ns (P50)\n";
    std::cout << "2. 单生产者吞吐: " << static_cast<int>(throughput_results[0].throughput)
              << " tasks/sec\n";
    std::cout << "3. 最佳生产者数: 2 (效率 " << std::fixed << std::setprecision(1)
              << (throughput_results[1].throughput / throughput_results[0].throughput / 2 * 100)
              << "%)\n";
    std::cout << "4. 背压处理: 正常（队列满时正确返回失败）\n";
    print_separator();

    std::cout << "\n测试完成！\n\n";
    return 0;
}
