#include <gtest/gtest.h>

#include "kairo/gpu/cuda_executor.hpp"

#include <future>
#include <string>

using kairo::gpu::CudaExecutor;
using kairo::gpu::GpuBackend;
using kairo::gpu::GpuExecutorConfig;
using kairo::gpu::GpuTaskConfig;
using kairo::gpu::InvalidStreamException;

namespace {

GpuExecutorConfig make_cuda_config(int device_id) {
    GpuExecutorConfig config;
    config.name = "cuda_status_last_error";
    config.backend = GpuBackend::CUDA;
    config.device_id = device_id;
    config.max_queue_size = 16;
    config.default_stream_count = 0;
    return config;
}

bool contains(const std::string& message, const std::string& needle) {
    return message.find(needle) != std::string::npos;
}

}  // namespace

TEST(CudaStatusRecordsLastError, DirectConstructionRecordsInvalidDeviceId) {
    auto config = make_cuda_config(-1);
    CudaExecutor executor(config.name, config);

    const auto status = executor.get_status();
    EXPECT_FALSE(status.is_running);
    EXPECT_TRUE(contains(status.last_error_message, "CUDA device_id") ||
                  contains(status.last_error_message, "device_id must be"))
        << status.last_error_message;
}

TEST(CudaStatusRecordsLastError, DirectStartRecordsInvalidDeviceId) {
    auto config = make_cuda_config(-1);
    CudaExecutor executor(config.name, config);

    EXPECT_FALSE(executor.start());

    const auto status = executor.get_status();
    EXPECT_FALSE(status.is_running);
    EXPECT_TRUE(contains(status.last_error_message, "CUDA device_id") ||
                  contains(status.last_error_message, "device_id must be"))
        << status.last_error_message;
}

TEST(CudaStatusRecordsLastError, SubmitWithInvalidStreamRecordsLastError) {
#ifndef KAIRO_ENABLE_CUDA
    GTEST_SKIP() << "CUDA support not enabled";
#else
    auto config = make_cuda_config(0);
    CudaExecutor executor(config.name, config);
    if (!executor.start()) {
        GTEST_SKIP() << "CUDA not available: "
                     << executor.get_status().last_error_message;
    }

    kairo::gpu::GpuTaskConfig task_config;
    task_config.stream_id = 1234;
    auto future = executor.submit_kernel([](void*) {}, task_config);

    EXPECT_THROW(future.get(), InvalidStreamException);

    const auto status = executor.get_status();
    EXPECT_TRUE(contains(status.last_error_message, "stream_id 1234"))
        << status.last_error_message;

    executor.stop();
#endif
}
