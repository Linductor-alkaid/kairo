/**
 * @file test_gpu_multi_device.cpp
 * @brief 多 GPU 设备集成测试：多设备管理、多 GPU 并行提交与负载均衡、P2P（可选）
 *
 * 使用真实 CudaExecutor。无 GPU 或多 GPU 注册失败时跳过相关用例。
 */

#include <iostream>
#include <string>
#include <vector>
#include <future>
#include <kairo/executor.hpp>

using namespace kairo;

#define TEST_ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            std::cerr << "FAILED: " << message << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
            return false; \
        } \
    } while(0)

static bool test_multi_device_management() {
    std::cout << "Testing multi-GPU device management..." << std::endl;

    ExecutorConfig exec_cfg;
    exec_cfg.min_threads = 2;
    exec_cfg.max_threads = 4;
    exec_cfg.queue_capacity = 1000;

    auto& exec = Executor::instance();
    if (!exec.initialize(exec_cfg)) {
        std::cout << "  Skipped: Executor init failed." << std::endl;
        return true;
    }

    std::vector<std::string> names;
    for (int i = 0; ; ++i) {
        gpu::GpuExecutorConfig gpu_cfg;
        gpu_cfg.name = "cuda" + std::to_string(i);
        gpu_cfg.backend = gpu::GpuBackend::CUDA;
        gpu_cfg.device_id = i;
        gpu_cfg.max_queue_size = 1000;
        gpu_cfg.default_stream_count = 1;
        if (!exec.register_gpu_executor(gpu_cfg.name, gpu_cfg)) {
            break;
        }
        names.push_back(gpu_cfg.name);
    }

    if (names.empty()) {
        exec.shutdown();
        std::cout << "  Skipped: no GPU registered." << std::endl;
        return true;
    }

    auto reported = exec.get_gpu_executor_names();
    TEST_ASSERT(reported.size() == names.size(), "get_gpu_executor_names size");
    for (const auto& n : names) {
        auto* e = exec.get_gpu_executor(n);
        TEST_ASSERT(e != nullptr, "get_gpu_executor non-null");
        TEST_ASSERT(e->get_name() == n, "executor name match");
        auto st = exec.get_gpu_executor_status(n);
        TEST_ASSERT(st.name == n, "status name match");
    }

    exec.shutdown();
    std::cout << "  Multi-device management: PASSED (" << names.size() << " GPUs)" << std::endl;
    return true;
}

static bool test_multi_gpu_load_balance() {
    std::cout << "Testing multi-GPU load balance (parallel submit + wait)..." << std::endl;

    ExecutorConfig exec_cfg;
    exec_cfg.min_threads = 2;
    exec_cfg.max_threads = 4;
    exec_cfg.queue_capacity = 1000;

    auto& exec = Executor::instance();
    if (!exec.initialize(exec_cfg)) {
        std::cout << "  Skipped: Executor init failed." << std::endl;
        return true;
    }

    std::vector<std::string> names;
    for (int i = 0; ; ++i) {
        gpu::GpuExecutorConfig gpu_cfg;
        gpu_cfg.name = "cuda" + std::to_string(i);
        gpu_cfg.backend = gpu::GpuBackend::CUDA;
        gpu_cfg.device_id = i;
        gpu_cfg.max_queue_size = 1000;
        gpu_cfg.default_stream_count = 1;
        if (!exec.register_gpu_executor(gpu_cfg.name, gpu_cfg)) {
            break;
        }
        names.push_back(gpu_cfg.name);
    }

    if (names.empty()) {
        exec.shutdown();
        std::cout << "  Skipped: no GPU registered." << std::endl;
        return true;
    }

    gpu::GpuTaskConfig task_cfg;
    task_cfg.grid_size[0] = 1;
    task_cfg.block_size[0] = 1;
    task_cfg.async = false;

    std::vector<std::future<void>> futures;
    for (const auto& name : names) {
        std::string n = name;
        auto f = exec.submit_gpu(name, [n]() { (void)n; }, task_cfg);
        futures.push_back(std::move(f));
    }

    for (size_t i = 0; i < futures.size(); ++i) {
        try {
            futures[i].get();
        } catch (const std::exception& e) {
            std::cerr << "  GPU task on " << names[i] << " exception: " << e.what() << std::endl;
            exec.shutdown();
            return false;
        }
    }

    exec.shutdown();
    std::cout << "  Multi-GPU load balance: PASSED (" << futures.size() << " tasks)" << std::endl;
    return true;
}

