/**
 * 最简单的批量提交性能测试
 */

#include <kairo/executor.hpp>
#include <chrono>
#include <iostream>
#include <atomic>

using namespace kairo;
using namespace std::chrono;

int main() {
    const int NUM_TASKS = 5000;  // 增加到 5000

    std::cout << "批量提交性能对比\n\n";

    // 测试 1
    std::cout << "测试 1: 循环 submit...\n";
    Executor executor1;
    std::atomic<int> counter1{0};

    auto start1 = steady_clock::now();
    for (int i = 0; i < NUM_TASKS; ++i) {
        executor1.submit([&counter1]() { counter1++; });
    }
    auto end1 = steady_clock::now();
    auto us1 = duration_cast<microseconds>(end1 - start1).count();

    (void)executor1.wait_for_completion(std::chrono::seconds{300});
    executor1.shutdown(false);

    std::cout << "  耗时: " << us1 << " μs\n";
    std::cout << "  计数: " << counter1.load() << "\n\n";

    // 测试 2
    std::cout << "测试 2: submit_batch_no_future...\n";
    Executor executor2;
    std::atomic<int> counter2{0};

    std::vector<std::function<void()>> tasks;
    for (int i = 0; i < NUM_TASKS; ++i) {
        tasks.push_back([&counter2]() { counter2++; });
    }

    auto start2 = steady_clock::now();
    executor2.submit_batch_no_future(tasks);
    auto end2 = steady_clock::now();
    auto us2 = duration_cast<microseconds>(end2 - start2).count();

    (void)executor2.wait_for_completion(std::chrono::seconds{300});
    executor2.shutdown(false);

    std::cout << "  耗时: " << us2 << " μs\n";
    std::cout << "  计数: " << counter2.load() << "\n\n";

    double speedup = (double)us1 / us2;
    std::cout << "加速比: " << speedup << "x\n\n";

    std::cout << "完成\n";
    return 0;
}
