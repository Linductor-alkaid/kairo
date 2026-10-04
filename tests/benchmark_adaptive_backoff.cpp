/**
 * 自适应退避策略基准测试
 */

#include "kairo/lockfree_task_executor.hpp"
#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>

using namespace std::chrono;
using namespace kairo;

struct TestResult {
    int num_producers;
    size_t backoff_multiplier;
    uint64_t total_pushes;
    uint64_t failed_pushes;
    double throughput;
    double failure_rate;
};

TestResult run_test(int num_producers, size_t backoff_multiplier) {
    const size_t QUEUE_CAPACITY = 16384;
    LockFreeTaskExecutor executor(QUEUE_CAPACITY, backoff_multiplier);
    executor.start();

    std::atomic<bool> running{true};
    std::atomic<uint64_t> total_pushes{0};
    std::atomic<uint64_t> failed_pushes{0};

    // 生产者线程
    std::vector<std::thread> producers;
    auto producer_func = [&]() {
        while (running.load()) {
            if (executor.push_task([](){})) {
                total_pushes.fetch_add(1);
            } else {
                failed_pushes.fetch_add(1);
            }
        }
    };

    for (int i = 0; i < num_producers; ++i) {
        producers.emplace_back(producer_func);
    }

    // 运行 1 秒
    auto start = steady_clock::now();
    std::this_thread::sleep_for(milliseconds(1000));
    running.store(false);
    auto end = steady_clock::now();

    for (auto& t : producers) {
        t.join();
    }

    double duration_ms = duration_cast<milliseconds>(end - start).count();

    TestResult result;
    result.num_producers = num_producers;
    result.backoff_multiplier = backoff_multiplier;
    result.total_pushes = total_pushes.load();
    result.failed_pushes = failed_pushes.load();
    result.throughput = result.total_pushes * 1000.0 / duration_ms;
    result.failure_rate = result.failed_pushes * 100.0 / (result.total_pushes + result.failed_pushes);

    return result;
}

int main() {
    std::cout << "=== 自适应退避策略测试 ===\n\n";

    std::vector<TestResult> results;

    // 测试不同生产者数量和退避倍数的组合
    std::vector<std::pair<int, size_t>> configs = {
        {1, 1},   {2, 1},   {4, 1},
        {8, 1},   {8, 2},
        {16, 1},  {16, 2},  {16, 4},
        {32, 1},  {32, 2},  {32, 4},  {32, 8},
    };

    for (const auto& [num_producers, multiplier] : configs) {
        std::cout << "测试: " << num_producers << " 生产者, "
                  << multiplier << "x 退避..." << std::flush;
        auto result = run_test(num_producers, multiplier);
        results.push_back(result);
        std::cout << " 完成\n";
    }

    // 输出结果
    std::cout << "\n=== 测试结果 ===\n\n";
    std::cout << "生产者 | 退避倍数 | 吞吐量(M ops/s) | 失败率(%) \n";
    std::cout << "-------|----------|-----------------|----------\n";

    for (const auto& r : results) {
        printf("%6d | %8zux | %15.2f | %9.2f\n",
               r.num_producers, r.backoff_multiplier,
               r.throughput / 1e6, r.failure_rate);
    }

    return 0;
}
