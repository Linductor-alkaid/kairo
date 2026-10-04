#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#define private public
#include "kairo/gpu/cuda_executor.hpp"
#undef private

namespace {

using kairo::gpu::CudaExecutor;
using kairo::gpu::GpuBackend;
using kairo::gpu::GpuExecutorConfig;
using namespace std::chrono_literals;

GpuExecutorConfig make_config() {
    GpuExecutorConfig config;
    config.name = "cuda_concurrent_stop";
    config.backend = GpuBackend::CUDA;
    config.device_id = 0;
    config.max_queue_size = 8;
    config.default_stream_count = 0;
    return config;
}

void start_mock_worker(CudaExecutor& executor, std::function<void()> body = {}) {
    executor.is_available_.store(true, std::memory_order_release);
    executor.is_running_.store(true, std::memory_order_release);
    executor.worker_joined_ = false;
    executor.worker_thread_ = std::thread([&executor, body = std::move(body)] {
        {
            std::lock_guard<std::mutex> stop_lock(executor.stop_mutex_);
            executor.worker_id_ = std::this_thread::get_id();
        }
        if (body) {
            body();
            return;
        }
        while (executor.is_running_.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });
}

TEST(CudaExecutorConcurrentStop, ConcurrentStopAndJoinTransfersWorkerOnce) {
    auto config = make_config();
    CudaExecutor executor(config.name, config);
    start_mock_worker(executor);

    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    bool first_result = false;
    bool second_result = false;
    auto stop = [&](bool& result) {
        ready.fetch_add(1, std::memory_order_release);
        while (!go.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        result = executor.stop_and_join();
    };

    std::thread first(stop, std::ref(first_result));
    std::thread second(stop, std::ref(second_result));
    while (ready.load(std::memory_order_acquire) != 2) {
        std::this_thread::yield();
    }
    go.store(true, std::memory_order_release);
    first.join();
    second.join();

    EXPECT_TRUE(first_result);
    EXPECT_TRUE(second_result);
    EXPECT_FALSE(executor.is_running_.load(std::memory_order_acquire));
    EXPECT_TRUE(executor.is_available_.load(std::memory_order_acquire));
    EXPECT_TRUE(executor.worker_joined_);
    EXPECT_FALSE(executor.worker_thread_.joinable());
}

TEST(CudaExecutorConcurrentStop, SelfStopDoesNotTerminate) {
    auto config = make_config();
    CudaExecutor executor(config.name, config);
    std::promise<bool> self_stop_result;
    auto self_stop_future = self_stop_result.get_future();
    start_mock_worker(executor, [&] {
        self_stop_result.set_value(executor.stop_and_join());
    });

    ASSERT_EQ(self_stop_future.wait_for(1s), std::future_status::ready);
    EXPECT_FALSE(self_stop_future.get());
    EXPECT_TRUE(executor.stop_and_join());
    EXPECT_FALSE(executor.is_running_.load(std::memory_order_acquire));
    EXPECT_TRUE(executor.worker_joined_);
}

TEST(CudaExecutorConcurrentStop, DoubleExternalStopIsANoop) {
    auto config = make_config();
    CudaExecutor executor(config.name, config);
    start_mock_worker(executor);

    EXPECT_TRUE(executor.stop_and_join());
    EXPECT_TRUE(executor.stop_and_join());
    EXPECT_TRUE(executor.worker_joined_);
    EXPECT_FALSE(executor.worker_thread_.joinable());
}

}  // namespace
