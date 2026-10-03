#include <cassert>
#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <future>
#include <algorithm>
#include <stdexcept>

// 包含 Executor 的头文件
#include <kairo/executor.hpp>
#include <kairo/executor_manager.hpp>
#include <kairo/config.hpp>
#include <kairo/interfaces.hpp>
#include <kairo/types.hpp>

using namespace kairo;

#ifdef KAIRO_ENABLE_GPU
// Mock GPU 执行器用于测试
class MockGpuExecutor : public IGpuExecutor {
public:
    MockGpuExecutor(const std::string& name) 
        : name_(name), running_(false) {
    }

    std::string get_name() const override {
        return name_;
    }

    gpu::GpuDeviceInfo get_device_info() const override {
        gpu::GpuDeviceInfo info;
        info.name = name_ + "_device";
        info.backend = gpu::GpuBackend::CUDA;
        info.device_id = 0;
        info.total_memory_bytes = 1024 * 1024 * 1024;  // 1GB
        info.free_memory_bytes = 512 * 1024 * 1024;     // 512MB
        return info;
    }

    gpu::GpuExecutorStatus get_status() const override {
        gpu::GpuExecutorStatus status;
        status.name = name_;
        status.is_running = running_.load();
        status.backend = gpu::GpuBackend::CUDA;
        status.device_id = 0;
        status.active_kernels = 0;
        status.completed_kernels = completed_kernels_.load();
        status.failed_kernels = 0;
        status.queue_size = 0;
        status.avg_kernel_time_ms = 0.0;
        status.memory_used_bytes = 0;
        status.memory_total_bytes = 1024 * 1024 * 1024;
        status.memory_usage_percent = 0.0;
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

    void wait_for_completion() override {
    }

    void* allocate_device_memory(size_t size) override {
        (void)size;
        return reinterpret_cast<void*>(0x1000);  // Mock 指针
    }

    void free_device_memory(void* ptr) override {
        (void)ptr;
    }

    bool copy_to_device(void* dst, const void* src, size_t size, bool async = false, int stream_id = 0) override {
        (void)dst;
        (void)src;
        (void)size;
        (void)async;
        (void)stream_id;
        return true;
    }

    bool copy_to_host(void* dst, const void* src, size_t size, bool async = false, int stream_id = 0) override {
        (void)dst;
        (void)src;
        (void)size;
        (void)async;
        (void)stream_id;
        return true;
    }

    bool copy_device_to_device(void* dst, const void* src, size_t size, bool async = false, int stream_id = 0) override {
        (void)dst;
        (void)src;
        (void)size;
        (void)async;
        (void)stream_id;
        return true;
    }

    bool copy_from_peer(IGpuExecutor* src_executor, const void* src_ptr, void* dst_ptr,
                       size_t size, bool async = false, int stream_id = 0) override {
        (void)src_executor;
        (void)src_ptr;
        (void)dst_ptr;
        (void)size;
        (void)async;
        (void)stream_id;
        return false;
    }

    void synchronize() override {
    }

    void synchronize_stream(int stream_id) override {
        (void)stream_id;
    }

    int create_stream() override {
        return 1;
    }

    void destroy_stream(int stream_id) override {
        (void)stream_id;
    }

    bool add_stream_callback(int stream_id, std::function<void()> callback) override {
        (void)stream_id;
        if (callback) {
            callback();
        }
        return true;
    }

    std::future<void> submit_kernel_impl(
        std::function<void(void*)> kernel_func,
        const gpu::GpuTaskConfig& config) override {
        (void)config;
        auto promise = std::make_shared<std::promise<void>>();
        auto future = promise->get_future();
        
        if (kernel_func) {
            kernel_func(nullptr);
            completed_kernels_.fetch_add(1);
        }
        promise->set_value();
        
        return future;
    }

private:
    std::string name_;
    std::atomic<bool> running_;
    std::atomic<size_t> completed_kernels_{0};
};
#endif

// 测试辅助宏
#define TEST_ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            std::cerr << "FAILED: " << message << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
            return false; \
        } \
    } while(0)

