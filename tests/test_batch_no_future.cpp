/**
 * 简单测试：验证 submit_batch_no_future 功能
 */

#include <kairo/executor.hpp>
#include <iostream>
#include <atomic>
#include <thread>
#include <chrono>

using namespace kairo;
using namespace std::chrono;

int main() {
    std::cout << "测试 submit_batch_no_future 功能...\n";

    Executor executor;
    std::atomic<int> counter{0};

    // 准备任务
    std::vector<std::function<void()>> tasks;
    for (int i = 0; i < 1000; ++i) {
        tasks.push_back([&counter]() {
            counter.fetch_add(1, std::memory_order_relaxed);
        });
    }

    std::cout << "提交 1000 个任务...\n";
    auto start = steady_clock::now();

    executor.submit_batch_no_future(tasks);

    auto end = steady_clock::now();
    auto duration = duration_cast<microseconds>(end - start).count();

    std::cout << "提交耗时: " << duration << " μs\n";

    // 等待任务完成
    std::this_thread::sleep_for(milliseconds(100));

    std::cout << "计数器值: " << counter.load() << " (期望: 1000)\n";

    executor.shutdown();

    if (counter.load() == 1000) {
        std::cout << "✅ 测试通过\n";
        return 0;
    } else {
        std::cout << "❌ 测试失败\n";
        return 1;
    }
}
