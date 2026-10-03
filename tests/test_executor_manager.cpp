#include <cassert>
#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <future>
#include <algorithm>

// 包含 ExecutorManager 的头文件
#include <kairo/executor_manager.hpp>
#include <kairo/executor.hpp>
#include <kairo/config.hpp>
#include <kairo/types.hpp>
#include <kairo/interfaces.hpp>

using namespace kairo;

// 测试辅助宏
#define TEST_ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            std::cerr << "FAILED: " << message << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
            return false; \
        } \
    } while(0)

// Mock RealtimeExecutor 用于测试
class MockRealtimeExecutor : public IRealtimeExecutor {
public:
    MockRealtimeExecutor(const std::string& name) 
        : name_(name), running_(false) {
    }

    std::string get_name() const override {
        return name_;
    }

    RealtimeExecutorStatus get_status() const override {
        RealtimeExecutorStatus status;
        status.name = name_;
        status.is_running = running_.load();
        status.cycle_period_ns = 2000000;  // 2ms
        status.cycle_count = 0;
        status.cycle_timeout_count = 0;
        status.avg_cycle_time_ns = 0.0;
        status.max_cycle_time_ns = 0.0;
        return status;
    }

    bool start() override {
        if (running_.load()) {
            return false;
        }
        running_.store(true);
        return true;
    }

    void stop() override {
        running_.store(false);
    }

    kairo::ExecutorResult push_task(std::function<void()> task) override {
        if (task) {
            task();
        }
        return kairo::ExecutorResult::success();
    }

private:
    std::string name_;
    std::atomic<bool> running_;
};

// ========== 单例模式测试 ==========

bool test_singleton_instance() {
    std::cout << "Testing ExecutorManager singleton instance..." << std::endl;
    
    // 获取单例实例
    ExecutorManager& instance1 = ExecutorManager::instance();
    ExecutorManager& instance2 = ExecutorManager::instance();
    
    // 验证是同一个实例
    TEST_ASSERT(&instance1 == &instance2, "Singleton instances should be the same");
    
    std::cout << "  Singleton instance: PASSED" << std::endl;
    return true;
}

bool test_singleton_thread_safety() {
    std::cout << "Testing ExecutorManager singleton thread safety..." << std::endl;
    
    const int num_threads = 10;
    std::vector<std::thread> threads;
    std::vector<ExecutorManager*> instances(num_threads);
    std::atomic<int> ready_count(0);
    
    // 多个线程同时获取单例
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([i, &instances, &ready_count]() {
            ready_count.fetch_add(1);
            // 等待所有线程就绪
            while (ready_count.load() < num_threads) {
                std::this_thread::yield();
            }
            instances[i] = &ExecutorManager::instance();
        });
    }
    
    // 等待所有线程完成
    for (auto& t : threads) {
        t.join();
    }
    
    // 验证所有线程获取的是同一个实例
    ExecutorManager* first_instance = instances[0];
    for (int i = 1; i < num_threads; ++i) {
        TEST_ASSERT(instances[i] == first_instance, 
                   "All threads should get the same singleton instance");
    }
    
    std::cout << "  Singleton thread safety: PASSED" << std::endl;
    return true;
}

// ========== 实例化模式测试 ==========

bool test_instance_mode() {
    std::cout << "Testing ExecutorManager instance mode..." << std::endl;
    
    // 创建独立的实例
    ExecutorManager manager1;
    ExecutorManager manager2;
    
    // 验证是不同的实例
    TEST_ASSERT(&manager1 != &manager2, "Instance mode should create different instances");
    
    // 验证与单例不同
    ExecutorManager& singleton = ExecutorManager::instance();
    TEST_ASSERT(&manager1 != &singleton, "Instance should be different from singleton");
    TEST_ASSERT(&manager2 != &singleton, "Instance should be different from singleton");
    
    std::cout << "  Instance mode: PASSED" << std::endl;
    return true;
}

// ========== 异步执行器管理测试 ==========

