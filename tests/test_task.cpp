#include <cassert>
#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <algorithm>

// 包含 task 模块的头文件
#include <kairo/types.hpp>
#include "kairo/task/task.hpp"
#include "kairo/task/task_dependency_manager.hpp"

using namespace kairo;

// 测试辅助宏
#define TEST_ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            std::cerr << "FAILED: " << message << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
            return false; \
        } \
    } while(0)

// ========== Task 测试 ==========

bool test_task_basic() {
    std::cout << "Testing Task basic operations..." << std::endl;
    
    Task task;
    task.task_id = "test_task_1";
    task.priority = TaskPriority::HIGH;
    task.submit_time_ns = 1000;
    
    TEST_ASSERT(task.task_id == "test_task_1", "Task ID should match");
    TEST_ASSERT(task.priority == TaskPriority::HIGH, "Task priority should match");
    TEST_ASSERT(task.submit_time_ns == 1000, "Submit time should match");
    TEST_ASSERT(!is_task_cancelled(task), "Task should not be cancelled initially");
    
    std::cout << "  Task basic operations: PASSED" << std::endl;
    return true;
}

bool test_task_comparison() {
    std::cout << "Testing Task comparison operators..." << std::endl;
    
    Task task1, task2, task3, task4;
    
    // 测试优先级比较
    task1.priority = TaskPriority::CRITICAL;
    task1.submit_time_ns = 1000;
    task2.priority = TaskPriority::HIGH;
    task2.submit_time_ns = 500;
    
    // CRITICAL > HIGH，所以 task1 < task2 应该为 false（task1 优先级更高）
    TEST_ASSERT(!(task1 < task2), "CRITICAL priority should be higher than HIGH");
    TEST_ASSERT(task2 < task1, "HIGH priority should be lower than CRITICAL");
    
    // 测试同优先级按时间排序
    task3.priority = TaskPriority::NORMAL;
    task3.submit_time_ns = 1000;
    task4.priority = TaskPriority::NORMAL;
    task4.submit_time_ns = 2000;
    
    // 同优先级，提交时间早的优先（submit_time_ns 小的优先）
    // task3 < task4 应该为 false（task3 时间更早，应该优先）
    TEST_ASSERT(!(task3 < task4), "Earlier submit time should have higher priority");
    TEST_ASSERT(task4 < task3, "Later submit time should have lower priority");
    
    std::cout << "  Task comparison operators: PASSED" << std::endl;
    return true;
}

bool test_task_cancellation() {
    std::cout << "Testing Task cancellation..." << std::endl;
    
    Task task;
    task.task_id = "test_task_1";
    
    TEST_ASSERT(!is_task_cancelled(task), "Task should not be cancelled initially");
    
    cancel_task(task);
    TEST_ASSERT(is_task_cancelled(task), "Task should be cancelled after cancel_task");
    
    std::cout << "  Task cancellation: PASSED" << std::endl;
    return true;
}

