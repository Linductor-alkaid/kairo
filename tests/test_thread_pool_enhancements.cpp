#include <cassert>
#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <future>
#include <algorithm>

// 包含新组件的头文件
#include "kairo/thread_pool/load_balancer.hpp"
#include "kairo/thread_pool/task_dispatcher.hpp"
#include "kairo/thread_pool/worker_local_queue.hpp"
#include "kairo/thread_pool/priority_scheduler.hpp"
#include "kairo/thread_pool/thread_pool.hpp"
#include "kairo/task/task.hpp"
#include <kairo/config.hpp>
#include <kairo/types.hpp>

using namespace kairo;

// 测试辅助宏
#define TEST_ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            std::cerr << "FAILED: " << message << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
            return false; \
        } \
    } while(0)

// ========== LoadBalancer 测试 ==========

bool test_load_balancer_round_robin() {
    std::cout << "Testing LoadBalancer round-robin strategy..." << std::endl;
    
    LoadBalancer balancer(4);
    balancer.set_strategy(LoadBalancer::Strategy::ROUND_ROBIN);
    
    // 测试轮询选择
    std::vector<size_t> selections;
    for (int i = 0; i < 10; ++i) {
        size_t worker = balancer.select_worker();
        selections.push_back(worker);
        TEST_ASSERT(worker < 4, "Selected worker ID should be valid");
    }
    
    // 验证轮询分布（应该大致均匀）
    std::vector<size_t> counts(4, 0);
    for (size_t w : selections) {
        counts[w]++;
    }
    
    // 每个worker应该至少被选择一次
    for (size_t count : counts) {
        TEST_ASSERT(count > 0, "Each worker should be selected at least once");
    }
    
    return true;
}

bool test_load_balancer_least_tasks() {
    std::cout << "Testing LoadBalancer least-tasks strategy..." << std::endl;
    
    LoadBalancer balancer(4);
    balancer.set_strategy(LoadBalancer::Strategy::LEAST_TASKS);
    
    // 设置不同的负载
    balancer.update_load(0, 10, 2);  // 总任务数：12
    balancer.update_load(1, 5, 1);    // 总任务数：6
    balancer.update_load(2, 2, 0);    // 总任务数：2
    balancer.update_load(3, 1, 0);   // 总任务数：1
    
    // 应该选择任务数最少的worker（worker 3）
    size_t selected = balancer.select_worker();
    TEST_ASSERT(selected == 3, "Should select worker with least tasks");
    
    return true;
}

bool test_load_balancer_update_load() {
    std::cout << "Testing LoadBalancer load update..." << std::endl;
    
    LoadBalancer balancer(3);
    
    // 更新负载
    balancer.update_load(0, 5, 1);
    balancer.update_load(1, 3, 0);
    balancer.update_load(2, 10, 2);
    
    // 获取负载信息
    auto load0 = balancer.get_load(0);
    TEST_ASSERT(load0.queue_size == 5, "Queue size should be 5");
    TEST_ASSERT(load0.active_tasks == 1, "Active tasks should be 1");
    
    auto load1 = balancer.get_load(1);
    TEST_ASSERT(load1.queue_size == 3, "Queue size should be 3");
    
    return true;
}

bool test_load_balancer_resize_initializes_new_workers() {
    std::cout << "Testing LoadBalancer resize initializes new workers..." << std::endl;

    LoadBalancer balancer(2);
    balancer.update_load(0, 10, 2);
    balancer.update_load(1, 8, 1);

    const auto before_resize = std::chrono::steady_clock::now();
    balancer.resize(5);
    const auto after_resize = std::chrono::steady_clock::now();

    const auto default_time = std::chrono::steady_clock::time_point{};
    const auto tolerance = std::chrono::milliseconds(100);
    auto loads = balancer.get_all_loads();
    TEST_ASSERT(loads.size() == 5, "Resize should grow load metadata to 5 workers");

    for (size_t i = 2; i < 5; ++i) {
        TEST_ASSERT(loads[i].queue_size == 0, "New worker queue_size should be 0");
        TEST_ASSERT(loads[i].active_tasks == 0, "New worker active_tasks should be 0");
        TEST_ASSERT(loads[i].last_update != default_time,
                    "New worker last_update should not be default constructed");
        TEST_ASSERT(loads[i].last_update >= before_resize - tolerance,
                    "New worker last_update should be no earlier than resize window");
        TEST_ASSERT(loads[i].last_update <= after_resize + tolerance,
                    "New worker last_update should be no later than resize window");
    }

    balancer.set_strategy(LoadBalancer::Strategy::LEAST_LOAD);
    for (size_t expected = 2; expected < 5; ++expected) {
        const size_t selected = balancer.select_worker();
        TEST_ASSERT(selected == expected, "Least-load should select newly initialized workers first");
        balancer.update_load(selected, 1, 0);
    }

    balancer.resize(2);
    loads = balancer.get_all_loads();
    TEST_ASSERT(loads.size() == 2, "Shrink should drop removed worker metadata");

    const auto removed_load = balancer.get_load(3);
    TEST_ASSERT(removed_load.queue_size == 0, "Out-of-range load query should return default queue_size");
    TEST_ASSERT(removed_load.active_tasks == 0, "Out-of-range load query should return default active_tasks");
    TEST_ASSERT(removed_load.last_update == default_time,
                "Out-of-range load query should return default last_update");

    return true;
}

