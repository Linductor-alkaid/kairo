#include <cassert>
#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <future>
#include <algorithm>

#include "kairo/thread_pool/thread_pool.hpp"
#include <kairo/config.hpp>

using namespace kairo;

// 测试辅助宏
#define TEST_ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            std::cerr << "FAILED: " << message << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
            return false; \
        } \
    } while(0)

// ========== 完整工作流测试 ==========

bool test_complete_workflow() {
    std::cout << "Testing complete workflow with enhancements..." << std::endl;
    
    ThreadPoolConfig config;
    config.min_threads = 4;
    config.max_threads = 8;
    config.queue_capacity = 1000;
    config.enable_work_stealing = true;
    
    ThreadPool pool;
    bool initialized = pool.initialize(config);
    TEST_ASSERT(initialized, "ThreadPool should initialize successfully");
    
    // 检查初始状态
    auto status = pool.get_status();
    TEST_ASSERT(status.total_threads == 4, "Initial thread count should be 4");
    
    // 提交大量任务
    std::atomic<int> completed(0);
    const int num_tasks = 100;
    
    for (int i = 0; i < num_tasks; ++i) {
        pool.submit([&completed, i]() {
            // 模拟一些工作
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            completed.fetch_add(1);
        });
    }
    
    // 等待所有任务完成
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    pool.wait_for_completion();
    
    TEST_ASSERT(completed.load() == num_tasks, "All tasks should complete");
    
    // 检查最终状态
    status = pool.get_status();
    TEST_ASSERT(status.completed_tasks == num_tasks, "Completed tasks should match");
    
    pool.shutdown(true);
    
    return true;
}

// ========== 高负载场景测试 ==========

bool test_high_load_scenario() {
    std::cout << "Testing high load scenario..." << std::endl;
    
    ThreadPoolConfig config;
    config.min_threads = 2;
    config.max_threads = 8;
    config.queue_capacity = 500;
    config.enable_work_stealing = true;
    
    ThreadPool pool;
    bool initialized = pool.initialize(config);
    TEST_ASSERT(initialized, "ThreadPool should initialize successfully");
    
    // 提交大量高优先级任务
    std::atomic<int> high_priority_completed(0);
    std::atomic<int> normal_priority_completed(0);
    
    const int num_high_priority = 50;
    const int num_normal_priority = 100;
    
    // 提交高优先级任务
    for (int i = 0; i < num_high_priority; ++i) {
        pool.submit_priority(3, [&high_priority_completed]() {  // CRITICAL
            high_priority_completed.fetch_add(1);
        });
    }
    
    // 提交普通优先级任务
    for (int i = 0; i < num_normal_priority; ++i) {
        pool.submit([&normal_priority_completed]() {
            normal_priority_completed.fetch_add(1);
        });
    }
    
    // 等待所有任务完成
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    pool.wait_for_completion();
    
    TEST_ASSERT(high_priority_completed.load() == num_high_priority, 
                "All high priority tasks should complete");
    TEST_ASSERT(normal_priority_completed.load() == num_normal_priority, 
                "All normal priority tasks should complete");
    
    pool.shutdown(true);
    
    return true;
}

// ========== 工作窃取性能测试 ==========

bool test_work_stealing_performance() {
    std::cout << "Testing work stealing performance..." << std::endl;
    
    ThreadPoolConfig config;
    config.min_threads = 4;
    config.max_threads = 4;
    config.queue_capacity = 100;
    config.enable_work_stealing = true;
    
    ThreadPool pool;
    bool initialized = pool.initialize(config);
    TEST_ASSERT(initialized, "ThreadPool should initialize successfully");
    
    // 提交不均匀分布的任务（模拟某些线程负载高的情况）
    std::atomic<int> completed(0);
    const int num_tasks = 200;
    
    for (int i = 0; i < num_tasks; ++i) {
        pool.submit([&completed]() {
            // 模拟不同长度的任务
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            completed.fetch_add(1);
        });
    }
    
    auto start = std::chrono::steady_clock::now();
    
    // 等待所有任务完成
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    pool.wait_for_completion();
    
    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    TEST_ASSERT(completed.load() == num_tasks, "All tasks should complete");
    std::cout << "  Completed " << num_tasks << " tasks in " << duration.count() << "ms" << std::endl;
    
    pool.shutdown(true);
    
    return true;
}

// ========== 主测试函数 ==========

int main() {
    std::cout << "=== Thread Pool Integration Tests ===" << std::endl;
    
    bool all_passed = true;
    
    // 完整工作流测试
    all_passed &= test_complete_workflow();
    
    // 高负载场景测试
    all_passed &= test_high_load_scenario();
    
    // 工作窃取性能测试
    all_passed &= test_work_stealing_performance();
    
    if (all_passed) {
        std::cout << "\n=== All integration tests PASSED ===" << std::endl;
        return 0;
    } else {
        std::cout << "\n=== Some integration tests FAILED ===" << std::endl;
        return 1;
    }
}