bool test_task_cancellation_thread_safe() {
    std::cout << "Testing Task cancellation thread safety..." << std::endl;
    
    Task task;
    task.task_id = "test_task_1";
    
    const int num_threads = 10;
    const int operations_per_thread = 100;
    std::vector<std::thread> threads;
    std::atomic<int> cancel_count{0};
    std::atomic<int> check_count{0};
    
    // 多个线程同时取消和检查
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&]() {
            for (int j = 0; j < operations_per_thread; ++j) {
                if (j % 2 == 0) {
                    cancel_task(task);
                    cancel_count.fetch_add(1);
                } else {
                    is_task_cancelled(task);
                    check_count.fetch_add(1);
                }
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    TEST_ASSERT(cancel_count.load() == num_threads * operations_per_thread / 2,
               "All cancel operations should complete");
    TEST_ASSERT(check_count.load() == num_threads * operations_per_thread / 2,
               "All check operations should complete");
    TEST_ASSERT(is_task_cancelled(task), "Task should be cancelled after all operations");
    
    std::cout << "  Task cancellation thread safety: PASSED" << std::endl;
    return true;
}

bool test_generate_task_id() {
    std::cout << "Testing generate_task_id..." << std::endl;
    
    std::string id1 = generate_task_id();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::string id2 = generate_task_id();
    
    TEST_ASSERT(!id1.empty(), "Generated task ID should not be empty");
    TEST_ASSERT(!id2.empty(), "Generated task ID should not be empty");
    TEST_ASSERT(id1 != id2, "Generated task IDs should be unique");
    TEST_ASSERT(id1.find("task_") == 0, "Task ID should start with 'task_'");
    TEST_ASSERT(id2.find("task_") == 0, "Task ID should start with 'task_'");
    
    std::cout << "  generate_task_id: PASSED" << std::endl;
    return true;
}

// ========== TaskDependencyManager 测试 ==========

bool test_dependency_manager_basic() {
    std::cout << "Testing TaskDependencyManager basic operations..." << std::endl;
    
    TaskDependencyManager manager;
    
    // 添加依赖
    TEST_ASSERT(manager.add_dependency("task_b", "task_a"), 
               "Should be able to add dependency");
    
    // 检查依赖
    auto deps = manager.get_dependencies("task_b");
    TEST_ASSERT(deps.size() == 1, "Should have one dependency");
    TEST_ASSERT(deps[0] == "task_a", "Dependency should be task_a");
    
    // 检查是否可执行
    TEST_ASSERT(!manager.is_ready("task_b"), 
               "task_b should not be ready (task_a not completed)");
    TEST_ASSERT(manager.is_ready("task_a"), 
               "task_a should be ready (no dependencies)");
    
    // 标记完成
    manager.mark_completed("task_a");
    TEST_ASSERT(manager.is_ready("task_b"), 
               "task_b should be ready (task_a completed)");
    
    std::cout << "  TaskDependencyManager basic operations: PASSED" << std::endl;
    return true;
}

bool test_dependency_manager_multiple_dependencies() {
    std::cout << "Testing TaskDependencyManager multiple dependencies..." << std::endl;
    
    TaskDependencyManager manager;
    
    // task_c 依赖于 task_a 和 task_b
    TEST_ASSERT(manager.add_dependency("task_c", "task_a"), 
               "Should be able to add first dependency");
    TEST_ASSERT(manager.add_dependency("task_c", "task_b"), 
               "Should be able to add second dependency");
    
    TEST_ASSERT(!manager.is_ready("task_c"), 
               "task_c should not be ready (dependencies not completed)");
    
    // 只完成一个依赖
    manager.mark_completed("task_a");
    TEST_ASSERT(!manager.is_ready("task_c"), 
               "task_c should not be ready (task_b not completed)");
    
    // 完成所有依赖
    manager.mark_completed("task_b");
    TEST_ASSERT(manager.is_ready("task_c"), 
               "task_c should be ready (all dependencies completed)");
    
    std::cout << "  TaskDependencyManager multiple dependencies: PASSED" << std::endl;
    return true;
}

bool test_dependency_manager_no_dependencies() {
    std::cout << "Testing TaskDependencyManager no dependencies..." << std::endl;
    
    TaskDependencyManager manager;
    
    // 没有依赖的任务应该可以直接执行
    TEST_ASSERT(manager.is_ready("task_x"), 
               "Task without dependencies should be ready");
    
    // 添加依赖后不应该可执行
    manager.add_dependency("task_y", "task_z");
    TEST_ASSERT(!manager.is_ready("task_y"), 
               "Task with uncompleted dependency should not be ready");
    
    std::cout << "  TaskDependencyManager no dependencies: PASSED" << std::endl;
    return true;
}

bool test_dependency_manager_cycle_detection() {
    std::cout << "Testing TaskDependencyManager cycle detection..." << std::endl;
    
    TaskDependencyManager manager;
    
    // 直接循环：task_a -> task_a
    TEST_ASSERT(!manager.add_dependency("task_a", "task_a"), 
               "Should not allow self-dependency");
    
    // 简单循环：task_a -> task_b -> task_a
    TEST_ASSERT(manager.add_dependency("task_a", "task_b"), 
               "Should be able to add first dependency");
    TEST_ASSERT(!manager.add_dependency("task_b", "task_a"), 
               "Should not allow cycle (task_b -> task_a creates cycle)");
    
    // 复杂循环：task_a -> task_b -> task_c -> task_a
    manager.clear();
    TEST_ASSERT(manager.add_dependency("task_a", "task_b"), 
               "Should be able to add first dependency");
    TEST_ASSERT(manager.add_dependency("task_b", "task_c"), 
               "Should be able to add second dependency");
    TEST_ASSERT(!manager.add_dependency("task_c", "task_a"), 
               "Should not allow cycle (task_c -> task_a creates cycle)");
    
    std::cout << "  TaskDependencyManager cycle detection: PASSED" << std::endl;
    return true;
}

bool test_dependency_manager_clear() {
    std::cout << "Testing TaskDependencyManager clear..." << std::endl;
    
    TaskDependencyManager manager;
    
    manager.add_dependency("task_b", "task_a");
    manager.mark_completed("task_a");
    
    TEST_ASSERT(manager.is_ready("task_b"), 
               "task_b should be ready before clear");
    
    manager.clear();
    
    // 清除后，所有任务都应该可以直接执行（没有依赖）
    TEST_ASSERT(manager.is_ready("task_b"), 
               "task_b should be ready after clear (no dependencies)");
    TEST_ASSERT(!manager.is_completed("task_a"), 
               "task_a should not be marked as completed after clear");
    
    auto deps = manager.get_dependencies("task_b");
    TEST_ASSERT(deps.empty(), "task_b should have no dependencies after clear");
    
    std::cout << "  TaskDependencyManager clear: PASSED" << std::endl;
    return true;
}

bool test_dependency_manager_is_completed() {
    std::cout << "Testing TaskDependencyManager is_completed..." << std::endl;
    
    TaskDependencyManager manager;
    
    TEST_ASSERT(!manager.is_completed("task_a"), 
               "Task should not be completed initially");
    
    manager.mark_completed("task_a");
    TEST_ASSERT(manager.is_completed("task_a"), 
               "Task should be completed after mark_completed");
    
    std::cout << "  TaskDependencyManager is_completed: PASSED" << std::endl;
    return true;
}

bool test_dependency_manager_edge_cases() {
    std::cout << "Testing TaskDependencyManager edge cases..." << std::endl;
    
    TaskDependencyManager manager;
    
    // 空任务ID
    TEST_ASSERT(!manager.add_dependency("", "task_a"), 
               "Should not allow empty task_id");
    TEST_ASSERT(!manager.add_dependency("task_a", ""), 
               "Should not allow empty depends_on");
    TEST_ASSERT(!manager.is_ready(""), 
               "Should return false for empty task_id");
    TEST_ASSERT(!manager.is_completed(""), 
               "Should return false for empty task_id");
    
    // 不存在的任务
    TEST_ASSERT(manager.is_ready("nonexistent_task"), 
               "Nonexistent task should be ready (no dependencies)");
    TEST_ASSERT(!manager.is_completed("nonexistent_task"), 
               "Nonexistent task should not be completed");
    
    // 重复添加相同依赖
    TEST_ASSERT(manager.add_dependency("task_b", "task_a"), 
               "Should be able to add dependency");
    TEST_ASSERT(manager.add_dependency("task_b", "task_a"), 
               "Should be able to add same dependency again (idempotent)");
    
    auto deps = manager.get_dependencies("task_b");
    TEST_ASSERT(deps.size() == 1, "Should have only one dependency (no duplicates)");
    
    std::cout << "  TaskDependencyManager edge cases: PASSED" << std::endl;
    return true;
}

bool test_dependency_manager_concurrent() {
    std::cout << "Testing TaskDependencyManager concurrent operations..." << std::endl;
    
    TaskDependencyManager manager;
    const int num_threads = 10;
    const int operations_per_thread = 100;
    std::vector<std::thread> threads;
    std::atomic<int> add_count{0};
    std::atomic<int> ready_count{0};
    std::atomic<int> complete_count{0};
    
    // 多个线程同时操作
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&, i]() {
            for (int j = 0; j < operations_per_thread; ++j) {
                std::string task_id = "task_" + std::to_string(i) + "_" + std::to_string(j);
                std::string dep_id = "dep_" + std::to_string(i) + "_" + std::to_string(j);
                
                // 添加依赖
                if (manager.add_dependency(task_id, dep_id)) {
                    add_count.fetch_add(1);
                }
                
                // 检查是否可执行
                manager.is_ready(task_id);
                ready_count.fetch_add(1);
                
                // 标记完成
                manager.mark_completed(dep_id);
                complete_count.fetch_add(1);
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    TEST_ASSERT(add_count.load() > 0, "Some dependencies should be added");
    TEST_ASSERT(ready_count.load() == num_threads * operations_per_thread,
               "All ready checks should complete");
    TEST_ASSERT(complete_count.load() == num_threads * operations_per_thread,
               "All complete operations should complete");
    
    std::cout << "  TaskDependencyManager concurrent operations: PASSED" << std::endl;
    return true;
}

bool test_dependency_manager_complex_scenario() {
    std::cout << "Testing TaskDependencyManager complex scenario..." << std::endl;
    
    TaskDependencyManager manager;
    
    // 构建一个复杂的依赖图：
    // task_a (无依赖)
    // task_b -> task_a
    // task_c -> task_a, task_b
    // task_d -> task_c
    // task_e (无依赖)
    // task_f -> task_d, task_e
    
    TEST_ASSERT(manager.add_dependency("task_b", "task_a"), 
               "Should add task_b -> task_a");
    TEST_ASSERT(manager.add_dependency("task_c", "task_a"), 
               "Should add task_c -> task_a");
    TEST_ASSERT(manager.add_dependency("task_c", "task_b"), 
               "Should add task_c -> task_b");
    TEST_ASSERT(manager.add_dependency("task_d", "task_c"), 
               "Should add task_d -> task_c");
    TEST_ASSERT(manager.add_dependency("task_f", "task_d"), 
               "Should add task_f -> task_d");
    TEST_ASSERT(manager.add_dependency("task_f", "task_e"), 
               "Should add task_f -> task_e");
    
    // 初始状态
    TEST_ASSERT(manager.is_ready("task_a"), "task_a should be ready");
    TEST_ASSERT(manager.is_ready("task_e"), "task_e should be ready");
    TEST_ASSERT(!manager.is_ready("task_b"), "task_b should not be ready");
    TEST_ASSERT(!manager.is_ready("task_c"), "task_c should not be ready");
    TEST_ASSERT(!manager.is_ready("task_d"), "task_d should not be ready");
    TEST_ASSERT(!manager.is_ready("task_f"), "task_f should not be ready");
    
    // 完成 task_a
    manager.mark_completed("task_a");
    TEST_ASSERT(manager.is_ready("task_b"), "task_b should be ready after task_a completes");
    TEST_ASSERT(!manager.is_ready("task_c"), "task_c should not be ready (task_b not completed)");
    
    // 完成 task_b
    manager.mark_completed("task_b");
    TEST_ASSERT(manager.is_ready("task_c"), "task_c should be ready after task_b completes");
    
    // 完成 task_c
    manager.mark_completed("task_c");
    TEST_ASSERT(manager.is_ready("task_d"), "task_d should be ready after task_c completes");
    
    // 完成 task_d 和 task_e
    manager.mark_completed("task_d");
    manager.mark_completed("task_e");
    TEST_ASSERT(manager.is_ready("task_f"), "task_f should be ready after all dependencies complete");
    
    std::cout << "  TaskDependencyManager complex scenario: PASSED" << std::endl;
    return true;
}

// ========== 主测试函数 ==========

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Executor Task Module Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;
    
    bool all_passed = true;
    
    // Task 测试
    std::cout << "--- Task Tests ---" << std::endl;
    all_passed &= test_task_basic();
    all_passed &= test_task_comparison();
    all_passed &= test_task_cancellation();
    all_passed &= test_task_cancellation_thread_safe();
    all_passed &= test_generate_task_id();
    std::cout << std::endl;
    
    // TaskDependencyManager 测试
    std::cout << "--- TaskDependencyManager Tests ---" << std::endl;
    all_passed &= test_dependency_manager_basic();
    all_passed &= test_dependency_manager_multiple_dependencies();
    all_passed &= test_dependency_manager_no_dependencies();
    all_passed &= test_dependency_manager_cycle_detection();
    all_passed &= test_dependency_manager_clear();
    all_passed &= test_dependency_manager_is_completed();
    all_passed &= test_dependency_manager_edge_cases();
    all_passed &= test_dependency_manager_concurrent();
    all_passed &= test_dependency_manager_complex_scenario();
    std::cout << std::endl;
    
    // 总结
    std::cout << "========================================" << std::endl;
    std::cout << "Test Summary:" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;
    
    if (all_passed) {
        std::cout << "All tests PASSED!" << std::endl;
        return 0;
    } else {
        std::cout << "Some tests FAILED!" << std::endl;
        return 1;
    }
}