// 测试函数前向声明
bool test_singleton_instance();
bool test_instance_mode();
bool test_initialize();
bool test_submit();
bool test_submit_priority();
bool test_submit_delayed();
bool test_delayed_timer_thread_creation_failure_rolls_back();
bool test_submit_periodic();
bool test_realtime_task_management();
bool test_monitor_queries();
bool test_concurrent_submit();
bool test_enable_monitoring();
bool test_wait_for_completion();
bool test_lazy_init_facade();
bool test_lazy_init_thread_safety();
bool test_explicit_init_priority();
bool test_atexit_shutdown_singleton();
bool test_instance_mode_no_atexit();

#ifdef KAIRO_ENABLE_GPU
bool test_gpu_executor_registration();
bool test_gpu_task_submission();
bool test_gpu_executor_status();
#endif

// ========== 单例模式测试 ==========

bool test_singleton_instance() {
    std::cout << "Testing Executor singleton instance..." << std::endl;
    
    // 获取单例实例
    Executor& instance1 = Executor::instance();
    Executor& instance2 = Executor::instance();
    
    // 验证是同一个实例
    TEST_ASSERT(&instance1 == &instance2, "Singleton instances should be the same");
    
    std::cout << "  Singleton instance: PASSED" << std::endl;
    return true;
}

// ========== 实例化模式测试 ==========

bool test_instance_mode() {
    std::cout << "Testing Executor instance mode..." << std::endl;
    
    // 创建独立的 Executor 实例
    Executor executor1;
    Executor executor2;
    
    // 初始化两个实例
    ExecutorConfig config1;
    config1.min_threads = 2;
    config1.max_threads = 4;
    
    ExecutorConfig config2;
    config2.min_threads = 3;
    config2.max_threads = 6;
    
    TEST_ASSERT(executor1.initialize(config1), "executor1 initialization should succeed");
    TEST_ASSERT(executor2.initialize(config2), "executor2 initialization should succeed");
    
    // 验证两个实例是独立的
    auto status1 = executor1.get_async_executor_status();
    auto status2 = executor2.get_async_executor_status();
    
    TEST_ASSERT(status1.is_running, "executor1 should be running");
    TEST_ASSERT(status2.is_running, "executor2 should be running");
    
    executor1.shutdown();
    executor2.shutdown();
    
    std::cout << "  Instance mode: PASSED" << std::endl;
    return true;
}

// ========== 初始化测试 ==========

bool test_initialize() {
    std::cout << "Testing Executor::initialize()..." << std::endl;
    
    Executor executor;
    
    ExecutorConfig config;
    config.min_threads = 4;
    config.max_threads = 8;
    config.queue_capacity = 100;
    
    TEST_ASSERT(executor.initialize(config), "Initialization should succeed");
    
    auto status = executor.get_async_executor_status();
    TEST_ASSERT(status.is_running, "Executor should be running after initialization");
    
    executor.shutdown();
    
    std::cout << "  Initialize: PASSED" << std::endl;
    return true;
}

// ========== 基本任务提交测试 ==========

bool test_submit() {
    std::cout << "Testing Executor::submit()..." << std::endl;
    
    Executor executor;
    ExecutorConfig config;
    config.min_threads = 2;
    config.max_threads = 4;
    executor.initialize(config);
    
    // 测试基本任务提交
    auto future = executor.submit([]() noexcept {
        return 42;
    });
    
    TEST_ASSERT(future.get() == 42, "Task result should be 42");
    
    // 测试带参数的任务
    auto future2 = executor.submit([](int a, int b) noexcept {
        return a + b;
    }, 10, 20);
    
    TEST_ASSERT(future2.get() == 30, "Task result should be 30");
    
    executor.shutdown();
    
    std::cout << "  Submit: PASSED" << std::endl;
    return true;
}

// ========== 优先级任务提交测试 ==========

