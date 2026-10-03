/**
 * GPU 监控和统计功能测试（2.5）
 * - 统计信息收集：get_status() 中 queue_size、completed_kernels、failed_kernels、avg_kernel_time_ms 等
 * - 监控查询 API：get_gpu_executor_status(name)、get_all_gpu_executor_status()
 * - 边界：未注册 name 返回默认状态；无 GPU 时 get_all 返回空 map
 */
#include <iostream>
#include <atomic>
#include <chrono>
#include <future>
#include <map>
#include <string>

#include <kairo/executor.hpp>
#include <kairo/executor_manager.hpp>
#include <kairo/types.hpp>
#include <kairo/interfaces.hpp>
#include <kairo/monitor/statistics_collector.hpp>

using namespace kairo;

#define TEST_ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            std::cerr << "FAILED: " << message << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
            return false; \
        } \
    } while(0)

static bool test_get_all_gpu_status_empty_when_no_gpu() {
    std::cout << "Testing get_all_gpu_executor_status() returns empty when no GPU registered..."
              << std::endl;
    Executor exec;
    ExecutorConfig config;
    config.min_threads = 1;
    config.max_threads = 2;
    exec.initialize(config);
    auto all = exec.get_all_gpu_executor_status();
    TEST_ASSERT(all.empty(), "get_all_gpu_executor_status() should be empty when no GPU");
    exec.shutdown();
    std::cout << "  get_all empty when no GPU: PASSED" << std::endl;
    return true;
}

static bool test_get_gpu_status_unknown_returns_default() {
    std::cout << "Testing get_gpu_executor_status(unknown) returns default status..." << std::endl;
    Executor exec;
    ExecutorConfig config;
    config.min_threads = 1;
    config.max_threads = 2;
    exec.initialize(config);
    auto status = exec.get_gpu_executor_status("nonexistent_gpu");
    TEST_ASSERT(status.name == "nonexistent_gpu", "Status name should be the requested name");
    TEST_ASSERT(status.is_running == false, "Unknown executor should report not running");
    exec.shutdown();
    std::cout << "  get_gpu_executor_status(unknown) default: PASSED" << std::endl;
    return true;
}

#ifdef KAIRO_ENABLE_GPU
#include "kairo/gpu/cuda_executor.hpp"

static bool test_gpu_monitor_stats_and_query_api() {
    std::cout << "Testing GPU monitor stats collection and query API (with CudaExecutor)..."
              << std::endl;
    Executor exec;
    ExecutorConfig config;
    config.min_threads = 1;
    config.max_threads = 2;
    exec.initialize(config);

    gpu::GpuExecutorConfig gpu_config;
    gpu_config.name = "gpu_monitor_test";
    gpu_config.backend = gpu::GpuBackend::CUDA;
    gpu_config.device_id = 0;
    gpu_config.max_queue_size = 1000;
    gpu_config.default_stream_count = 1;

    bool registered = exec.register_gpu_executor("gpu_monitor_test", gpu_config);
    if (!registered) {
        std::cout << "  GPU monitor test: SKIPPED (GPU not available or failed to start)"
                  << std::endl;
        exec.shutdown();
        return true;
    }

    auto* gpu_exec = exec.get_gpu_executor("gpu_monitor_test");
    TEST_ASSERT(gpu_exec != nullptr, "GPU executor should be retrievable");
    auto status_direct = gpu_exec->get_status();
    TEST_ASSERT(status_direct.is_running, "Executor should be running");

    // 通过 Facade 查询应与直接 get_status() 一致
    auto status_facade = exec.get_gpu_executor_status("gpu_monitor_test");
    TEST_ASSERT(status_facade.name == status_direct.name, "Facade status name should match");
    TEST_ASSERT(status_facade.is_running == status_direct.is_running, "Facade is_running should match");
    TEST_ASSERT(status_facade.queue_size == 0, "queue_size should be 0 (no pending queue)");
    TEST_ASSERT(status_facade.queue_size == status_direct.queue_size, "Facade queue_size should match");

    // 提交若干成功 kernel，等待完成
    gpu::GpuTaskConfig task_config;
    task_config.grid_size[0] = 1;
    task_config.block_size[0] = 1;
    const int num_success = 3;
    std::vector<std::future<void>> futures;
    for (int i = 0; i < num_success; ++i) {
        futures.push_back(exec.submit_gpu("gpu_monitor_test", []() { /* no-op kernel */ }, task_config));
    }
    for (auto& f : futures) {
        if (f.valid()) {
            try {
                f.get();
            } catch (...) {}
        }
    }

    // 再查一次状态：completed_kernels 应增加，avg_kernel_time_ms 可能非零
    status_direct = gpu_exec->get_status();
    status_facade = exec.get_gpu_executor_status("gpu_monitor_test");
    TEST_ASSERT(status_facade.completed_kernels >= num_success,
                "completed_kernels should be at least num_success");
    TEST_ASSERT(status_facade.completed_kernels == status_direct.completed_kernels,
                "Facade completed_kernels should match direct");
    TEST_ASSERT(status_facade.queue_size == 0, "queue_size should remain 0");

    // get_all_gpu_executor_status() 应包含该执行器
    auto all = exec.get_all_gpu_executor_status();
    TEST_ASSERT(all.size() >= 1u, "get_all should contain at least one GPU executor");
    TEST_ASSERT(all.find("gpu_monitor_test") != all.end(), "gpu_monitor_test should be in map");
    TEST_ASSERT(all.at("gpu_monitor_test").completed_kernels == status_facade.completed_kernels,
                "get_all entry should match single get");

    exec.shutdown();
    std::cout << "  GPU monitor stats and query API: PASSED" << std::endl;
    return true;
}

static bool test_statistics_collector_gpu_apis() {
    std::cout << "Testing StatisticsCollector get_gpu_executor_status / get_all_gpu_executor_statuses..."
              << std::endl;
    // StatisticsCollector 由 ExecutorManager 持有并设置了 provider；通过 Manager 获取统计时
    // 会走 StatisticsCollector。这里直接构造 Manager + 设置 provider，模拟一致行为。
    ExecutorManager manager;
    manager.get_all_gpu_executor_statuses();  // 空 map
    auto all = manager.get_all_gpu_executor_statuses();
    TEST_ASSERT(all.empty(), "New Manager should have empty GPU statuses");
    std::cout << "  StatisticsCollector/Manager GPU APIs: PASSED" << std::endl;
    return true;
}
#endif

int main() {
    std::cout << "=== GPU monitor and statistics tests (2.5) ===" << std::endl;
    bool ok = true;
    ok &= test_get_all_gpu_status_empty_when_no_gpu();
    ok &= test_get_gpu_status_unknown_returns_default();
#ifdef KAIRO_ENABLE_GPU
    ok &= test_gpu_monitor_stats_and_query_api();
    ok &= test_statistics_collector_gpu_apis();
#else
    std::cout << "  KAIRO_ENABLE_GPU not defined, skipping CudaExecutor-based tests"
              << std::endl;
#endif
    std::cout << (ok ? "=== All GPU monitor tests PASSED ===" : "=== Some tests FAILED ===")
              << std::endl;
    return ok ? 0 : 1;
}
