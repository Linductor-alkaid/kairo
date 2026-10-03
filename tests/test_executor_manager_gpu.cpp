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

// Mock GPU 执行器用于测试
class MockGpuExecutor : public IGpuExecutor {
public:
    MockGpuExecutor(const std::string& name) 
        : name_(name), running_(false), stop_called_(false), wait_called_(false) {
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
        status.completed_kernels = 0;
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
        stop_called_.store(true);
        running_.store(false);
    }

    void wait_for_completion() override {
        wait_called_.store(true);
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
        }
        promise->set_value();
        
        return future;
    }

    bool was_stop_called() const { return stop_called_.load(); }
    bool was_wait_called() const { return wait_called_.load(); }

private:
    std::string name_;
    std::atomic<bool> running_;
    std::atomic<bool> stop_called_;
    std::atomic<bool> wait_called_;
};

// ========== GPU 执行器注册和获取测试 ==========

bool test_gpu_executor_registration() {
    std::cout << "Testing GPU executor registration..." << std::endl;
    
    ExecutorManager manager;
    
    // 创建 Mock GPU 执行器
    auto executor = std::make_unique<MockGpuExecutor>("gpu0");
    
    // 注册执行器
    TEST_ASSERT(manager.register_gpu_executor("gpu0", std::move(executor)), 
                "GPU executor registration should succeed");
    
    // 验证可以获取
    auto* retrieved = manager.get_gpu_executor("gpu0");
    TEST_ASSERT(retrieved != nullptr, "Retrieved GPU executor should not be nullptr");
    TEST_ASSERT(retrieved->get_name() == "gpu0", "Retrieved executor name should match");
    
    std::cout << "  GPU executor registration: PASSED" << std::endl;
    return true;
}

bool test_gpu_executor_retrieval() {
    std::cout << "Testing GPU executor retrieval..." << std::endl;
    
    ExecutorManager manager;
    
    // 注册执行器
    auto executor = std::make_unique<MockGpuExecutor>("gpu1");
    manager.register_gpu_executor("gpu1", std::move(executor));
    
    // 获取存在的执行器
    auto* retrieved = manager.get_gpu_executor("gpu1");
    TEST_ASSERT(retrieved != nullptr, "Retrieved GPU executor should not be nullptr");
    
    // 获取不存在的执行器
    auto* not_found = manager.get_gpu_executor("nonexistent");
    TEST_ASSERT(not_found == nullptr, "Non-existent GPU executor should return nullptr");
    
    std::cout << "  GPU executor retrieval: PASSED" << std::endl;
    return true;
}

bool test_gpu_executor_duplicate_name() {
    std::cout << "Testing GPU executor duplicate name..." << std::endl;
    
    ExecutorManager manager;
    
    // 注册第一个执行器
    auto executor1 = std::make_unique<MockGpuExecutor>("gpu2");
    TEST_ASSERT(manager.register_gpu_executor("gpu2", std::move(executor1)), 
                "First GPU executor registration should succeed");
    
    // 尝试用相同名称注册第二个执行器
    auto executor2 = std::make_unique<MockGpuExecutor>("gpu2");
    TEST_ASSERT(!manager.register_gpu_executor("gpu2", std::move(executor2)), 
                "Duplicate name registration should fail");
    
    std::cout << "  GPU executor duplicate name: PASSED" << std::endl;
    return true;
}

bool test_gpu_executor_invalid_name() {
    std::cout << "Testing GPU executor invalid name..." << std::endl;
    
    ExecutorManager manager;
    
    // 尝试用空名称注册
    auto executor1 = std::make_unique<MockGpuExecutor>("");
    TEST_ASSERT(!manager.register_gpu_executor("", std::move(executor1)), 
                "Empty name registration should fail");
    
    // 尝试用 nullptr 注册
    TEST_ASSERT(!manager.register_gpu_executor("gpu3", nullptr), 
                "Nullptr executor registration should fail");
    
    std::cout << "  GPU executor invalid name: PASSED" << std::endl;
    return true;
}

// ========== 多 GPU 执行器管理测试 ==========

bool test_multiple_gpu_executors() {
    std::cout << "Testing multiple GPU executors..." << std::endl;
    
    ExecutorManager manager;
    
    // 注册多个 GPU 执行器
    const int num_executors = 5;
    for (int i = 0; i < num_executors; ++i) {
        std::string name = "gpu" + std::to_string(i);
        auto executor = std::make_unique<MockGpuExecutor>(name);
        TEST_ASSERT(manager.register_gpu_executor(name, std::move(executor)), 
                    "GPU executor registration should succeed");
    }
    
    // 验证所有执行器都可以获取
    for (int i = 0; i < num_executors; ++i) {
        std::string name = "gpu" + std::to_string(i);
        auto* executor = manager.get_gpu_executor(name);
        TEST_ASSERT(executor != nullptr, "GPU executor should be retrievable");
        TEST_ASSERT(executor->get_name() == name, "GPU executor name should match");
    }
    
    std::cout << "  Multiple GPU executors: PASSED" << std::endl;
    return true;
}