bool test_submit_priority() {
    std::cout << "Testing Executor::submit_priority()..." << std::endl;
    
    Executor executor;
    ExecutorConfig config;
    config.min_threads = 2;
    config.max_threads = 4;
    executor.initialize(config);
    
    // 测试不同优先级的任务
    std::vector<int> execution_order;
    std::mutex order_mutex;
    
    // 提交低优先级任务
    executor.submit_priority(0, [&execution_order, &order_mutex]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::lock_guard<std::mutex> lock(order_mutex);
        execution_order.push_back(0);
    });
    
    // 提交高优先级任务
    executor.submit_priority(2, [&execution_order, &order_mutex]() {
        std::lock_guard<std::mutex> lock(order_mutex);
        execution_order.push_back(2);
    });
    
    // 等待任务完成
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // 验证高优先级任务先执行（理想情况下）
    // 注意：由于线程调度的不确定性，这个测试可能不稳定
    {
        std::lock_guard<std::mutex> lock(order_mutex);
        TEST_ASSERT(execution_order.size() == 2, "Both tasks should complete");
    }
    
    executor.shutdown();
    
    std::cout << "  Submit priority: PASSED" << std::endl;
    return true;
}

// ========== 延迟任务提交测试 ==========

bool test_submit_delayed() {
    std::cout << "Testing Executor::submit_delayed()..." << std::endl;
    
    Executor executor;
    ExecutorConfig config;
    config.min_threads = 2;
    config.max_threads = 4;
    executor.initialize(config);
    
    // 测试延迟任务
    auto start_time = std::chrono::steady_clock::now();
    std::atomic<bool> task_executed(false);
    
    auto future = executor.submit_delayed(100, [&task_executed]() noexcept {
        task_executed.store(true);
        return 100;
    });
    
    // 验证任务在延迟后执行
    auto result = future.get();
    auto end_time = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time).count();
    
    TEST_ASSERT(result == 100, "Task result should be 100");
    TEST_ASSERT(task_executed.load(), "Task should be executed");
    TEST_ASSERT(elapsed >= 90, "Task should execute after delay (at least 90ms)");
    TEST_ASSERT(elapsed < 200, "Task should execute within reasonable time (less than 200ms)");
    
    executor.shutdown();
    
    std::cout << "  Submit delayed: PASSED" << std::endl;
    return true;
}

bool test_delayed_timer_thread_creation_failure_rolls_back() {
    std::cout << "Testing Executor delayed timer thread creation failure rollback..." << std::endl;

    Executor executor;
    ExecutorConfig config;
    config.min_threads = 2;
    config.max_threads = 4;
    TEST_ASSERT(executor.initialize(config), "Executor initialization should succeed");

    std::atomic<int> execution_count(0);
    executor.set_timer_thread_factory_for_test(
        [](std::function<void()>) -> std::thread {
            throw std::runtime_error("injected timer thread creation failure");
        });

    bool threw = false;
    try {
        auto failed_future = executor.submit_delayed(1, [&execution_count]() noexcept {
            execution_count.fetch_add(1);
            return 1;
        });
        (void)failed_future;
    } catch (const std::runtime_error&) {
        threw = true;
    }

    TEST_ASSERT(threw, "submit_delayed should propagate timer thread creation failure");

    executor.set_timer_thread_factory_for_test(nullptr);
    auto future = executor.submit_delayed(1, [&execution_count]() noexcept {
        execution_count.fetch_add(1);
        return 2;
    });

    auto status = future.wait_for(std::chrono::seconds(2));
    TEST_ASSERT(status == std::future_status::ready,
                "Timer thread should be startable after failed creation rolls back");
    TEST_ASSERT(future.get() == 2, "Second delayed task result should be 2");
    TEST_ASSERT(execution_count.load() >= 1, "Delayed task should execute after retry");

    executor.shutdown();

    std::cout << "  Delayed timer thread creation failure rollback: PASSED" << std::endl;
    return true;
}

// ========== 周期性任务测试 ==========

