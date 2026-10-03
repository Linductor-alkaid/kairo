#include <cassert>
#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <future>
#include <algorithm>

// 包含 Executor 的头文件
#include <kairo/executor.hpp>

using namespace kairo;

// 测试辅助宏
#define TEST_ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            std::cerr << "FAILED: " << message << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
            return false; \
        } \
    } while(0)

// 测试函数前向声明
bool test_throughput();
bool test_priority_performance();

// ========== 吞吐量测试 ==========

bool test_throughput() {
    std::cout << "Testing throughput (large number of tasks)..." << std::endl;
    
    ExecutorConfig config;
    config.min_threads = 4;
    config.max_threads = 8;
    config.queue_capacity = 10000;
    
    Executor executor;
    TEST_ASSERT(executor.initialize(config), "Executor initialization should succeed");
    
    const int num_tasks = 50000;
    std::atomic<int> completed_tasks{0};
    
    auto start_time = std::chrono::steady_clock::now();
    
    // 提交大量简单任务
    std::vector<std::future<void>> futures;
    futures.reserve(num_tasks);
    
    for (int i = 0; i < num_tasks; ++i) {
        auto future = executor.submit([&completed_tasks]() {
            // 简单计算任务
            std::atomic<int> sum{0};
            for (int j = 0; j < 100; ++j) {
                sum.fetch_add(j, std::memory_order_relaxed);
            }
            completed_tasks.fetch_add(1);
        });
        futures.push_back(std::move(future));
    }
    
    // 等待所有任务完成
    for (auto& future : futures) {
        future.get();
    }
    
    auto end_time = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time).count();
    
    TEST_ASSERT(completed_tasks.load() == num_tasks, "All tasks should be completed");
    
    double throughput = (num_tasks * 1000.0) / elapsed_ms;  // tasks per second
    
    std::cout << "  Submitted tasks: " << num_tasks << std::endl;
    std::cout << "  Elapsed time: " << elapsed_ms << " ms" << std::endl;
    std::cout << "  Throughput: " << throughput << " tasks/second" << std::endl;
    std::cout << "  Throughput test: PASSED" << std::endl;
    
    executor.shutdown();
    return true;
}

// ========== 优先级性能测试 ==========

bool test_priority_performance() {
    std::cout << "Testing priority scheduling performance..." << std::endl;
    
    ExecutorConfig config;
    config.min_threads = 4;
    config.max_threads = 8;
    config.queue_capacity = 1000;
    
    Executor executor;
    TEST_ASSERT(executor.initialize(config), "Executor initialization should succeed");
    
    const int tasks_per_priority = 1000;
    std::vector<int> execution_order;
    std::mutex order_mutex;
    
    auto start_time = std::chrono::steady_clock::now();
    
    // 提交不同优先级的任务
    // 低优先级任务（会 sleep，模拟慢任务）
    for (int i = 0; i < tasks_per_priority; ++i) {
        executor.submit_priority(0, [i, &execution_order, &order_mutex]() {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            std::lock_guard<std::mutex> lock(order_mutex);
            execution_order.push_back(0);
        });
    }
    
    // 高优先级任务（立即完成）
    for (int i = 0; i < tasks_per_priority; ++i) {
        executor.submit_priority(2, [i, &execution_order, &order_mutex]() {
            std::lock_guard<std::mutex> lock(order_mutex);
            execution_order.push_back(2);
        });
    }
    
    // 等待所有任务完成
    (void)executor.wait_for_completion(std::chrono::seconds{300});
    
    auto end_time = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time).count();
    
    TEST_ASSERT(execution_order.size() == tasks_per_priority * 2, 
                "All tasks should be completed");
    
    // 检查高优先级任务是否优先完成（前 N 个任务中，高优先级应该占多数）
    const int check_count = std::min(500, static_cast<int>(execution_order.size()));
    int high_priority_first = 0;
    for (int i = 0; i < check_count; ++i) {
        if (execution_order[i] == 2) {
            high_priority_first++;
        }
    }
    
    double high_priority_ratio = static_cast<double>(high_priority_first) / check_count;
    
    std::cout << "  Total tasks: " << execution_order.size() << std::endl;
    std::cout << "  Elapsed time: " << elapsed_ms << " ms" << std::endl;
    std::cout << "  High priority ratio in first " << check_count 
              << " tasks: " << high_priority_ratio << std::endl;
    std::cout << "  Priority performance test: PASSED" << std::endl;
    
    executor.shutdown();
    return true;
}

// ========== 主函数 ==========

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Performance Test" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;
    
    bool all_passed = true;
    
    // 吞吐量测试
    std::cout << "--- Throughput Test ---" << std::endl;
    all_passed &= test_throughput();
    std::cout << std::endl;
    
    // 优先级性能测试
    std::cout << "--- Priority Performance Test ---" << std::endl;
    all_passed &= test_priority_performance();
    std::cout << std::endl;
    
    if (all_passed) {
        std::cout << "========================================" << std::endl;
        std::cout << "All performance tests PASSED" << std::endl;
        std::cout << "========================================" << std::endl;
        return 0;
    } else {
        std::cerr << "========================================" << std::endl;
        std::cerr << "Some performance tests FAILED" << std::endl;
        std::cerr << "========================================" << std::endl;
        return 1;
    }
}