bool test_async_executor_initialization() {
    std::cout << "Testing async executor initialization..." << std::endl;
    
    ExecutorManager manager;
    
    // 创建配置
    ExecutorConfig config;
    config.min_threads = 2;
    config.max_threads = 4;
    config.queue_capacity = 100;
    
    // 初始化异步执行器
    TEST_ASSERT(manager.initialize_async_executor(config), 
               "Async executor should initialize successfully");
    
    // 获取异步执行器
    IAsyncExecutor* executor = manager.get_default_async_executor();
    TEST_ASSERT(executor != nullptr, "Async executor should not be nullptr");
    TEST_ASSERT(executor->get_name() == "default", "Executor name should be 'default'");
    
    // 验证状态
    auto status = executor->get_status();
    TEST_ASSERT(status.is_running == true, "Executor should be running after initialization");
    
    // 测试重复初始化
    TEST_ASSERT(!manager.initialize_async_executor(config), 
               "Re-initialization should fail");
    
    std::cout << "  Async executor initialization: PASSED" << std::endl;
    return true;
}

bool test_async_executor_task_submission() {
    std::cout << "Testing async executor task submission..." << std::endl;
    
    ExecutorManager manager;
    
    ExecutorConfig config;
    config.min_threads = 2;
    config.max_threads = 4;
    config.queue_capacity = 100;
    
    TEST_ASSERT(manager.initialize_async_executor(config), 
               "Async executor should initialize successfully");
    
    IAsyncExecutor* executor = manager.get_default_async_executor();
    TEST_ASSERT(executor != nullptr, "Async executor should not be nullptr");
    
    // 提交任务
    auto future = executor->submit([]() {
        return 42;
    });
    
    int result = future.get();
    TEST_ASSERT(result == 42, "Task result should be 42");
    
    // 提交多个任务
    const int num_tasks = 50;
    std::vector<std::future<int>> futures;
    
    for (int i = 0; i < num_tasks; ++i) {
        auto f = executor->submit([i]() {
            return i * 2;
        });
        futures.push_back(std::move(f));
    }
    
    // 验证所有任务结果
    for (int i = 0; i < num_tasks; ++i) {
        int r = futures[i].get();
        TEST_ASSERT(r == i * 2, "Task result should match");
    }
    
    std::cout << "  Async executor task submission: PASSED" << std::endl;
    return true;
}

bool test_async_executor_lazy_init() {
    std::cout << "Testing async executor lazy initialization..." << std::endl;
    
    ExecutorManager manager;
    
    // 未显式调用 initialize() 时，首次 get_default_async_executor() 触发懒初始化并返回非空
    IAsyncExecutor* executor = manager.get_default_async_executor();
    TEST_ASSERT(executor != nullptr, "Lazy init: get_default_async_executor() should return non-null");
    
    // 提交任务验证执行器可用
    std::atomic<int> done{0};
    executor->submit([&done]() { done = 1; });
    while (done.load() == 0) {
        std::this_thread::yield();
    }
    TEST_ASSERT(done.load() == 1, "Task should run after lazy init");
    
    std::cout << "  Async executor lazy initialization: PASSED" << std::endl;
    return true;
}

bool test_executor_manager_concurrent_initialize() {
    std::cout << "Testing concurrent async executor initialization..." << std::endl;

    ExecutorManager manager;
    ExecutorConfig config;
    config.min_threads = 2;
    config.max_threads = 4;
    config.queue_capacity = 256;

    const int num_threads = 32;
    std::vector<std::thread> threads;
    std::atomic<int> ready_count{0};
    std::atomic<bool> start{false};
    std::atomic<int> success_count{0};

    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&manager, &config, &ready_count, &start, &success_count, num_threads]() {
            ready_count.fetch_add(1);
            while (ready_count.load() < num_threads) {
                std::this_thread::yield();
            }
            while (!start.load()) {
                std::this_thread::yield();
            }

            if (manager.initialize_async_executor(config)) {
                success_count.fetch_add(1);
            }
        });
    }

    start.store(true);

    for (auto& thread : threads) {
        thread.join();
    }

    TEST_ASSERT(success_count.load() == 1,
               "Exactly one concurrent initialize_async_executor call should succeed");

    IAsyncExecutor* executor = manager.get_default_async_executor();
    TEST_ASSERT(executor != nullptr, "Executor should exist after concurrent initialization");

    auto future = executor->submit([]() {
        return 7;
    });
    TEST_ASSERT(future.get() == 7, "Executor should accept tasks after concurrent initialization");

    std::cout << "  Concurrent async executor initialization: PASSED" << std::endl;
    return true;
}

// ========== 实时执行器管理测试 ==========

