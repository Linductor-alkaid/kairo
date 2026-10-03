/**
 * MPSC 性能基准测试（完整版）
 *
 * 测试不同生产者数量下的性能表现
 * 输出：吞吐量、延迟分布、失败率、JSON格式
 */

#include "kairo/lockfree_task_executor.hpp"
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <numeric>
#include <cstring>

using namespace kairo;
using namespace std::chrono;

struct BenchmarkResult {
    int num_producers;
    uint64_t total_pushes;
    uint64_t failed_pushes;
    uint64_t processed_tasks;
    double duration_ms;
    double throughput;
    double failure_rate;
    std::vector<uint64_t> latencies_ns;
};

// 计算百分位数
uint64_t percentile(std::vector<uint64_t>& data, double p) {
    if (data.empty()) return 0;
    size_t idx = static_cast<size_t>(data.size() * p);
    if (idx >= data.size()) idx = data.size() - 1;
    std::nth_element(data.begin(), data.begin() + idx, data.end());
    return data[idx];
}

// 基准测试：完整性能测试
BenchmarkResult benchmark_throughput(int num_producers) {
    const int test_duration_ms = 1000;
    const size_t sample_interval = 100; // 每100次采样一次延迟
    LockFreeTaskExecutor executor(16384);

    BenchmarkResult result;
    result.num_producers = num_producers;

    if (!executor.start()) {
        std::cerr << "Failed to start executor\n";
        return result;
    }

    std::atomic<uint64_t> total_pushes{0};
    std::atomic<uint64_t> failed_pushes{0};
    std::atomic<bool> stop_flag{false};
    std::vector<std::thread> producers;
    std::vector<std::vector<uint64_t>> per_thread_latencies(num_producers);

    auto start = steady_clock::now();

    // 启动生产者线程
    for (int i = 0; i < num_producers; ++i) {
        producers.emplace_back([&, thread_id = i]() {
            uint64_t local_success = 0;
            uint64_t local_failed = 0;
            uint64_t sample_counter = 0;

            while (!stop_flag.load(std::memory_order_relaxed)) {
                auto push_start = steady_clock::now();
                bool success = executor.push_task([]() {});

                if (success) {
                    ++local_success;
                    // 采样延迟
                    if (++sample_counter % sample_interval == 0) {
                        auto push_end = steady_clock::now();
                        uint64_t latency_ns = duration_cast<nanoseconds>(push_end - push_start).count();
                        per_thread_latencies[thread_id].push_back(latency_ns);
                    }
                } else {
                    ++local_failed;
                    std::this_thread::yield();
                }
            }
            total_pushes.fetch_add(local_success, std::memory_order_relaxed);
            failed_pushes.fetch_add(local_failed, std::memory_order_relaxed);
        });
    }

    // 运行指定时间
    std::this_thread::sleep_for(milliseconds(test_duration_ms));
    stop_flag.store(true);

    // 等待生产者完成
    for (auto& t : producers) {
        t.join();
    }

    auto end = steady_clock::now();
    result.duration_ms = duration_cast<milliseconds>(end - start).count();

    // 等待消费者处理完
    std::this_thread::sleep_for(milliseconds(200));

    result.total_pushes = total_pushes.load();
    result.failed_pushes = failed_pushes.load();
    result.processed_tasks = executor.processed_count();
    result.throughput = result.total_pushes * 1000.0 / result.duration_ms;
    result.failure_rate = result.failed_pushes * 100.0 / (result.total_pushes + result.failed_pushes);

    // 合并所有线程的延迟数据
    for (auto& latencies : per_thread_latencies) {
        result.latencies_ns.insert(result.latencies_ns.end(), latencies.begin(), latencies.end());
    }

    executor.stop();
    return result;
}

// 打印结果（控制台）
void print_result(const BenchmarkResult& result) {
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "生产者数量: " << result.num_producers << "\n";
    std::cout << "  提交任务数: " << result.total_pushes << "\n";
    std::cout << "  失败次数: " << result.failed_pushes << "\n";
    std::cout << "  失败率: " << result.failure_rate << "%\n";
    std::cout << "  处理任务数: " << result.processed_tasks << "\n";
    std::cout << "  吞吐量: " << result.throughput << " tasks/sec\n";

    if (!result.latencies_ns.empty()) {
        auto latencies = result.latencies_ns;
        uint64_t p50 = percentile(latencies, 0.50);
        uint64_t p95 = percentile(latencies, 0.95);
        uint64_t p99 = percentile(latencies, 0.99);
        uint64_t avg = std::accumulate(latencies.begin(), latencies.end(), 0ULL) / latencies.size();

        std::cout << "  延迟 (ns): avg=" << avg << " p50=" << p50
                  << " p95=" << p95 << " p99=" << p99 << "\n";
    }
    std::cout << "\n";
}

// 输出JSON格式
void print_json(const std::vector<BenchmarkResult>& results) {
    std::cout << "{\n";
    std::cout << "  \"benchmark\": \"lockfree_mpsc\",\n";
    std::cout << "  \"timestamp\": \"" << duration_cast<seconds>(system_clock::now().time_since_epoch()).count() << "\",\n";
    std::cout << "  \"results\": [\n";

    for (size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        std::cout << "    {\n";
        std::cout << "      \"num_producers\": " << r.num_producers << ",\n";
        std::cout << "      \"total_pushes\": " << r.total_pushes << ",\n";
        std::cout << "      \"failed_pushes\": " << r.failed_pushes << ",\n";
        std::cout << "      \"failure_rate\": " << r.failure_rate << ",\n";
        std::cout << "      \"throughput\": " << r.throughput << ",\n";
        std::cout << "      \"duration_ms\": " << r.duration_ms << ",\n";

        if (!r.latencies_ns.empty()) {
            auto latencies = r.latencies_ns;
            uint64_t p50 = percentile(latencies, 0.50);
            uint64_t p95 = percentile(latencies, 0.95);
            uint64_t p99 = percentile(latencies, 0.99);
            uint64_t avg = std::accumulate(latencies.begin(), latencies.end(), 0ULL) / latencies.size();

            std::cout << "      \"latency_ns\": {\n";
            std::cout << "        \"avg\": " << avg << ",\n";
            std::cout << "        \"p50\": " << p50 << ",\n";
            std::cout << "        \"p95\": " << p95 << ",\n";
            std::cout << "        \"p99\": " << p99 << "\n";
            std::cout << "      }\n";
        }

        std::cout << "    }" << (i < results.size() - 1 ? "," : "") << "\n";
    }

    std::cout << "  ]\n";
    std::cout << "}\n";
}

int main(int argc, char** argv) {
    bool json_output = false;
    if (argc > 1 && std::strcmp(argv[1], "--json") == 0) {
        json_output = true;
    }

    if (!json_output) {
        std::cout << "\n=== MPSC 性能基准测试 ===\n\n";
    }

    std::vector<int> producer_counts = {1, 2, 4, 8, 16, 32};
    std::vector<BenchmarkResult> results;

    for (int count : producer_counts) {
        auto result = benchmark_throughput(count);
        results.push_back(result);

        if (!json_output) {
            print_result(result);
        }
    }

    if (json_output) {
        print_json(results);
    } else {
        std::cout << "测试完成！使用 --json 参数输出JSON格式\n";
    }

    return 0;
}
