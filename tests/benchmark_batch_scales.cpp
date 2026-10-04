/**
 * 批量提交性能测试 - 多个规模
 */

#include <kairo/executor.hpp>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <atomic>

using namespace kairo;
using namespace std::chrono;

void test_scale(int num_tasks) {
    std::cout << "\n测试规模: " << num_tasks << " 个任务\n";
    std::cout << std::string(50, '-') << "\n";

    // 测试 1: 循环 submit
    Executor executor1;
    std::atomic<int> counter1{0};

    auto start1 = steady_clock::now();
    for (int i = 0; i < num_tasks; ++i) {
        executor1.submit([&counter1]() { counter1++; });
    }
    auto end1 = steady_clock::now();
    auto us1 = duration_cast<microseconds>(end1 - start1).count();

    std::this_thread::sleep_for(milliseconds(200));
    executor1.shutdown();

    std::cout << "循环 submit:           " << std::setw(8) << us1 << " μs\n";

    // 测试 2: submit_batch_no_future
    Executor executor2;
    std::atomic<int> counter2{0};

    std::vector<std::function<void()>> tasks;
    tasks.reserve(num_tasks);
    for (int i = 0; i < num_tasks; ++i) {
        tasks.push_back([&counter2]() { counter2++; });
    }

    auto start2 = steady_clock::now();
    executor2.submit_batch_no_future(tasks);
    auto end2 = steady_clock::now();
    auto us2 = duration_cast<microseconds>(end2 - start2).count();

    std::this_thread::sleep_for(milliseconds(200));
    executor2.shutdown();

    std::cout << "submit_batch_no_future: " << std::setw(8) << us2 << " μs\n";

    double speedup = static_cast<double>(us1) / static_cast<double>(us2);
    std::cout << "加速比: " << std::fixed << std::setprecision(2) << speedup << "x\n";

    if (counter1.load() != num_tasks || counter2.load() != num_tasks) {
        std::cout << "⚠️  警告: 计数器不匹配 (" << counter1.load() << ", "
                  << counter2.load() << ")\n";
    }
}

int main() {
    std::cout << "\n批量提交性能测试 - 多个规模\n";
    std::cout << std::string(50, '=') << "\n";

    test_scale(500);
    test_scale(1000);
    test_scale(2000);
    test_scale(5000);

    std::cout << "\n" << std::string(50, '=') << "\n";
    std::cout << "测试完成\n\n";

    return 0;
}