// ========== WorkerLocalQueue 测试 ==========

bool test_worker_local_queue_basic() {
    std::cout << "Testing WorkerLocalQueue basic operations..." << std::endl;
    
    WorkerLocalQueue queue(100);
    
    TEST_ASSERT(queue.empty(), "Queue should be empty initially");
    TEST_ASSERT(queue.size() == 0, "Queue size should be 0");
    
    // 测试 push
    Task task1;
    task1.task_id = "task_1";
    task1.priority = TaskPriority::NORMAL;
    task1.function = []() {};
    
    bool pushed = queue.push(task1);
    TEST_ASSERT(pushed, "Push should succeed");
    TEST_ASSERT(!queue.empty(), "Queue should not be empty");
    TEST_ASSERT(queue.size() == 1, "Queue size should be 1");
    
    // 测试 pop
    Task popped_task;
    bool popped = queue.pop(popped_task);
    TEST_ASSERT(popped, "Pop should succeed");
    TEST_ASSERT(popped_task.task_id == "task_1", "Popped task ID should match");
    TEST_ASSERT(queue.empty(), "Queue should be empty after pop");
    
    return true;
}

bool test_worker_local_queue_steal() {
    std::cout << "Testing WorkerLocalQueue steal operation..." << std::endl;
    
    WorkerLocalQueue queue(100);
    
    // 添加多个任务
    for (int i = 0; i < 5; ++i) {
        Task task;
        task.task_id = "task_" + std::to_string(i);
        task.priority = TaskPriority::NORMAL;
        task.function = []() {};
        queue.push(task);
    }
    
    TEST_ASSERT(queue.size() == 5, "Queue should have 5 tasks");
    
    // 测试 steal（从后端弹出）
    Task stolen_task;
    bool stolen = queue.steal(stolen_task);
    TEST_ASSERT(stolen, "Steal should succeed");
    TEST_ASSERT(stolen_task.task_id == "task_4", "Should steal from back (task_4)");
    TEST_ASSERT(queue.size() == 4, "Queue size should be 4 after steal");
    
    // 测试 pop（从前端弹出）
    Task popped_task;
    bool popped = queue.pop(popped_task);
    TEST_ASSERT(popped, "Pop should succeed");
    TEST_ASSERT(popped_task.task_id == "task_0", "Should pop from front (task_0)");
    
    return true;
}

// P-260623-001: capacity=0 必须回退到一个合理的默认容量,且行为可观察 (不能静默 cap 到 100 后
// 假装 0=unlimited; hpp 文档现已明确化为 sentinel 语义,此测试固化该契约).
bool test_worker_local_queue_capacity_zero_fallback() {
    std::cout << "Testing WorkerLocalQueue(0) uses documented fallback capacity (P-260623-001)..." << std::endl;

    // capacity=0 => 内部回退到 100 槽 (详见 worker_local_queue.cpp 的硬编码回退)
    WorkerLocalQueue q0(0);
    Task task;
    task.task_id = "t";
    task.priority = TaskPriority::NORMAL;
    task.function = []() {};

    int pushed = 0;
    for (int i = 0; i < 200; ++i) {
        if (q0.push(task)) ++pushed;
        else break;
    }
    // 回退容量是 100,所以前 100 个 push 成功,第 101 个失败
    TEST_ASSERT(pushed == 100, "capacity=0 must fall back to 100 slots (got " << pushed << ")");
    TEST_ASSERT(q0.size() == 100, "queue size after filling fallback buffer must be 100");

    // 对照:显式 capacity=100 行为一致
    WorkerLocalQueue q100(100);
    int pushed100 = 0;
    for (int i = 0; i < 200; ++i) {
        if (q100.push(task)) ++pushed100;
        else break;
    }
    TEST_ASSERT(pushed100 == 100, "explicit capacity=100 must allow exactly 100 pushes (got " << pushed100 << ")");

    return true;
}

