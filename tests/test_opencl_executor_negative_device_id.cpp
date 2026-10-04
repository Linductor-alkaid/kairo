#include <gtest/gtest.h>

#include <kairo/executor_manager.hpp>
#include "kairo/gpu/opencl_executor.hpp"
#include "kairo/gpu/opencl_loader.hpp"

#include <memory>
#include <string>

using kairo::ExecutorManager;
using kairo::gpu::GpuBackend;
using kairo::gpu::GpuExecutorConfig;
using kairo::gpu::OpenCLExecutor;
using kairo::gpu::OpenCLLoader;

namespace {

GpuExecutorConfig make_opencl_config(int device_id) {
    GpuExecutorConfig config;
    config.name = "opencl_negative_device_id";
    config.backend = GpuBackend::OPENCL;
    config.device_id = device_id;
    config.max_queue_size = 16;
    config.default_stream_count = 1;
    return config;
}

bool has_invalid_device_message(const std::string& message) {
    return message.find("device_id must be >= 0") != std::string::npos;
}

}  // namespace

TEST(OpenCLExecutorNegativeDeviceId, FactoryRejectsNegativeDeviceId) {
    ExecutorManager manager;
    auto config = make_opencl_config(-1);

    auto executor = manager.create_gpu_executor(config);

    EXPECT_EQ(executor, nullptr);
}

TEST(OpenCLExecutorNegativeDeviceId, DirectConstructionRecordsInvalidConfig) {
    auto config = make_opencl_config(-1);
    OpenCLExecutor executor("opencl_negative_device_id", config);

    const auto status = executor.get_status();
    EXPECT_FALSE(status.is_running);
    EXPECT_TRUE(has_invalid_device_message(status.last_error_message))
        << status.last_error_message;
}

TEST(OpenCLExecutorNegativeDeviceId, DirectStartRejectsNegativeDeviceId) {
    auto config = make_opencl_config(-1);
    OpenCLExecutor executor("opencl_negative_device_id", config);

    EXPECT_FALSE(executor.start());

    const auto status = executor.get_status();
    EXPECT_FALSE(status.is_running);
    EXPECT_TRUE(has_invalid_device_message(status.last_error_message))
        << status.last_error_message;
}

TEST(OpenCLExecutorNegativeDeviceId, RegisteredExecutorStartRejectsNegativeDeviceId) {
    ExecutorManager manager;
    auto config = make_opencl_config(-1);
    auto executor = std::make_unique<OpenCLExecutor>("registered_opencl_negative", config);

    ASSERT_TRUE(manager.register_gpu_executor("registered_opencl_negative", std::move(executor)));
    auto* registered = manager.get_gpu_executor("registered_opencl_negative");
    ASSERT_NE(registered, nullptr);

    EXPECT_FALSE(registered->start());

    const auto status = registered->get_status();
    EXPECT_FALSE(status.is_running);
    EXPECT_TRUE(has_invalid_device_message(status.last_error_message))
        << status.last_error_message;
}

TEST(OpenCLExecutorNegativeDeviceId, DeviceZeroStillStartsWhenOpenCLDeviceIsAvailable) {
    auto config = make_opencl_config(0);
    config.name = "opencl_device_zero";
    OpenCLExecutor executor("opencl_device_zero", config);

    ASSERT_TRUE(executor.get_status().last_error_message.empty());

    auto& loader = OpenCLLoader::instance();
    if (!loader.load()) {
        GTEST_SKIP() << "OpenCL loader is not available";
    }

    if (!executor.start()) {
        GTEST_SKIP() << "OpenCL device 0 is not available: "
                     << executor.get_status().last_error_message;
    }

    EXPECT_TRUE(executor.get_status().is_running);
    executor.stop();
}