bool test_realtime_executor_registration() {
    std::cout << "Testing realtime executor registration..." << std::endl;
    
    ExecutorManager manager;
    
    // 注册实时执行器
    auto executor1 = std::make_unique<MockRealtimeExecutor>("realtime_1");
    TEST_ASSERT(manager.register_realtime_executor("realtime_1", std::move(executor1)), 
               "Realtime executor should register successfully");
    
    // 获取实时执行器
    IRealtimeExecutor* retrieved = manager.get_realtime_executor("realtime_1");
    TEST_ASSERT(retrieved != nullptr, "Realtime executor should not be nullptr");
    TEST_ASSERT(retrieved->get_name() == "realtime_1", "Executor name should match");
    
    // 注册多个实时执行器
    auto executor2 = std::make_unique<MockRealtimeExecutor>("realtime_2");
    auto executor3 = std::make_unique<MockRealtimeExecutor>("realtime_3");
    TEST_ASSERT(manager.register_realtime_executor("realtime_2", std::move(executor2)), 
               "Second realtime executor should register successfully");
    TEST_ASSERT(manager.register_realtime_executor("realtime_3", std::move(executor3)), 
               "Third realtime executor should register successfully");
    
    // 测试重复注册
    auto executor4 = std::make_unique<MockRealtimeExecutor>("realtime_1");
    TEST_ASSERT(!manager.register_realtime_executor("realtime_1", std::move(executor4)), 
               "Duplicate registration should fail");
    
    // 测试空名称
    auto executor5 = std::make_unique<MockRealtimeExecutor>("");
    TEST_ASSERT(!manager.register_realtime_executor("", std::move(executor5)), 
               "Empty name registration should fail");
    
    std::cout << "  Realtime executor registration: PASSED" << std::endl;
    return true;
}

bool test_realtime_executor_retrieval() {
    std::cout << "Testing realtime executor retrieval..." << std::endl;
    
    ExecutorManager manager;
    
    // 注册执行器
    auto executor1 = std::make_unique<MockRealtimeExecutor>("realtime_1");
    auto executor2 = std::make_unique<MockRealtimeExecutor>("realtime_2");
    manager.register_realtime_executor("realtime_1", std::move(executor1));
    manager.register_realtime_executor("realtime_2", std::move(executor2));
    
    // 获取执行器
    IRealtimeExecutor* e1 = manager.get_realtime_executor("realtime_1");
    IRealtimeExecutor* e2 = manager.get_realtime_executor("realtime_2");
    TEST_ASSERT(e1 != nullptr, "First executor should not be nullptr");
    TEST_ASSERT(e2 != nullptr, "Second executor should not be nullptr");
    TEST_ASSERT(e1 != e2, "Executors should be different");
    
    // 获取不存在的执行器
    IRealtimeExecutor* e3 = manager.get_realtime_executor("nonexistent");
    TEST_ASSERT(e3 == nullptr, "Nonexistent executor should return nullptr");
    
    std::cout << "  Realtime executor retrieval: PASSED" << std::endl;
    return true;
}

bool test_realtime_executor_names() {
    std::cout << "Testing realtime executor names..." << std::endl;
    
    ExecutorManager manager;
    
    // 初始应该为空
    auto names = manager.get_realtime_executor_names();
    TEST_ASSERT(names.empty(), "Initial names list should be empty");
    
    // 注册多个执行器
    manager.register_realtime_executor("executor_1", 
                                      std::make_unique<MockRealtimeExecutor>("executor_1"));
    manager.register_realtime_executor("executor_2", 
                                      std::make_unique<MockRealtimeExecutor>("executor_2"));
    manager.register_realtime_executor("executor_3", 
                                      std::make_unique<MockRealtimeExecutor>("executor_3"));
    
    // 获取名称列表
    names = manager.get_realtime_executor_names();
    TEST_ASSERT(names.size() == 3, "Names list should have 3 elements");
    
    // 验证名称存在
    bool found1 = std::find(names.begin(), names.end(), "executor_1") != names.end();
    bool found2 = std::find(names.begin(), names.end(), "executor_2") != names.end();
    bool found3 = std::find(names.begin(), names.end(), "executor_3") != names.end();
    TEST_ASSERT(found1 && found2 && found3, "All executor names should be found");
    
    std::cout << "  Realtime executor names: PASSED" << std::endl;
    return true;
}