bool test_gpu_executor_names() {
    std::cout << "Testing GPU executor names..." << std::endl;
    
    ExecutorManager manager;
    
    // 注册多个执行器
    std::vector<std::string> expected_names = {"gpu0", "gpu1", "gpu2"};
    for (const auto& name : expected_names) {
        auto executor = std::make_unique<MockGpuExecutor>(name);
        manager.register_gpu_executor(name, std::move(executor));
    }
    
    // 获取所有名称
    auto names = manager.get_gpu_executor_names();
    TEST_ASSERT(names.size() == expected_names.size(), 
                "Number of GPU executor names should match");
    
    // 验证所有名称都存在
    for (const auto& expected_name : expected_names) {
        auto it = std::find(names.begin(), names.end(), expected_name);
        TEST_ASSERT(it != names.end(), "Expected GPU executor name should be found");
    }
    
    std::cout << "  GPU executor names: PASSED" << std::endl;
    return true;
}

// ========== 工厂方法测试 ==========

bool test_create_gpu_executor() {
    std::cout << "Testing create GPU executor..." << std::endl;
    
    ExecutorManager manager;
    
#ifdef KAIRO_ENABLE_GPU
    // 创建有效的 GPU 配置
    gpu::GpuExecutorConfig config;
    config.name = "cuda_test";
    config.backend = gpu::GpuBackend::CUDA;
    config.device_id = 0;
    config.max_queue_size = 1000;
    config.default_stream_count = 1;
    
    // 尝试创建执行器（可能失败，如果 CUDA 不可用）
    auto executor = manager.create_gpu_executor(config);
    // 注意：如果 CUDA 不可用，executor 可能为 nullptr，这是正常的
    // 我们只测试方法调用不会崩溃
#else
    // GPU 支持未启用，应该返回 nullptr
    gpu::GpuExecutorConfig config;
    config.name = "cuda_test";
    config.backend = gpu::GpuBackend::CUDA;
    auto executor = manager.create_gpu_executor(config);
    TEST_ASSERT(executor == nullptr, 
                "Create GPU executor should return nullptr when GPU is disabled");
#endif
    
    std::cout << "  Create GPU executor: PASSED" << std::endl;
    return true;
}

bool test_create_gpu_executor_invalid_config() {
    std::cout << "Testing create GPU executor with invalid config..." << std::endl;
    
    ExecutorManager manager;
    
    // 测试空名称
    gpu::GpuExecutorConfig config1;
    config1.name = "";  // 无效：空名称
    config1.backend = gpu::GpuBackend::CUDA;
    auto executor1 = manager.create_gpu_executor(config1);
    TEST_ASSERT(executor1 == nullptr, 
                "Create GPU executor with empty name should return nullptr");
    
    // 测试无效的队列大小
    gpu::GpuExecutorConfig config2;
    config2.name = "test";
    config2.backend = gpu::GpuBackend::CUDA;
    config2.max_queue_size = 0;  // 无效：队列大小为 0
    auto executor2 = manager.create_gpu_executor(config2);
    TEST_ASSERT(executor2 == nullptr, 
                "Create GPU executor with invalid queue size should return nullptr");
    
    std::cout << "  Create GPU executor invalid config: PASSED" << std::endl;
    return true;
}

// ========== 生命周期管理测试 ==========

bool test_gpu_executor_lifecycle_raii() {
    std::cout << "Testing GPU executor lifecycle RAII..." << std::endl;
    
    {
        ExecutorManager manager;
        
        // 注册 GPU 执行器
        auto executor = std::make_unique<MockGpuExecutor>("gpu_lifecycle");
        auto* executor_ptr = executor.get();
        manager.register_gpu_executor("gpu_lifecycle", std::move(executor));
        
        // 验证执行器存在
        TEST_ASSERT(manager.get_gpu_executor("gpu_lifecycle") != nullptr, 
                    "GPU executor should exist before destruction");
    }
    
    // manager 已析构，执行器应该被自动清理
    // 注意：我们无法直接验证，但可以通过检查没有崩溃来确认
    
    std::cout << "  GPU executor lifecycle RAII: PASSED" << std::endl;
    return true;
}

bool test_gpu_executor_shutdown() {
    std::cout << "Testing GPU executor shutdown..." << std::endl;
    
    ExecutorManager manager;
    
    // 注册 GPU 执行器
    auto executor = std::make_unique<MockGpuExecutor>("gpu_shutdown");
    auto* executor_ptr = executor.get();
    manager.register_gpu_executor("gpu_shutdown", std::move(executor));
    
    // 启动执行器
    executor_ptr->start();
    
    // 在 shutdown 之前保存状态检查标志
    // 注意：shutdown 后 executor_ptr 可能无效，所以我们在 shutdown 前获取指针
    // 但由于 executor 已被移动到 manager，我们需要通过 manager 获取
    auto* retrieved_before = manager.get_gpu_executor("gpu_shutdown");
    TEST_ASSERT(retrieved_before != nullptr, "GPU executor should exist before shutdown");
    
    // 调用 shutdown
    manager.shutdown(true);
    
    // 验证执行器已被清理（无法再获取）
    auto* retrieved_after = manager.get_gpu_executor("gpu_shutdown");
    TEST_ASSERT(retrieved_after == nullptr, 
                "GPU executor should be removed after shutdown");
    
    // 注意：由于 executor 已被移动到 manager 并在 shutdown 时清理，
    // 我们无法直接验证 stop 和 wait_for_completion 被调用
    // 但可以通过验证 shutdown 没有崩溃来间接确认
    
    std::cout << "  GPU executor shutdown: PASSED" << std::endl;
    return true;
}