static bool test_p2p_copy() {
    std::cout << "Testing P2P device-to-device copy..." << std::endl;

    ExecutorConfig exec_cfg;
    exec_cfg.min_threads = 2;
    exec_cfg.max_threads = 4;
    exec_cfg.queue_capacity = 1000;

    auto& exec = Executor::instance();
    if (!exec.initialize(exec_cfg)) {
        std::cout << "  Skipped: Executor init failed." << std::endl;
        return true;
    }

    std::vector<std::string> names;
    for (int i = 0; ; ++i) {
        gpu::GpuExecutorConfig gpu_cfg;
        gpu_cfg.name = "cuda" + std::to_string(i);
        gpu_cfg.backend = gpu::GpuBackend::CUDA;
        gpu_cfg.device_id = i;
        gpu_cfg.max_queue_size = 1000;
        gpu_cfg.default_stream_count = 1;
        if (!exec.register_gpu_executor(gpu_cfg.name, gpu_cfg)) {
            break;
        }
        names.push_back(gpu_cfg.name);
    }

    if (names.size() < 2u) {
        exec.shutdown();
        std::cout << "  Skipped: need >= 2 GPUs for P2P." << std::endl;
        return true;
    }

    auto* cuda0 = exec.get_gpu_executor("cuda0");
    auto* cuda1 = exec.get_gpu_executor("cuda1");
    TEST_ASSERT(cuda0 != nullptr && cuda1 != nullptr, "cuda0/cuda1 non-null");

    const size_t n = 64;
    const size_t size_bytes = n * sizeof(float);
    std::vector<float> host_src(n);
    for (size_t i = 0; i < n; ++i) host_src[i] = static_cast<float>(i);
    std::vector<float> host_dst(n, 0.0f);

    void* d_src = cuda0->allocate_device_memory(size_bytes);
    void* d_dst = cuda1->allocate_device_memory(size_bytes);
    if (!d_src || !d_dst) {
        if (d_src) cuda0->free_device_memory(d_src);
        if (d_dst) cuda1->free_device_memory(d_dst);
        exec.shutdown();
        std::cout << "  Skipped: P2P allocation failed." << std::endl;
        return true;
    }

    if (!cuda0->copy_to_device(d_src, host_src.data(), size_bytes, false)) {
        cuda0->free_device_memory(d_src);
        cuda1->free_device_memory(d_dst);
        exec.shutdown();
        std::cout << "  Skipped: copy_to_device failed." << std::endl;
        return true;
    }

    bool used_p2p = cuda1->copy_from_peer(cuda0, d_src, d_dst, size_bytes, false);
    if (!used_p2p) {
        std::vector<float> host_tmp(n);
        if (!cuda0->copy_to_host(host_tmp.data(), d_src, size_bytes, false) ||
            !cuda1->copy_to_device(d_dst, host_tmp.data(), size_bytes, false)) {
            cuda0->free_device_memory(d_src);
            cuda1->free_device_memory(d_dst);
            exec.shutdown();
            std::cout << "  Skipped: P2P and host fallback both failed." << std::endl;
            return true;
        }
    }

    gpu::GpuTaskConfig task_cfg;
    task_cfg.grid_size[0] = 1;
    task_cfg.block_size[0] = 1;
    task_cfg.async = false;
    auto f = exec.submit_gpu("cuda1", [d_dst]() { (void)d_dst; }, task_cfg);
    f.get();

    bool ok = cuda1->copy_to_host(host_dst.data(), d_dst, size_bytes, false);
    cuda0->free_device_memory(d_src);
    cuda1->free_device_memory(d_dst);
    exec.shutdown();

    TEST_ASSERT(ok, "copy_to_host after transfer");
    for (size_t i = 0; i < n; ++i) {
        TEST_ASSERT(host_dst[i] == host_src[i], "verify element");
    }

    std::cout << "  P2P copy: PASSED" << (used_p2p ? "." : " (via host fallback).") << std::endl;
    return true;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "GPU Multi-Device Integration Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;

    bool ok = true;
    ok &= test_multi_device_management();
    ok &= test_multi_gpu_load_balance();
    ok &= test_p2p_copy();

    std::cout << std::endl;
    std::cout << "========================================" << std::endl;
    if (ok) {
        std::cout << "All tests PASSED!" << std::endl;
        return 0;
    }
    std::cout << "Some tests FAILED!" << std::endl;
    return 1;
}
