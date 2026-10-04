/**
 * LockFreeTaskExecutor 使用示例
 *
 * 演示高性能无锁任务执行器的基本用法
 */

#include <kairo/lockfree_task_executor.hpp>
#include <iostream>
#include <atomic>
#include <chrono>
#include <thread>

using namespace kairo;

// 示例1: 基本使用
void example_basic() {
    std::cout << "=== Example 1: Basic Usage ===\n";

    LockFreeTaskExecutor exec(1024);
    exec.start();

    std::atomic<int> counter{0};

    for (int i = 0; i < 10; ++i) {
        exec.push_task([&counter, i]() {
            counter.fetch_add(1);
            std::cout << "Task " << i << " executed\n";
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    exec.stop();

    std::cout << "Total tasks processed: " << exec.processed_count() << "\n\n";
}

// 示例2: 高频日志收集
void example_logging() {
    std::cout << "=== Example 2: High-Frequency Logging ===\n";

    LockFreeTaskExecutor logger(4096);
    logger.start();

    auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < 1000; ++i) {
        logger.push_task([i]() {
            // 模拟日志写入
            // std::cout << "Log entry " << i << "\n";
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    logger.stop();

    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << "Processed " << logger.processed_count() << " log entries in " << duration << " ms\n\n";
}

// 示例3: 异步事件处理
void example_event_processing() {
    std::cout << "=== Example 3: Async Event Processing ===\n";

    LockFreeTaskExecutor event_processor(2048);
    event_processor.start();

    std::atomic<int> events_processed{0};

    // 模拟事件生成
    for (int i = 0; i < 100; ++i) {
        bool success = event_processor.push_task([&events_processed, i]() {
            // 处理事件
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            events_processed.fetch_add(1);
        });

        if (!success) {
            std::cout << "Queue full at event " << i << "\n";
        }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::cout << "Events processed: " << events_processed.load() << "\n";
    std::cout << "Pending: " << event_processor.pending_count() << "\n";

    event_processor.stop();
    std::cout << "\n";
}

int main() {
    example_basic();
    example_logging();
    example_event_processing();

    return 0;
}