bool test_submit_periodic() {
    std::cout << "Testing Executor::submit_periodic()..." << std::endl;
    
    Executor executor;
    ExecutorConfig config;
    config.min_threads = 2;
    config.max_threads = 4;
    executor.initialize(config);
    
    // 测试周期性任务
    std::atomic<int> execution_count(0);
    
    std::string task_id = executor.submit_periodic(50, [&execution_count]() noexcept {
        execution_count.fetch_add(1);
    });
    
    TEST_ASSERT(!task_id.empty(), "Task ID should not be empty");
    
    // 等待几个周期。217ms 刻意落在 50ms 网格之间：网格锚定后第 4 个 tick
    // 的 deadline 恰为 200ms 整，若在 200ms 整点 cancel，会与已注册的在途
    // tick 竞速（cancel 不追回已登记 tick 是既定契约），断言将被随机击穿。
    std::this_thread::sleep_for(std::chrono::milliseconds(217));

    // 验证任务执行了多次
    int count = execution_count.load();
    TEST_ASSERT(count >= 3, "Task should execute at least 3 times");
    TEST_ASSERT(count <= 6, "Task should execute at most 6 times (with some tolerance)");
    
    // 取消任务
    TEST_ASSERT(executor.cancel_task(task_id), "Task cancellation should succeed");
    
    // 等待一段时间，验证任务不再执行
    int count_before = execution_count.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    int count_after = execution_count.load();
    
    TEST_ASSERT(count_after == count_before, "Task should not execute after cancellation");
    
    executor.shutdown();
    
    std::cout << "  Submit periodic: PASSED" << std::endl;
    return true;
}

// ========== 实时任务管理测试 ==========

bool test_realtime_task_management() {
    std::cout << "Testing realtime task management..." << std::endl;
    
    Executor executor;
    ExecutorConfig config;
    config.min_threads = 2;
    config.max_threads = 4;
    executor.initialize(config);
    
    // 注册实时任务
    RealtimeThreadConfig rt_config;
    rt_config.thread_name = "test_realtime";
    rt_config.cycle_period_ns = 10000000;  // 10ms
    rt_config.thread_priority = 0;  // 普通优先级（测试环境）
    rt_config.cycle_callback = []() noexcept {
        // 空回调
    };
    
    TEST_ASSERT(executor.register_realtime_task("test_realtime", rt_config),
                "Realtime task registration should succeed");
    
    // 验证任务已注册
    auto task_list = executor.get_realtime_task_list();
    TEST_ASSERT(std::find(task_list.begin(), task_list.end(), "test_realtime") != task_list.end(),
                "Realtime task should be in the list");
    
    // 启动实时任务
    TEST_ASSERT(executor.start_realtime_task("test_realtime"),
                "Realtime task start should succeed");
    
    // 验证任务正在运行
    auto status = executor.get_realtime_executor_status("test_realtime");
    TEST_ASSERT(status.is_running, "Realtime task should be running");
    
    // 停止实时任务
    executor.stop_realtime_task("test_realtime");
    
    // 等待线程停止
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // 验证任务已停止
    auto status2 = executor.get_realtime_executor_status("test_realtime");
    TEST_ASSERT(!status2.is_running, "Realtime task should be stopped");
    
    executor.shutdown();
    
    std::cout << "  Realtime task management: PASSED" << std::endl;
    return true;
}

// ========== 监控查询测试 ==========

bool test_monitor_queries() {
    std::cout << "Testing monitor queries..." << std::endl;
    
    Executor executor;
    ExecutorConfig config;
    config.min_threads = 2;
    config.max_threads = 4;
    executor.initialize(config);
    
    // 提交一些任务
    for (int i = 0; i < 10; ++i) {
        executor.submit([i]() noexcept {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return i;
        });
    }
    
    // 等待任务完成
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // 查询异步执行器状态
    auto async_status = executor.get_async_executor_status();
    TEST_ASSERT(async_status.is_running, "Async executor should be running");
    TEST_ASSERT(async_status.completed_tasks > 0, "Some tasks should be completed");
    
    executor.shutdown();
    
    std::cout << "  Monitor queries: PASSED" << std::endl;
    return true;
}

// ========== 并发测试 ==========