// ========== 线程安全测试 ==========

bool test_concurrent_gpu_registration() {
    std::cout << "Testing concurrent GPU executor registration..." << std::endl;
    
    ExecutorManager manager;
    
    const int num_threads = 10;
    const int executors_per_thread = 5;
    std::vector<std::thread> threads;
    std::atomic<int> success_count(0);
    std::atomic<int> fail_count(0);
    
    // 多个线程同时注册执行器
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&manager, t, &success_count, &fail_count, executors_per_thread]() {
            for (int i = 0; i < executors_per_thread; ++i) {
                std::string name = "gpu_t" + std::to_string(t) + "_" + std::to_string(i);
                auto executor = std::make_unique<MockGpuExecutor>(name);
                if (manager.register_gpu_executor(name, std::move(executor))) {
                    success_count.fetch_add(1);
                } else {
                    fail_count.fetch_add(1);
                }
            }
        });
    }
    
    // 等待所有线程完成
    for (auto& thread : threads) {
        thread.join();
    }
    
    // 验证所有执行器都已注册（应该没有失败，因为名称唯一）
    TEST_ASSERT(fail_count.load() == 0, 
                "All GPU executor registrations should succeed with unique names");
    TEST_ASSERT(success_count.load() == num_threads * executors_per_thread, 
                "All GPU executors should be registered successfully");
    
    // 验证所有执行器都可以获取
    int retrieved_count = 0;
    for (int t = 0; t < num_threads; ++t) {
        for (int i = 0; i < executors_per_thread; ++i) {
            std::string name = "gpu_t" + std::to_string(t) + "_" + std::to_string(i);
            if (manager.get_gpu_executor(name) != nullptr) {
                retrieved_count++;
            }
        }
    }
    TEST_ASSERT(retrieved_count == num_threads * executors_per_thread, 
                "All registered GPU executors should be retrievable");
    
    std::cout << "  Concurrent GPU executor registration: PASSED" << std::endl;
    return true;
}

bool test_concurrent_gpu_retrieval() {
    std::cout << "Testing concurrent GPU executor retrieval..." << std::endl;
    
    ExecutorManager manager;
    
    // 先注册一些执行器
    const int num_executors = 10;
    for (int i = 0; i < num_executors; ++i) {
        std::string name = "gpu_retrieve_" + std::to_string(i);
        auto executor = std::make_unique<MockGpuExecutor>(name);
        manager.register_gpu_executor(name, std::move(executor));
    }
    
    const int num_threads = 20;
    std::vector<std::thread> threads;
    std::atomic<int> success_count(0);
    
    // 多个线程同时获取执行器
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&manager, &success_count, num_executors]() {
            for (int i = 0; i < num_executors; ++i) {
                std::string name = "gpu_retrieve_" + std::to_string(i);
                auto* executor = manager.get_gpu_executor(name);
                if (executor != nullptr && executor->get_name() == name) {
                    success_count.fetch_add(1);
                }
            }
        });
    }
    
    // 等待所有线程完成
    for (auto& thread : threads) {
        thread.join();
    }
    
    // 验证所有获取都成功
    TEST_ASSERT(success_count.load() == num_threads * num_executors, 
                "All GPU executor retrievals should succeed");
    
    std::cout << "  Concurrent GPU executor retrieval: PASSED" << std::endl;
    return true;
}

// ========== 主函数 ==========

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "ExecutorManager GPU Extension Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;
    
    bool all_passed = true;
    
    // GPU 执行器注册和获取测试
    all_passed &= test_gpu_executor_registration();
    all_passed &= test_gpu_executor_retrieval();
    all_passed &= test_gpu_executor_duplicate_name();
    all_passed &= test_gpu_executor_invalid_name();
    
    // 多 GPU 执行器管理测试
    all_passed &= test_multiple_gpu_executors();
    all_passed &= test_gpu_executor_names();
    
    // 工厂方法测试
    all_passed &= test_create_gpu_executor();
    all_passed &= test_create_gpu_executor_invalid_config();
    
    // 生命周期管理测试
    all_passed &= test_gpu_executor_lifecycle_raii();
    all_passed &= test_gpu_executor_shutdown();
    
    // 线程安全测试
    all_passed &= test_concurrent_gpu_registration();
    all_passed &= test_concurrent_gpu_retrieval();
    
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
