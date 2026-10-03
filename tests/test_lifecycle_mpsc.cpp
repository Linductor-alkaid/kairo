/**
 * 连续创建销毁测试 - 定位段错误问题
 */

#include "kairo/lockfree_task_executor.hpp"
#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>

using namespace kairo;

void run_test(int test_num, int num_threads) {
    std::cout << "测试 " << test_num << ": " << num_threads << " 个线程\n";

    LockFreeTaskExecutor executor(16384);
    executor.start();

    std::atomic<int> submitted{0};
    std::atomic<bool> stop_flag{false};
    std::vector<std::thread> threads;

    // 启动多个线程
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&]() {
            int count = 0;
            while (!stop_flag.load() && count < 10000) {
                bool success = executor.push_task([]() {
                    // 空任务
                });
                if (success) {
                    count++;
                }
            }
            submitted.fetch_add(count);
        });
    }

    // 运行100ms
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stop_flag.store(true);

    // 等待所有线程完成
    for (auto& t : threads) {
        t.join();
    }

    // 等待任务处理完成
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::cout << "  提交: " << submitted.load()
              << ", 处理: " << executor.processed_count() << "\n";

    executor.stop();
}

int main() {
    std::cout << "连续创建销毁执行器测试\n\n";

    run_test(1, 1);
    run_test(2, 2);
    run_test(3, 4);
    run_test(4, 8);
    run_test(5, 16);

    std::cout << "\n所有测试完成！\n";
    return 0;
}
