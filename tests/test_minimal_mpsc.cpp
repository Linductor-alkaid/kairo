/**
 * 最小化测试 - 定位段错误问题
 */

#include "kairo/lockfree_task_executor.hpp"
#include <iostream>
#include <thread>
#include <chrono>

using namespace kairo;

int main() {
    std::cout << "测试1: 创建执行器\n";
    LockFreeTaskExecutor executor(1024);

    std::cout << "测试2: 启动执行器\n";
    if (!executor.start()) {
        std::cerr << "启动失败\n";
        return 1;
    }

    std::cout << "测试3: 提交10个任务\n";
    for (int i = 0; i < 10; ++i) {
        bool success = executor.push_task([]() {
            // 空任务
        });
        if (!success) {
            std::cerr << "任务 " << i << " 提交失败\n";
        }
    }

    std::cout << "测试4: 等待任务完成\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::cout << "测试5: 停止执行器\n";
    executor.stop();

    std::cout << "测试6: 处理任务数 = " << executor.processed_count() << "\n";

    std::cout << "所有测试通过！\n";
    return 0;
}