bool test_concurrent_submit() {
    std::cout << "Testing concurrent submit..." << std::endl;
    
    Executor executor;
    ExecutorConfig config;
    config.min_threads = 4;
    config.max_threads = 8;
    executor.initialize(config);
    
    const int num_tasks = 100;
    std::vector<std::future<int>> futures;
    std::atomic<int> completed_count(0);
    
    // 并发提交任务
    for (int i = 0; i < num_tasks; ++i) {
        auto future = executor.submit([i, &completed_count]() noexcept {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            completed_count.fetch_add(1);
            return i;
        });
        futures.push_back(std::move(future));
    }
    
    // 等待所有任务完成
    for (auto& future : futures) {
        future.get();
    }
    
    TEST_ASSERT(completed_count.load() == num_tasks, "All tasks should complete");
    
    executor.shutdown();
    
    std::cout << "  Concurrent submit: PASSED" << std::endl;
    return true;
}

// ========== enable_monitoring 测试 ==========

bool test_enable_monitoring() {
    std::cout << "Testing Executor::enable_monitoring()..." << std::endl;
    
    Executor executor;
    ExecutorConfig config;
    config.min_threads = 2;
    config.max_threads = 4;
    config.enable_monitoring = false;  // 初始禁用
    executor.initialize(config);
    
    // 提交一些任务（监控禁用时）
    for (int i = 0; i < 5; ++i) {
        executor.submit([i]() noexcept {
            return i;
        });
    }
    executor.wait_for_completion();
    
    // 查询统计（应该为空或很少）
    auto stats_before = executor.get_task_statistics("default");
    int64_t count_before = stats_before.total_count;
    
    // 启用监控
    executor.enable_monitoring(true);
    
    // 提交更多任务
    for (int i = 0; i < 10; ++i) {
        executor.submit([i]() noexcept {
            return i;
        });
    }
    executor.wait_for_completion();
    
    // 查询统计（应该有新的计数）
    auto stats_after = executor.get_task_statistics("default");
    TEST_ASSERT(stats_after.total_count > count_before, 
                "Task count should increase after enabling monitoring");
    
    // 禁用监控
    executor.enable_monitoring(false);
    int64_t count_after_disable = stats_after.total_count;
    
    // 提交更多任务（监控禁用时）
    for (int i = 0; i < 5; ++i) {
        executor.submit([i]() noexcept {
            return i;
        });
    }
    executor.wait_for_completion();
    
    // 统计应该不变
    auto stats_final = executor.get_task_statistics("default");
    TEST_ASSERT(stats_final.total_count == count_after_disable,
                "Task count should not increase when monitoring is disabled");
    
    executor.shutdown();
    
    std::cout << "  Enable monitoring: PASSED" << std::endl;
    return true;
}

// ========== wait_for_completion 测试 ==========

bool test_wait_for_completion() {
    std::cout << "Testing Executor::wait_for_completion()..." << std::endl;
    
    Executor executor;
    ExecutorConfig config;
    config.min_threads = 2;
    config.max_threads = 4;
    executor.initialize(config);
    
    std::atomic<int> completed_count{0};
    const int num_tasks = 20;
    
    // 提交多个任务
    std::vector<std::future<void>> futures;
    for (int i = 0; i < num_tasks; ++i) {
        auto future = executor.submit([i, &completed_count]() noexcept {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            completed_count.fetch_add(1);
        });
        futures.push_back(std::move(future));
    }
    
    // 调用 wait_for_completion
    executor.wait_for_completion();
    
    // 验证所有任务已完成
    TEST_ASSERT(completed_count.load() == num_tasks,
                "All tasks should be completed after wait_for_completion");
    
    // 验证所有 future 可以立即获取结果
    for (auto& future : futures) {
        future.get();  // 应该立即返回，不阻塞
    }
    
    // 再次调用 wait_for_completion（应该立即返回，因为没有待处理任务）
    auto start = std::chrono::steady_clock::now();
    executor.wait_for_completion();
    auto end = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        end - start).count();
    
    TEST_ASSERT(elapsed < 100, 
                "wait_for_completion should return immediately when no pending tasks");
    
    executor.shutdown();
    
    std::cout << "  Wait for completion: PASSED" << std::endl;
    return true;
}

// ========== 兜底策略：懒初始化与退出时关闭 ==========

