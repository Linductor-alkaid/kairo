#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <thread>
#include <vector>

#include "kairo/gpu/cuda_executor.hpp"

using namespace kairo;
using namespace kairo::gpu;

TEST(CudaStreamDestroyRace, ConcurrentDestroyRecreateReturnsInvalidStream) {
#ifndef KAIRO_ENABLE_CUDA
    GTEST_SKIP() << "CUDA support not enabled";
#else
    GpuExecutorConfig config;
    config.name = "test_cuda_stream_destroy_race";
    config.backend = GpuBackend::CUDA;
    config.device_id = 0;
    config.max_queue_size = 4096;
    config.default_stream_count = 0;

    CudaExecutor executor(config.name, config);
    if (!executor.start()) {
        GTEST_SKIP() << "CUDA not available";
    }

    constexpr int kIterations = 10000;
    const size_t element_count = 64;
    const size_t size = element_count * sizeof(float);
    std::vector<float> host_data(element_count, 1.0f);

    void* device_ptr = executor.allocate_device_memory(size);
    if (device_ptr == nullptr) {
        executor.stop();
        GTEST_SKIP() << "CUDA device allocation failed";
    }

    int initial_stream = executor.create_stream();
    ASSERT_GT(initial_stream, 0);

    std::atomic<int> stream_id{initial_stream};
    std::atomic<bool> start{false};
    std::atomic<bool> create_failed{false};
    std::atomic<bool> unexpected_exception{false};
    std::atomic<int> recreate_successes{0};
    std::atomic<int> slot_reuses{0};
    std::atomic<int> async_copy_destroyed{0};
    std::atomic<int> submit_destroyed{0};
    std::atomic<int> immediate_copy_destroyed{0};
    std::atomic<int> immediate_submit_destroyed{0};

    auto empty_kernel = [](void*) {};

    std::thread submitter([&]() {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        for (int i = 0; i < kIterations; ++i) {
            int sid = stream_id.load(std::memory_order_acquire);

            GpuTaskConfig cfg;
            cfg.stream_id = sid;
            cfg.async = true;

            auto future = executor.submit_kernel(empty_kernel, cfg);
            bool copied = executor.copy_to_device(device_ptr, host_data.data(), size, true, sid);
            if (!copied) {
                async_copy_destroyed.fetch_add(1, std::memory_order_relaxed);
            }

            try {
                future.get();
            } catch (const InvalidStreamException&) {
                submit_destroyed.fetch_add(1, std::memory_order_relaxed);
            } catch (...) {
                unexpected_exception.store(true, std::memory_order_release);
            }
        }
    });

    std::thread destroyer([&]() {
        start.store(true, std::memory_order_release);
        for (int i = 0; i < kIterations; ++i) {
            int current_stream = stream_id.load(std::memory_order_acquire);
            executor.synchronize_stream(current_stream);
            executor.destroy_stream(current_stream);

            if (!executor.copy_to_device(device_ptr, host_data.data(), size, true, current_stream)) {
                immediate_copy_destroyed.fetch_add(1, std::memory_order_relaxed);
            }

            GpuTaskConfig cfg;
            cfg.stream_id = current_stream;
            cfg.async = true;
            auto future = executor.submit_kernel(empty_kernel, cfg);
            try {
                future.get();
            } catch (const InvalidStreamException&) {
                immediate_submit_destroyed.fetch_add(1, std::memory_order_relaxed);
            } catch (...) {
                unexpected_exception.store(true, std::memory_order_release);
            }

            int new_stream = executor.create_stream();
            if (new_stream < 0) {
                create_failed.store(true, std::memory_order_release);
                break;
            }
            recreate_successes.fetch_add(1, std::memory_order_relaxed);
            if (new_stream == current_stream) {
                slot_reuses.fetch_add(1, std::memory_order_relaxed);
            }
            stream_id.store(new_stream, std::memory_order_release);
        }
    });

    submitter.join();
    destroyer.join();

    int final_stream = stream_id.load(std::memory_order_acquire);
    executor.destroy_stream(final_stream);
    executor.free_device_memory(device_ptr);
    executor.stop();

    EXPECT_FALSE(create_failed.load(std::memory_order_acquire));
    EXPECT_FALSE(unexpected_exception.load(std::memory_order_acquire));
    EXPECT_EQ(recreate_successes.load(std::memory_order_acquire), kIterations);
    EXPECT_EQ(slot_reuses.load(std::memory_order_acquire), kIterations);
    EXPECT_EQ(immediate_copy_destroyed.load(std::memory_order_acquire), kIterations);
    EXPECT_EQ(immediate_submit_destroyed.load(std::memory_order_acquire), kIterations);
#endif
}
