/**
 * @file multi_project.cpp
 * @brief 多项目/多模块使用示例
 *
 * 本示例演示 Executor 在不同使用场景下的行为：
 * 1. 场景 1：单例共享模式 - 同一进程内多个模块共享同一个 Executor 实例
 * 2. 场景 2：实例隔离模式 - 不同项目使用独立的 Executor 实例，资源隔离
 * 3. 场景 3：混合模式 - 全局单例处理通用任务，独立实例处理特殊任务
 */

#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <atomic>
#include <kairo/executor.hpp>

using namespace kairo;

// ========== 场景 1：单例共享模式 ==========

void scenario1_shared_singleton() {
    std::cout << "========================================" << std::endl;
    std::cout << "Scenario 1: Shared Singleton Mode" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;
    
    // 主模块初始化全局 Executor（单例模式）
    ExecutorConfig global_config;
    global_config.min_threads = 4;
    global_config.max_threads = 8;
    global_config.queue_capacity = 1000;
    
    auto& global_executor = Executor::instance();
    if (!global_executor.initialize(global_config)) {
        std::cerr << "Failed to initialize global executor" << std::endl;
        return;
    }
    
    std::cout << "Global executor initialized (singleton mode)" << std::endl;
    std::cout << "  Config: min_threads=" << global_config.min_threads 
              << ", max_threads=" << global_config.max_threads << std::endl;
    std::cout << std::endl;
    
    // 模拟模块 A 使用全局 Executor
    std::atomic<int> module_a_tasks{0};
    std::cout << "Module A: Submitting tasks via Executor::instance()..." << std::endl;
    
    for (int i = 0; i < 5; ++i) {
        Executor::instance().submit([i, &module_a_tasks]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            module_a_tasks.fetch_add(1);
            std::cout << "  [Module A] Task " << i << " completed" << std::endl;
        });
    }
    
    // 模拟模块 B 使用全局 Executor（共享同一个实例）
    std::atomic<int> module_b_tasks{0};
    std::cout << "Module B: Submitting tasks via Executor::instance()..." << std::endl;
    
    for (int i = 0; i < 5; ++i) {
        Executor::instance().submit([i, &module_b_tasks]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            module_b_tasks.fetch_add(1);
            std::cout << "  [Module B] Task " << i << " completed" << std::endl;
        });
    }
    
    // 等待所有任务完成
    (void)Executor::instance().wait_for_completion(std::chrono::seconds{300});
    
    std::cout << std::endl;
    std::cout << "Module A completed tasks: " << module_a_tasks.load() << std::endl;
    std::cout << "Module B completed tasks: " << module_b_tasks.load() << std::endl;
    
    // 查询全局状态
    auto status = Executor::instance().get_async_executor_status();
    std::cout << "Global executor status:" << std::endl;
    std::cout << "  Completed tasks: " << status.completed_tasks << std::endl;
    std::cout << "  Queue size: " << status.queue_size << std::endl;
    std::cout << std::endl;
    
    std::cout << "Key point: Module A and Module B share the same thread pool" << std::endl;
    std::cout << std::endl;
    
    Executor::instance().shutdown();
}

// ========== 场景 2：实例隔离模式 ==========

class ProjectA {
private:
    Executor executor_;
    
public:
    ProjectA() {
        ExecutorConfig config;
        config.min_threads = 2;
        config.max_threads = 4;
        config.queue_capacity = 500;
        executor_.initialize(config);
    }
    
    void do_work() {
        std::atomic<int> tasks{0};
        std::cout << "  [Project A] Submitting tasks..." << std::endl;
        
        for (int i = 0; i < 5; ++i) {
            executor_.submit([i, &tasks]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                tasks.fetch_add(1);
                std::cout << "    [Project A] Task " << i << " completed" << std::endl;
            });
        }
        
        (void)executor_.wait_for_completion(std::chrono::seconds{300});
        std::cout << "  [Project A] Completed " << tasks.load() << " tasks" << std::endl;
        
        auto status = executor_.get_async_executor_status();
        std::cout << "  [Project A] Executor status: completed=" << status.completed_tasks 
                  << ", queue=" << status.queue_size << std::endl;
    }
    
    ~ProjectA() {
        executor_.shutdown();
    }
};

class ProjectB {
private:
    Executor executor_;
    
public:
    ProjectB() {
        ExecutorConfig config;
        config.min_threads = 3;
        config.max_threads = 6;
        config.queue_capacity = 800;
        executor_.initialize(config);
    }
    
    void do_work() {
        std::atomic<int> tasks{0};
        std::cout << "  [Project B] Submitting tasks..." << std::endl;
        
        for (int i = 0; i < 5; ++i) {
            executor_.submit([i, &tasks]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                tasks.fetch_add(1);
                std::cout << "    [Project B] Task " << i << " completed" << std::endl;
            });
        }
        
        (void)executor_.wait_for_completion(std::chrono::seconds{300});
        std::cout << "  [Project B] Completed " << tasks.load() << " tasks" << std::endl;
        
        auto status = executor_.get_async_executor_status();
        std::cout << "  [Project B] Executor status: completed=" << status.completed_tasks 
                  << ", queue=" << status.queue_size << std::endl;
    }
    
    ~ProjectB() {
        executor_.shutdown();
    }
};