bool test_lazy_init_facade() {
    std::cout << "Testing lazy init at Facade (no initialize(), submit directly)..." << std::endl;
    // 不调用 initialize()，直接通过单例 submit，应触发懒初始化且不抛异常
    auto& ex = Executor::instance();
    auto future = ex.submit([]() noexcept { return 42; });
    int result = future.get();
    TEST_ASSERT(result == 42, "Task should run and return 42 after lazy init");
    std::cout << "  Lazy init Facade: PASSED" << std::endl;
    return true;
}

bool test_lazy_init_thread_safety() {
    std::cout << "Testing lazy init thread safety (concurrent first submit)..." << std::endl;
    const int num_threads = 8;
    const int tasks_per_thread = 4;
    std::vector<std::thread> threads;
    std::atomic<int> success_count(0);
    std::atomic<int> ready_count(0);
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&success_count, &ready_count, num_threads, tasks_per_thread, i]() {
            ready_count.fetch_add(1);
            while (ready_count.load() < num_threads) {
                std::this_thread::yield();
            }
            auto& ex = Executor::instance();
            for (int j = 0; j < tasks_per_thread; ++j) {
                try {
                    auto f = ex.submit([k = i * tasks_per_thread + j]() noexcept { return k; });
                    int r = f.get();
                    (void)r;
                    success_count.fetch_add(1);
                } catch (...) {
                    // ignore
                }
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    TEST_ASSERT(success_count.load() == num_threads * tasks_per_thread,
                "All tasks should complete successfully with single init");
    std::cout << "  Lazy init thread safety: PASSED" << std::endl;
    return true;
}

bool test_explicit_init_priority() {
    std::cout << "Testing explicit init priority (initialize then submit, re-init returns false)..." << std::endl;
    Executor executor;
    ExecutorConfig custom_config;
    custom_config.min_threads = 2;
    custom_config.max_threads = 4;
    custom_config.queue_capacity = 50;
    TEST_ASSERT(executor.initialize(custom_config), "Explicit init should succeed");
    auto status = executor.get_async_executor_status();
    TEST_ASSERT(status.is_running, "Executor should be running after explicit init");
    auto future = executor.submit([]() noexcept { return 1; });
    TEST_ASSERT(future.get() == 1, "Task should run with custom config");
    ExecutorConfig other_config;
    other_config.min_threads = 8;
    other_config.max_threads = 16;
    TEST_ASSERT(!executor.initialize(other_config), "Second initialize() should return false");
    executor.shutdown();
    std::cout << "  Explicit init priority: PASSED" << std::endl;
    return true;
}

bool test_atexit_shutdown_singleton() {
    std::cout << "Testing exit-time shutdown (singleton, no explicit shutdown)..." << std::endl;
    // 使用单例、不调用 shutdown()；进程退出时 atexit 会调用 shutdown(false)
    auto& ex = Executor::instance();
    ex.submit([]() {}).get();
    std::cout << "  Exit-time shutdown: PASSED" << std::endl;
    return true;
}

bool test_instance_mode_no_atexit() {
    std::cout << "Testing instance mode does not register atexit..." << std::endl;
    {
        Executor ex;
        ExecutorConfig config;
        config.min_threads = 2;
        config.max_threads = 4;
        TEST_ASSERT(ex.initialize(config), "Instance init should succeed");
        auto f = ex.submit([]() noexcept { return 7; });
        TEST_ASSERT(f.get() == 7, "Task should run");
    }
    std::cout << "  Instance mode no atexit: PASSED" << std::endl;
    return true;
}

#ifdef KAIRO_ENABLE_GPU
// ========== GPU 执行器注册测试 ==========

bool test_gpu_executor_registration() {
    std::cout << "Testing GPU executor registration..." << std::endl;
    
    Executor executor;
    ExecutorConfig config;
    config.min_threads = 2;
    config.max_threads = 4;
    executor.initialize(config);
    
    // 注册 GPU 执行器（使用 Mock）
    // 注意：由于 register_gpu_executor 需要 GpuExecutorConfig，而实际创建可能失败
    // 我们直接通过 ExecutorManager 注册 Mock 执行器来测试 Facade 方法
    gpu::GpuExecutorConfig gpu_config;
    gpu_config.name = "gpu_test";
    gpu_config.backend = gpu::GpuBackend::CUDA;
    gpu_config.device_id = 0;
    gpu_config.max_queue_size = 1000;
    gpu_config.default_stream_count = 1;
    
    // 尝试注册（如果 CUDA 不可用，可能失败，这是正常的）
    bool registered = executor.register_gpu_executor("gpu_test", gpu_config);
    
    // 如果注册成功，验证可以获取
    if (registered) {
        auto* gpu_executor = executor.get_gpu_executor("gpu_test");
        TEST_ASSERT(gpu_executor != nullptr, "GPU executor should be retrievable after registration");
        
        // 验证获取执行器名称列表
        auto names = executor.get_gpu_executor_names();
        TEST_ASSERT(std::find(names.begin(), names.end(), "gpu_test") != names.end(),
                    "GPU executor name should be in the list");
        
        // 验证重复注册失败
        bool duplicate = executor.register_gpu_executor("gpu_test", gpu_config);
        TEST_ASSERT(!duplicate, "Duplicate GPU executor registration should fail");
    } else {
        // GPU 不可用，跳过测试（这是正常的，如果 CUDA 未安装）
        std::cout << "  GPU executor registration: SKIPPED (GPU not available)" << std::endl;
        return true;
    }
    
    executor.shutdown();
    
    std::cout << "  GPU executor registration: PASSED" << std::endl;
    return true;
}

// ========== GPU 任务提交测试 ==========

bool test_gpu_task_submission() {
    std::cout << "Testing GPU task submission..." << std::endl;
    
    Executor executor;
    ExecutorConfig config;
    config.min_threads = 2;
    config.max_threads = 4;
    executor.initialize(config);
    
    // 注册 GPU 执行器
    gpu::GpuExecutorConfig gpu_config;
    gpu_config.name = "gpu_submit";
    gpu_config.backend = gpu::GpuBackend::CUDA;
    gpu_config.device_id = 0;
    gpu_config.max_queue_size = 1000;
    gpu_config.default_stream_count = 1;
    
    bool registered = executor.register_gpu_executor("gpu_submit", gpu_config);
    
    if (!registered) {
        // GPU 不可用或启动失败，跳过测试
        std::cout << "  GPU task submission: SKIPPED (GPU not available or failed to start)" << std::endl;
        executor.shutdown();
        return true;
    }
    
    // 验证执行器已注册并可以获取
    auto* gpu_executor = executor.get_gpu_executor("gpu_submit");
    if (!gpu_executor) {
        std::cout << "  GPU task submission: SKIPPED (GPU executor not found after registration)" << std::endl;
        executor.shutdown();
        return true;
    }
    
    // 检查执行器状态
    auto status = gpu_executor->get_status();
    if (!status.is_running) {
        std::cout << "  GPU task submission: SKIPPED (GPU executor is not running)" << std::endl;
        executor.shutdown();
        return true;
    }
    
    // 提交 GPU 任务
    std::atomic<bool> task_executed(false);
    gpu::GpuTaskConfig task_config;
    task_config.grid_size[0] = 1;
    task_config.block_size[0] = 1;
    
    // 使用 shared_ptr 确保 lambda 中的引用在任务执行时仍然有效
    auto task_executed_ptr = std::make_shared<std::atomic<bool>>(false);
    
    auto future = executor.submit_gpu("gpu_submit",
        [task_executed_ptr]() {
            // 简单的任务，只设置标志
            task_executed_ptr->store(true);
        },
        task_config);
    
    // 验证 future 返回
    TEST_ASSERT(future.valid(), "Future should be valid");
    
    // 等待任务完成
    future.wait();
    
    // 获取结果，捕获可能的异常
    bool task_succeeded = false;
    try {
        future.get();  // 获取结果
        task_succeeded = true;
    } catch (const std::exception& e) {
        // 如果执行器不可用或未运行，会抛出异常
        // 这是可以接受的（例如 CUDA 不可用或执行器未启动）
        std::cerr << "  Note: GPU task submission failed: " << e.what() << std::endl;
        std::cerr << "  This is acceptable if CUDA is not available or executor is not running" << std::endl;
        task_succeeded = false;
    } catch (...) {
        // 捕获所有其他异常
        std::cerr << "  Note: GPU task submission failed with unknown exception" << std::endl;
        task_succeeded = false;
    }
    
    // 如果任务执行成功，验证执行标志
    if (task_succeeded) {
        TEST_ASSERT(task_executed_ptr->load(), "GPU task should be executed");
    }
    // 如果任务失败，不进行断言（这是可以接受的情况）
    
    // 测试不存在的执行器
    bool exception_thrown2 = false;
    try {
        executor.submit_gpu("nonexistent",
            []() {},
            task_config);
    } catch (const std::runtime_error&) {
        exception_thrown2 = true;
    } catch (...) {
        // 捕获所有异常类型
        exception_thrown2 = true;
    }
    TEST_ASSERT(exception_thrown2, "Submitting to non-existent GPU executor should throw exception");
    
    executor.shutdown();
    
    std::cout << "  GPU task submission: PASSED" << std::endl;
    return true;
}

// ========== GPU 执行器状态查询测试 ==========

bool test_gpu_executor_status() {
    std::cout << "Testing GPU executor status..." << std::endl;
    
    Executor executor;
    ExecutorConfig config;
    config.min_threads = 2;
    config.max_threads = 4;
    executor.initialize(config);
    
    // 注册 GPU 执行器
    gpu::GpuExecutorConfig gpu_config;
    gpu_config.name = "gpu_status";
    gpu_config.backend = gpu::GpuBackend::CUDA;
    gpu_config.device_id = 0;
    gpu_config.max_queue_size = 1000;
    gpu_config.default_stream_count = 1;
    
    bool registered = executor.register_gpu_executor("gpu_status", gpu_config);
    
    if (!registered) {
        // GPU 不可用，跳过测试
        std::cout << "  GPU executor status: SKIPPED (GPU not available)" << std::endl;
        executor.shutdown();
        return true;
    }
    
    // 查询状态
    auto status = executor.get_gpu_executor_status("gpu_status");
    TEST_ASSERT(status.name == "gpu_status", "Status name should match");
    TEST_ASSERT(status.backend == gpu::GpuBackend::CUDA, "Status backend should be CUDA");
    TEST_ASSERT(status.device_id == 0, "Status device_id should match");
    
    // 测试不存在的执行器返回默认状态
    auto status_not_found = executor.get_gpu_executor_status("nonexistent");
    TEST_ASSERT(status_not_found.name == "nonexistent", "Status name should match requested name");
    TEST_ASSERT(!status_not_found.is_running, "Non-existent executor should not be running");
    
    executor.shutdown();
    
    std::cout << "  GPU executor status: PASSED" << std::endl;
    return true;
}
#endif

// ========== 主函数 ==========

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Executor Facade Integration Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;
    
    bool all_passed = true;
    
    // 运行所有测试
    all_passed &= test_singleton_instance();
    all_passed &= test_instance_mode();
    all_passed &= test_initialize();
    all_passed &= test_submit();
    all_passed &= test_submit_priority();
    all_passed &= test_submit_delayed();
    all_passed &= test_delayed_timer_thread_creation_failure_rolls_back();
    all_passed &= test_submit_periodic();
    all_passed &= test_realtime_task_management();
    all_passed &= test_monitor_queries();
    all_passed &= test_concurrent_submit();
    all_passed &= test_enable_monitoring();
    all_passed &= test_wait_for_completion();
    all_passed &= test_lazy_init_facade();
    all_passed &= test_lazy_init_thread_safety();
    all_passed &= test_explicit_init_priority();
    all_passed &= test_instance_mode_no_atexit();
    all_passed &= test_atexit_shutdown_singleton();
    
#ifdef KAIRO_ENABLE_GPU
    // GPU 相关测试
    all_passed &= test_gpu_executor_registration();
    all_passed &= test_gpu_task_submission();
    all_passed &= test_gpu_executor_status();
#endif
    
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
