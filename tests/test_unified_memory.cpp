#include <gtest/gtest.h>
#include "../src/executor/gpu/cuda_executor.hpp"
#include <vector>
#include <chrono>
#include <string>

class UnifiedMemoryTest : public ::testing::Test {
protected:
    void SetUp() override {
        config_.name = "test_unified_cuda";
        config_.backend = kairo::gpu::GpuBackend::CUDA;
        config_.device_id = 0;
        config_.enable_unified_memory = true;
        config_.enable_monitoring = false;

        executor_ = std::make_unique<kairo::gpu::CudaExecutor>("test_unified", config_);
#ifndef KAIRO_ENABLE_CUDA
        GTEST_SKIP() << "CUDA support not enabled";
#else
        if (!executor_->start()) {
            auto last_error = executor_->get_status().last_error_message;
            if (last_error.empty()) {
                last_error = "CUDA runtime or device is unavailable";
            }
            GTEST_SKIP() << "CUDA unavailable for unified memory tests: "
                         << last_error;
        }
#endif
    }

    void TearDown() override {
        if (executor_) {
            executor_->stop();
        }
    }

    kairo::gpu::GpuExecutorConfig config_;
    std::unique_ptr<kairo::gpu::CudaExecutor> executor_;
};

TEST_F(UnifiedMemoryTest, AllocateAndFree) {
    size_t size = 1024 * sizeof(float);
    void* ptr = executor_->allocate_unified_memory(size);

    if (ptr == nullptr) {
        GTEST_SKIP() << "Unified memory not supported on this system";
    }

    ASSERT_NE(ptr, nullptr);

    // 验证可以从 CPU 访问
    float* data = static_cast<float*>(ptr);
    data[0] = 42.0f;
    EXPECT_EQ(data[0], 42.0f);

    executor_->free_unified_memory(ptr);
}

TEST_F(UnifiedMemoryTest, PrefetchMemory) {
    size_t size = 1024 * sizeof(float);
    void* ptr = executor_->allocate_unified_memory(size);

    if (ptr == nullptr) {
        GTEST_SKIP() << "Unified memory not supported on this system";
    }

    // 初始化数据
    float* data = static_cast<float*>(ptr);
    for (int i = 0; i < 1024; ++i) {
        data[i] = static_cast<float>(i);
    }

    // 预取到设备
    bool success = executor_->prefetch_memory(ptr, size, 0, 0);
    if (!success) {
        executor_->free_unified_memory(ptr);
        GTEST_SKIP() << "Memory prefetch not supported";
    }

    executor_->synchronize();

#ifdef KAIRO_ENABLE_CUDA
    // 预取回主机
    success = executor_->prefetch_memory(ptr, size, cudaCpuDeviceId, 0);
    EXPECT_TRUE(success);

    executor_->synchronize();
#endif

    // 验证数据
    EXPECT_EQ(data[0], 0.0f);
    EXPECT_EQ(data[1023], 1023.0f);

    executor_->free_unified_memory(ptr);
}

TEST_F(UnifiedMemoryTest, MultipleAllocations) {
    const int count = 10;
    std::vector<void*> ptrs;

    for (int i = 0; i < count; ++i) {
        void* ptr = executor_->allocate_unified_memory(1024);
        if (ptr == nullptr) {
            for (auto p : ptrs) {
                executor_->free_unified_memory(p);
            }
            GTEST_SKIP() << "Unified memory not supported";
        }
        ptrs.push_back(ptr);
    }

    // 释放所有
    for (auto ptr : ptrs) {
        executor_->free_unified_memory(ptr);
    }

    SUCCEED();
}

TEST_F(UnifiedMemoryTest, PerformanceComparison) {
    const int n = 1024 * 1024;  // 1M elements
    size_t size = n * sizeof(float);

    // 测试统一内存
    auto start_um = std::chrono::high_resolution_clock::now();

    float* um_data = static_cast<float*>(executor_->allocate_unified_memory(size));
    if (um_data == nullptr) {
        GTEST_SKIP() << "Unified memory not supported";
    }

    for (int i = 0; i < n; ++i) {
        um_data[i] = static_cast<float>(i);
    }

    executor_->prefetch_memory(um_data, size, 0, 0);
    executor_->synchronize();

    auto end_um = std::chrono::high_resolution_clock::now();
    auto duration_um = std::chrono::duration_cast<std::chrono::microseconds>(end_um - start_um).count();

    executor_->free_unified_memory(um_data);

    // 测试显式传输
    auto start_explicit = std::chrono::high_resolution_clock::now();

    std::vector<float> host_data(n);
    for (int i = 0; i < n; ++i) {
        host_data[i] = static_cast<float>(i);
    }

    void* device_data = executor_->allocate_device_memory(size);
    ASSERT_NE(device_data, nullptr);

    executor_->copy_to_device(device_data, host_data.data(), size, false, 0);
    executor_->synchronize();

    auto end_explicit = std::chrono::high_resolution_clock::now();
    auto duration_explicit = std::chrono::duration_cast<std::chrono::microseconds>(end_explicit - start_explicit).count();

    executor_->free_device_memory(device_data);

    // 输出性能对比
    std::cout << "Unified Memory: " << duration_um << " us\n";
    std::cout << "Explicit Transfer: " << duration_explicit << " us\n";

    // 不做严格断言，因为性能取决于硬件
    EXPECT_GT(duration_um, 0);
    EXPECT_GT(duration_explicit, 0);
}