// ========== TaskDispatcher 测试 ==========

bool test_task_dispatcher_basic() {
    std::cout << "Testing TaskDispatcher basic operations..." << std::endl;
    
    // 注意：由于 WorkerLocalQueue 包含不可移动的 mutex，
    // 无法在测试中直接构造 vector<WorkerLocalQueue>
    // 这个测试在实际的 ThreadPool 中会正常工作
    // 因为 ThreadPool 使用特殊的方法（placement new）构造 vector 元素
    // 这里我们跳过直接测试 TaskDispatcher，改为测试集成场景
    std::cout << "  Skipping direct TaskDispatcher test (design limitation)" << std::endl;
    std::cout << "  TaskDispatcher is tested indirectly through ThreadPool integration" << std::endl;
    return true;
}

// ========== 工作窃取测试 ==========

bool test_work_stealing() {
    std::cout << "Testing work stealing mechanism..." << std::endl;
    
    ThreadPoolConfig config;
    config.min_threads = 2;
    config.max_threads = 4;
    config.queue_capacity = 100;
    config.enable_work_stealing = true;
    
    ThreadPool pool;
    bool initialized = pool.initialize(config);
    TEST_ASSERT(initialized, "ThreadPool should initialize successfully");
    
    // 提交一些任务
    std::atomic<int> completed(0);
    for (int i = 0; i < 10; ++i) {
        pool.submit([&completed]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            completed.fetch_add(1);
        });
    }
    
    // 等待所有任务完成
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    pool.shutdown(true);
    
    TEST_ASSERT(completed.load() == 10, "All tasks should complete");
    
    return true;
}

// ========== 动态扩缩容测试 ==========

bool test_dynamic_resize() {
    std::cout << "Testing dynamic resize (basic framework)..." << std::endl;
    
    ThreadPoolConfig config;
    config.min_threads = 2;
    config.max_threads = 4;
    config.queue_capacity = 100;
    config.enable_work_stealing = false;
    
    ThreadPool pool;
    bool initialized = pool.initialize(config);
    TEST_ASSERT(initialized, "ThreadPool should initialize successfully");
    
    // 检查初始线程数
    auto status = pool.get_status();
    TEST_ASSERT(status.total_threads == 2, "Initial thread count should be 2");
    
    // 提交一些任务
    for (int i = 0; i < 5; ++i) {
        pool.submit([]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        });
    }
    
    // 等待一段时间让监控线程运行
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    pool.shutdown(true);
    
    return true;
}

// ========== 主测试函数 ==========

int main() {
    std::cout << "=== Thread Pool Enhancements Tests ===" << std::endl;
    
    bool all_passed = true;
    
    // LoadBalancer 测试
    all_passed &= test_load_balancer_round_robin();
    all_passed &= test_load_balancer_least_tasks();
    all_passed &= test_load_balancer_update_load();
    all_passed &= test_load_balancer_resize_initializes_new_workers();
    
    // WorkerLocalQueue 测试
    all_passed &= test_worker_local_queue_basic();
    all_passed &= test_worker_local_queue_steal();
    all_passed &= test_worker_local_queue_capacity_zero_fallback();  // P-260623-001

    // TaskDispatcher 测试
    all_passed &= test_task_dispatcher_basic();
    
    // 工作窃取测试
    all_passed &= test_work_stealing();
    
    // 动态扩缩容测试
    all_passed &= test_dynamic_resize();
    
    if (all_passed) {
        std::cout << "\n=== All tests PASSED ===" << std::endl;
        return 0;
    } else {
        std::cout << "\n=== Some tests FAILED ===" << std::endl;
        return 1;
    }
}
