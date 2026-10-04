#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <stdexcept>

#include "kairo/gpu/cuda_executor.hpp"

using kairo::gpu::CudaExecutor;
using kairo::gpu::GpuBackend;
using kairo::gpu::GpuExecutorConfig;

namespace {

GpuExecutorConfig make_cuda_config() {
    GpuExecutorConfig config;
    config.name = "cuda_stream_callback";
    config.backend = GpuBackend::CUDA;
    config.device_id = 0;
    config.max_queue_size = 16;
    config.default_stream_count = 1;
    return config;
}

struct CallbackLifetime {
    explicit CallbackLifetime(std::atomic<int>& destructions)
        : destructions(destructions) {}

    ~CallbackLifetime() {
        destructions.fetch_add(1, std::memory_order_relaxed);
    }

    std::atomic<int>& destructions;
};

struct ThrowingHostCallbackShim {
    std::shared_ptr<CallbackLifetime> lifetime;

    void operator()() const {
        throw std::runtime_error("known stream callback exception");
    }
};

}  // namespace

TEST(CudaStreamCallbackTest, ThrowingCallbackDoesNotTerminate) {
#ifndef KAIRO_ENABLE_CUDA
    GTEST_SKIP() << "CUDA support not enabled";
#else
    auto config = make_cuda_config();
    CudaExecutor executor(config.name, config);
    if (!executor.start()) {
        GTEST_SKIP() << "CUDA not available: "
                     << executor.get_status().last_error_message;
    }

    std::atomic<int> context_destructions{0};
    auto lifetime = std::make_shared<CallbackLifetime>(context_destructions);
    ASSERT_TRUE(executor.add_stream_callback(0, ThrowingHostCallbackShim{lifetime}));
    lifetime.reset();

    executor.synchronize();
    EXPECT_EQ(context_destructions.load(std::memory_order_relaxed), 1);

    executor.stop();
#endif
}

TEST(CudaStreamCallbackTest, NonThrowingCallbackCompletesNormally) {
#ifndef KAIRO_ENABLE_CUDA
    GTEST_SKIP() << "CUDA support not enabled";
#else
    auto config = make_cuda_config();
    CudaExecutor executor(config.name, config);
    if (!executor.start()) {
        GTEST_SKIP() << "CUDA not available: "
                     << executor.get_status().last_error_message;
    }

    std::atomic<bool> callback_completed{false};
    ASSERT_TRUE(executor.add_stream_callback(0, [&] {
        callback_completed.store(true, std::memory_order_release);
    }));

    executor.synchronize();
    EXPECT_TRUE(callback_completed.load(std::memory_order_acquire));

    executor.stop();
#endif
}
