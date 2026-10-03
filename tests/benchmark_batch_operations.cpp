/**
 * 批量操作性能基准测试
 */

#include "kairo/util/lockfree_queue.hpp"
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <iostream>
#include <iomanip>

using namespace kairo::util;
using namespace std::chrono;

struct Result {
    double throughput_ops_per_sec;
    double latency_ns;
};

Result benchmark_single_ops(int num_producers, int duration_ms) {
    LockFreeQueue<int> queue(16384);
    std::atomic<uint64_t> total_ops{0};
    std::atomic<bool> stop{false};

    auto start = steady_clock::now();

    std::vector<std::thread> producers;
    for (int i = 0; i < num_producers; ++i) {
        producers.emplace_back([&]() {
            uint64_t local_ops = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                if (queue.push(42)) {
                    ++local_ops;
                }
            }
            total_ops.fetch_add(local_ops, std::memory_order_relaxed);
        });
    }

    std::thread consumer([&]() {
        int item;
        while (!stop.load(std::memory_order_relaxed)) {
            queue.pop(item);
        }
    });

    std::this_thread::sleep_for(milliseconds(duration_ms));
    stop.store(true);

    for (auto& t : producers) t.join();
    consumer.join();

    auto end = steady_clock::now();
    double duration_sec = duration_cast<nanoseconds>(end - start).count() / 1e9;
    uint64_t ops = total_ops.load();

    return {ops / duration_sec, duration_sec * 1e9 / ops};
}

Result benchmark_batch_ops(int num_producers, int duration_ms, size_t batch_size) {
    LockFreeQueue<int> queue(16384);
    std::atomic<uint64_t> total_ops{0};
    std::atomic<bool> stop{false};

    auto start = steady_clock::now();

    std::vector<std::thread> producers;
    for (int i = 0; i < num_producers; ++i) {
        producers.emplace_back([&]() {
            std::vector<int> batch(batch_size, 42);
            uint64_t local_ops = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                size_t pushed;
                if (queue.push_batch(batch.data(), batch_size, pushed)) {
                    local_ops += pushed;
                }
            }
            total_ops.fetch_add(local_ops, std::memory_order_relaxed);
        });
    }

    std::thread consumer([&]() {
        std::vector<int> batch(batch_size);
        while (!stop.load(std::memory_order_relaxed)) {
            queue.pop_batch(batch.data(), batch_size);
        }
    });

    std::this_thread::sleep_for(milliseconds(duration_ms));
    stop.store(true);

    for (auto& t : producers) t.join();
    consumer.join();

    auto end = steady_clock::now();
    double duration_sec = duration_cast<nanoseconds>(end - start).count() / 1e9;
    uint64_t ops = total_ops.load();

    return {ops / duration_sec, duration_sec * 1e9 / ops};
}

int main() {
    const int duration_ms = 1000;
    const std::vector<int> producer_counts = {1, 2, 4, 8};
    const std::vector<size_t> batch_sizes = {10, 50, 100};

    std::cout << "批量操作性能测试\n";
    std::cout << "================\n\n";

    for (int num_producers : producer_counts) {
        std::cout << num_producers << " 生产者:\n";

        auto single = benchmark_single_ops(num_producers, duration_ms);
        std::cout << "  单个操作: " << std::fixed << std::setprecision(2)
                  << single.throughput_ops_per_sec / 1e6 << " M ops/s\n";

        for (size_t batch_size : batch_sizes) {
            auto batch = benchmark_batch_ops(num_producers, duration_ms, batch_size);
            double speedup = batch.throughput_ops_per_sec / single.throughput_ops_per_sec;
            std::cout << "  批量(" << batch_size << "): "
                      << batch.throughput_ops_per_sec / 1e6 << " M ops/s"
                      << " (加速 " << speedup << "x)\n";
        }
        std::cout << "\n";
    }

    return 0;
}