bool test_create_realtime_executor() {
    std::cout << "Testing create_realtime_executor..." << std::endl;
    
    ExecutorManager manager;
    
    RealtimeThreadConfig config;
    config.thread_name = "test_thread";
    config.cycle_period_ns = 2000000;  // 2ms
    config.cycle_callback = []() {};
    
    // 阶段7之后，此方法应该返回有效的执行器
    auto executor = manager.create_realtime_executor("test", config);
    TEST_ASSERT(executor != nullptr, "create_realtime_executor should return valid executor");
    TEST_ASSERT(executor->get_name() == "test", "Executor name should match");
    
    // 测试无效配置（空名称）
    RealtimeThreadConfig invalid_config;
    invalid_config.thread_name = "test_thread";
    invalid_config.cycle_period_ns = 0;  // 无效周期
    invalid_config.cycle_callback = []() {};
    auto invalid_executor = manager.create_realtime_executor("", invalid_config);
    TEST_ASSERT(invalid_executor == nullptr, "create_realtime_executor should return nullptr for invalid config");
    
    std::cout << "  Create realtime executor: PASSED" << std::endl;
    return true;
}

// ========== 生命周期管理测试 ==========

bool test_lifecycle_raii() {
    std::cout << "Testing lifecycle RAII..." << std::endl;
    
    {
        ExecutorManager manager;
        
        // 初始化异步执行器
        ExecutorConfig config;
        config.min_threads = 2;
        config.max_threads = 4;
        config.queue_capacity = 100;
        manager.initialize_async_executor(config);
        
        // 注册实时执行器
        auto executor = std::make_unique<MockRealtimeExecutor>("realtime_1");
        executor->start();
        manager.register_realtime_executor("realtime_1", std::move(executor));
        
        // 验证执行器存在
        TEST_ASSERT(manager.get_default_async_executor() != nullptr, 
                   "Async executor should exist");
        TEST_ASSERT(manager.get_realtime_executor("realtime_1") != nullptr, 
                   "Realtime executor should exist");
        
        // manager 析构时应该自动关闭所有执行器
    }  // manager 在这里析构
    
    // 执行器应该已经被正确关闭和释放
    std::cout << "  Lifecycle RAII: PASSED" << std::endl;
    return true;
}

bool test_shutdown() {
    std::cout << "Testing shutdown..." << std::endl;
    
    ExecutorManager manager;
    
    // 初始化异步执行器
    ExecutorConfig config;
    config.min_threads = 2;
    config.max_threads = 4;
    config.queue_capacity = 100;
    manager.initialize_async_executor(config);
    
    // 注册实时执行器
    auto executor1 = std::make_unique<MockRealtimeExecutor>("realtime_1");
    executor1->start();
    manager.register_realtime_executor("realtime_1", std::move(executor1));
    
    // 验证执行器存在
    TEST_ASSERT(manager.get_default_async_executor() != nullptr, 
               "Async executor should exist");
    TEST_ASSERT(manager.get_realtime_executor("realtime_1") != nullptr, 
               "Realtime executor should exist");
    
    // 关闭所有执行器
    manager.shutdown(true);
    
    // 验证执行器已被释放
    TEST_ASSERT(manager.get_default_async_executor() == nullptr, 
               "Async executor should be nullptr after shutdown");
    TEST_ASSERT(manager.get_realtime_executor("realtime_1") == nullptr, 
               "Realtime executor should be nullptr after shutdown");
    
    std::cout << "  Shutdown: PASSED" << std::endl;
    return true;
}

bool test_executor_shutdown_false_does_not_block_long_task() {
    std::cout << "Testing Executor shutdown(false) does not block long task..." << std::endl;

    Executor executor;

    ExecutorConfig config;
    config.min_threads = 1;
    config.max_threads = 1;
    config.queue_capacity = 100;

    TEST_ASSERT(executor.initialize(config), "Executor should initialize successfully");

    auto future = executor.submit([]() {
        std::this_thread::sleep_for(std::chrono::seconds(2));
    });
    (void)future;

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto start = std::chrono::steady_clock::now();
    executor.shutdown(false);
    auto elapsed = std::chrono::steady_clock::now() - start;
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    TEST_ASSERT(elapsed_ms < 500, "shutdown(false) should return quickly");

    std::cout << "  Executor shutdown(false) does not block long task: PASSED" << std::endl;
    return true;
}

// ========== 线程安全测试 ==========