void scenario2_instance_isolation() {
    std::cout << "========================================" << std::endl;
    std::cout << "Scenario 2: Instance Isolation Mode" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;
    
    std::cout << "Creating Project A with independent Executor..." << std::endl;
    ProjectA project_a;
    std::cout << "  Project A config: min_threads=2, max_threads=4" << std::endl;
    
    std::cout << std::endl;
    std::cout << "Creating Project B with independent Executor..." << std::endl;
    ProjectB project_b;
    std::cout << "  Project B config: min_threads=3, max_threads=6" << std::endl;
    std::cout << std::endl;
    
    // 两个项目可以并行工作，互不影响
    std::cout << "Running Project A and Project B in parallel..." << std::endl;
    std::thread thread_a([&project_a]() { project_a.do_work(); });
    std::thread thread_b([&project_b]() { project_b.do_work(); });
    
    thread_a.join();
    thread_b.join();
    
    std::cout << std::endl;
    std::cout << "Key point: Project A and Project B have independent thread pools" << std::endl;
    std::cout << "  - Each project can have different configurations" << std::endl;
    std::cout << "  - Resources are completely isolated" << std::endl;
    std::cout << std::endl;
}

// ========== 场景 3：混合模式 ==========

void scenario3_hybrid_mode() {
    std::cout << "========================================" << std::endl;
    std::cout << "Scenario 3: Hybrid Mode" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;
    
    // 全局共享的 Executor（单例模式）- 用于通用任务
    ExecutorConfig global_config;
    global_config.min_threads = 4;
    global_config.max_threads = 8;
    global_config.queue_capacity = 1000;
    
    auto& global_executor = Executor::instance();
    if (!global_executor.initialize(global_config)) {
        std::cerr << "Failed to initialize global executor" << std::endl;
        return;
    }
    
    std::cout << "Global executor initialized (singleton)" << std::endl;
    std::cout << "  Config: min_threads=4, max_threads=8" << std::endl;
    std::cout << std::endl;
    
    // 特殊模块：创建独立的 Executor 实例 - 用于特殊任务
    Executor special_executor;
    ExecutorConfig special_config;
    special_config.min_threads = 2;
    special_config.max_threads = 4;
    special_config.queue_capacity = 500;
    special_config.thread_priority = 0;  // 可以设置不同的优先级
    
    if (!special_executor.initialize(special_config)) {
        std::cerr << "Failed to initialize special executor" << std::endl;
        global_executor.shutdown();
        return;
    }
    
    std::cout << "Special executor initialized (instance mode)" << std::endl;
    std::cout << "  Config: min_threads=2, max_threads=4" << std::endl;
    std::cout << std::endl;
    
    // 通用任务：使用全局 Executor
    std::atomic<int> general_tasks{0};
    std::cout << "Submitting general tasks to global executor..." << std::endl;
    
    for (int i = 0; i < 5; ++i) {
        global_executor.submit([i, &general_tasks]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            general_tasks.fetch_add(1);
            std::cout << "  [General] Task " << i << " completed" << std::endl;
        });
    }
    
    // 特殊任务：使用独立的 Executor
    std::atomic<int> special_tasks{0};
    std::cout << "Submitting special tasks to special executor..." << std::endl;
    
    for (int i = 0; i < 5; ++i) {
        special_executor.submit([i, &special_tasks]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            special_tasks.fetch_add(1);
            std::cout << "  [Special] Task " << i << " completed" << std::endl;
        });
    }
    
    // 等待所有任务完成
    (void)global_executor.wait_for_completion(std::chrono::seconds{300});
    (void)special_executor.wait_for_completion(std::chrono::seconds{300});
    
    std::cout << std::endl;
    std::cout << "General tasks completed: " << general_tasks.load() << std::endl;
    std::cout << "Special tasks completed: " << special_tasks.load() << std::endl;
    
    // 查询状态
    auto global_status = global_executor.get_async_executor_status();
    auto special_status = special_executor.get_async_executor_status();
    
    std::cout << std::endl;
    std::cout << "Global executor status:" << std::endl;
    std::cout << "  Completed tasks: " << global_status.completed_tasks << std::endl;
    std::cout << "  Queue size: " << global_status.queue_size << std::endl;
    
    std::cout << "Special executor status:" << std::endl;
    std::cout << "  Completed tasks: " << special_status.completed_tasks << std::endl;
    std::cout << "  Queue size: " << special_status.queue_size << std::endl;
    std::cout << std::endl;
    
    std::cout << "Key point: Hybrid mode allows:" << std::endl;
    std::cout << "  - Global executor for common tasks (shared across modules)" << std::endl;
    std::cout << "  - Special executor for isolated tasks (independent configuration)" << std::endl;
    std::cout << std::endl;
    
    special_executor.shutdown();
    global_executor.shutdown();
}

// ========== 主函数 ==========

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Multi-Project/Multi-Module Usage Example" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;
    
    // 场景 1：单例共享模式
    scenario1_shared_singleton();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    // 场景 2：实例隔离模式
    scenario2_instance_isolation();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    // 场景 3：混合模式
    scenario3_hybrid_mode();
    
    std::cout << "========================================" << std::endl;
    std::cout << "Example completed successfully!" << std::endl;
    std::cout << "========================================" << std::endl;
    
    return 0;
}
