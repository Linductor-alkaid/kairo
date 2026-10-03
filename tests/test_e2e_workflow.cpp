#include <cassert>
#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <future>

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
bool test_singleton_workflow();
bool test_instance_workflow();

// ========== 单例模式完整工作流测试 ==========

bool test_singleton_workflow() {
    std::cout << "Testing singleton mode complete workflow..." << std::endl;
    
    // 1. 初始化 Executor（单例模式）
    ExecutorConfig config;
    config.min_threads = 2;
    config.max_threads = 4;
    config.queue_capacity = 100;
    config.enable_monitoring = true;
    
    auto& executor = Executor::instance();
    TEST_ASSERT(executor.initialize(config), "Executor initialization should succeed");
    std::cout << "  Step 1: Executor initialized (singleton mode)" << std::endl;
    
    // 2. 提交异步任务并等待完成
    std::atomic<int> task_counter{0};
    const int num_tasks = 10;
    std::vector<std::future<int>> futures;
    
    for (int i = 0; i < num_tasks; ++i) {
        auto future = executor.submit([i, &task_counter]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            task_counter.fetch_add(1);
            return i * i;
        });
        futures.push_back(std::move(future));
    }
    
    // 等待所有任务完成
    (void)executor.wait_for_completion(std::chrono::seconds{300});
    
    // 验证任务完成
    TEST_ASSERT(task_counter.load() == num_tasks, "All tasks should be completed");
    std::cout << "  Step 2: Submitted and completed " << num_tasks << " async tasks" << std::endl;
    
    // 验证 future 结果
    for (size_t i = 0; i < futures.size(); ++i) {
        int result = futures[i].get();
        TEST_ASSERT(result == static_cast<int>(i * i), "Task result should match");
    }
    
    // 3. 注册并启动实时任务
    std::atomic<int> realtime_cycle_count{0};
    
    RealtimeThreadConfig rt_config;
    rt_config.thread_name = "e2e_realtime_task";
    rt_config.cycle_period_ns = 10000000;  // 10ms (避免 CI 环境抖动)
    rt_config.thread_priority = 0;  // 普通优先级（示例环境）
    rt_config.cycle_callback = [&realtime_cycle_count]() {
        realtime_cycle_count.fetch_add(1, std::memory_order_relaxed);
    };
    
    TEST_ASSERT(executor.register_realtime_task("e2e_realtime_task", rt_config),
                "Realtime task registration should succeed");
    std::cout << "  Step 3: Registered realtime task" << std::endl;
    
    TEST_ASSERT(executor.start_realtime_task("e2e_realtime_task"),
                "Realtime task start should succeed");
    std::cout << "  Step 4: Started realtime task" << std::endl;
    
    // 运行一段时间让实时任务执行几个周期
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // 验证实时任务正在运行
    TEST_ASSERT(realtime_cycle_count.load() > 0, "Realtime task should have executed cycles");
    std::cout << "  Step 5: Realtime task executed " << realtime_cycle_count.load() << " cycles" << std::endl;
    
    // 4. 向实时任务推送任务
    std::atomic<bool> push_task_executed{false};
    auto* rt_executor = executor.get_realtime_executor("e2e_realtime_task");
    TEST_ASSERT(rt_executor != nullptr, "Realtime executor should be available");
    
    if (rt_executor) {
        rt_executor->push_task([&push_task_executed]() {
            push_task_executed.store(true);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        TEST_ASSERT(push_task_executed.load(), "Pushed task should be executed");
        std::cout << "  Step 6: Pushed task to realtime executor and executed" << std::endl;
    }
    
    // 5. 调用监控 API
    auto async_status = executor.get_async_executor_status();
    TEST_ASSERT(async_status.is_running, "Async executor should be running");
    TEST_ASSERT(async_status.completed_tasks >= num_tasks, "Completed tasks should match");
    std::cout << "  Step 7: Query async executor status - completed: " 
              << async_status.completed_tasks << std::endl;
    
    auto realtime_status = executor.get_realtime_executor_status("e2e_realtime_task");
    TEST_ASSERT(realtime_status.is_running, "Realtime executor should be running");
    TEST_ASSERT(realtime_status.cycle_count > 0, "Cycle count should be positive");
    std::cout << "  Step 8: Query realtime executor status - cycles: " 
              << realtime_status.cycle_count << std::endl;
    
    // 查询任务统计（如果监控已启用）
    if (config.enable_monitoring) {
        auto task_stats = executor.get_task_statistics("default");
        TEST_ASSERT(task_stats.total_count >= num_tasks, "Task statistics should reflect executed tasks");
        std::cout << "  Step 9: Query task statistics - total: " << task_stats.total_count << std::endl;
        
        auto all_stats = executor.get_all_task_statistics();
        TEST_ASSERT(!all_stats.empty(), "All task statistics should not be empty");
        std::cout << "  Step 10: Query all task statistics - types: " << all_stats.size() << std::endl;
    }
    
    // 6. 停止实时任务
    executor.stop_realtime_task("e2e_realtime_task");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    auto stopped_status = executor.get_realtime_executor_status("e2e_realtime_task");
    TEST_ASSERT(!stopped_status.is_running, "Realtime executor should be stopped");
    std::cout << "  Step 11: Stopped realtime task" << std::endl;
    
    // 7. 关闭 Executor
    executor.shutdown(true);
    std::cout << "  Step 12: Executor shut down gracefully" << std::endl;
    
    std::cout << "  Singleton workflow: PASSED" << std::endl;
    return true;
}

// ========== 实例化模式完整工作流测试 ==========

bool test_instance_workflow() {
    std::cout << "Testing instance mode complete workflow..." << std::endl;
    
    // 1. 创建独立的 Executor 实例
    Executor executor;
    ExecutorConfig config;
    config.min_threads = 2;
    config.max_threads = 4;
    config.queue_capacity = 100;
    config.enable_monitoring = true;
    
    TEST_ASSERT(executor.initialize(config), "Executor initialization should succeed");
    std::cout << "  Step 1: Executor initialized (instance mode)" << std::endl;
    
    // 2. 提交异步任务
    std::atomic<int> task_counter{0};
    const int num_tasks = 5;
    
    for (int i = 0; i < num_tasks; ++i) {
        executor.submit([i, &task_counter]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            task_counter.fetch_add(1);
            return i;
        });
    }
    
    (void)executor.wait_for_completion(std::chrono::seconds{300});
    TEST_ASSERT(task_counter.load() == num_tasks, "All tasks should be completed");
    std::cout << "  Step 2: Completed " << num_tasks << " async tasks" << std::endl;
    
    // 3. 注册并启动实时任务
    std::atomic<int> realtime_cycles{0};
    
    RealtimeThreadConfig rt_config;
    rt_config.thread_name = "instance_realtime";
    rt_config.cycle_period_ns = 20000000;  // 20ms
    rt_config.thread_priority = 0;
    rt_config.cycle_callback = [&realtime_cycles]() {
        realtime_cycles.fetch_add(1);
    };
    
    TEST_ASSERT(executor.register_realtime_task("instance_realtime", rt_config),
                "Realtime task registration should succeed");
    TEST_ASSERT(executor.start_realtime_task("instance_realtime"),
                "Realtime task start should succeed");
    std::cout << "  Step 3: Started realtime task" << std::endl;
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    TEST_ASSERT(realtime_cycles.load() > 0, "Realtime task should have executed");
    std::cout << "  Step 4: Realtime task executed " << realtime_cycles.load() << " cycles" << std::endl;
    
    // 4. 查询状态
    auto async_status = executor.get_async_executor_status();
    TEST_ASSERT(async_status.is_running, "Async executor should be running");
    
    auto realtime_status = executor.get_realtime_executor_status("instance_realtime");
    TEST_ASSERT(realtime_status.is_running, "Realtime executor should be running");
    std::cout << "  Step 5: Queried executor statuses" << std::endl;
    
    // 5. 停止并关闭
    executor.stop_realtime_task("instance_realtime");
    executor.shutdown(true);
    std::cout << "  Step 6: Executor shut down" << std::endl;
    
    std::cout << "  Instance workflow: PASSED" << std::endl;
    return true;
}

// ========== 主函数 ==========

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "End-to-End Workflow Test" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;
    
    bool all_passed = true;
    
    // 测试单例模式完整工作流
    std::cout << "--- Singleton Mode Workflow ---" << std::endl;
    all_passed &= test_singleton_workflow();
    std::cout << std::endl;
    
    // 测试实例化模式完整工作流
    std::cout << "--- Instance Mode Workflow ---" << std::endl;
    all_passed &= test_instance_workflow();
    std::cout << std::endl;
    
    if (all_passed) {
        std::cout << "========================================" << std::endl;
        std::cout << "All E2E workflow tests PASSED" << std::endl;
        std::cout << "========================================" << std::endl;
        return 0;
    } else {
        std::cerr << "========================================" << std::endl;
        std::cerr << "Some E2E workflow tests FAILED" << std::endl;
        std::cerr << "========================================" << std::endl;
        return 1;
    }
}