bool test_concurrent_realtime_registration() {
    std::cout << "Testing concurrent realtime executor registration..." << std::endl;
    
    ExecutorManager manager;
    
    const int num_threads = 10;
    const int executors_per_thread = 5;
    std::vector<std::thread> threads;
    std::atomic<int> success_count(0);
    std::atomic<int> fail_count(0);
    
    // 多个线程并发注册实时执行器
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([i, &manager, &success_count, &fail_count, executors_per_thread]() {
            for (int j = 0; j < executors_per_thread; ++j) {
                std::string name = "executor_" + std::to_string(i) + "_" + std::to_string(j);
                auto executor = std::make_unique<MockRealtimeExecutor>(name);
                if (manager.register_realtime_executor(name, std::move(executor))) {
                    success_count.fetch_add(1);
                } else {
                    fail_count.fetch_add(1);
                }
            }
        });
    }
    
    // 等待所有线程完成
    for (auto& t : threads) {
        t.join();
    }
    
    // 验证注册成功数量
    int expected_success = num_threads * executors_per_thread;
    TEST_ASSERT(success_count.load() == expected_success, 
               "All registrations should succeed");
    TEST_ASSERT(fail_count.load() == 0, "No registrations should fail");
    
    // 验证所有执行器都可以获取
    auto names = manager.get_realtime_executor_names();
    TEST_ASSERT(static_cast<int>(names.size()) == expected_success, 
               "All executors should be registered");
    
    std::cout << "  Concurrent realtime registration: PASSED" << std::endl;
    return true;
}

bool test_concurrent_realtime_retrieval() {
    std::cout << "Testing concurrent realtime executor retrieval..." << std::endl;
    
    ExecutorManager manager;
    
    // 先注册一些执行器
    const int num_executors = 10;
    for (int i = 0; i < num_executors; ++i) {
        std::string name = "executor_" + std::to_string(i);
        manager.register_realtime_executor(name, 
                                          std::make_unique<MockRealtimeExecutor>(name));
    }
    
    const int num_threads = 20;
    std::vector<std::thread> threads;
    std::atomic<int> success_count(0);
    
    // 多个线程并发获取实时执行器
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([i, &manager, &success_count, num_executors]() {
            for (int j = 0; j < 100; ++j) {
                int index = (i + j) % num_executors;
                std::string name = "executor_" + std::to_string(index);
                IRealtimeExecutor* executor = manager.get_realtime_executor(name);
                if (executor != nullptr && executor->get_name() == name) {
                    success_count.fetch_add(1);
                }
            }
        });
    }
    
    // 等待所有线程完成
    for (auto& t : threads) {
        t.join();
    }
    
    // 验证所有获取都成功
    int expected_success = num_threads * 100;
    TEST_ASSERT(success_count.load() == expected_success, 
               "All retrievals should succeed");
    
    std::cout << "  Concurrent realtime retrieval: PASSED" << std::endl;
    return true;
}

// ========== 主函数 ==========

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "ExecutorManager Integration Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;
    
    bool all_passed = true;
    
    // 单例模式测试
    all_passed &= test_singleton_instance();
    all_passed &= test_singleton_thread_safety();
    
    // 实例化模式测试
    all_passed &= test_instance_mode();
    
    // 异步执行器管理测试
    all_passed &= test_async_executor_initialization();
    all_passed &= test_async_executor_task_submission();
    all_passed &= test_async_executor_lazy_init();
    
    all_passed &= test_executor_manager_concurrent_initialize();
    // 实时执行器管理测试
    all_passed &= test_realtime_executor_registration();
    all_passed &= test_realtime_executor_retrieval();
    all_passed &= test_realtime_executor_names();
    all_passed &= test_create_realtime_executor();
    
    // 生命周期管理测试
    all_passed &= test_lifecycle_raii();
    all_passed &= test_shutdown();
    all_passed &= test_executor_shutdown_false_does_not_block_long_task();
    
    // 线程安全测试
    all_passed &= test_concurrent_realtime_registration();
    all_passed &= test_concurrent_realtime_retrieval();
    
    std::cout << std::endl;
    std::cout << "========================================" << std::endl;
    if (all_passed) {
        std::cout << "All tests PASSED!" << std::endl;
        return 0;
    } else {
        std::cout << "Some tests FAILED!" << std::endl;
        return 1;
    }
}
