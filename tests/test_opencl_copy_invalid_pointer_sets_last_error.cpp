#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#define private public
#include "kairo/gpu/opencl_executor.hpp"
#include "kairo/gpu/opencl_loader.hpp"
#undef private

using kairo::gpu::GpuBackend;
using kairo::gpu::GpuExecutorConfig;
using kairo::gpu::OpenCLExecutor;
using kairo::gpu::OpenCLLoader;

namespace {

constexpr cl_int kInjectedWriteBufferFailure = -38;  // CL_INVALID_MEM_OBJECT

cl_int failing_cl_enqueue_write_buffer(cl_command_queue,
                                       cl_mem,
                                       cl_bool,
                                       size_t,
                                       size_t,
                                       const void*,
                                       cl_uint,
                                       const cl_event*,
                                       cl_event*) {
    return kInjectedWriteBufferFailure;
}

GpuExecutorConfig make_opencl_config() {
    GpuExecutorConfig config;
    config.name = "opencl_runtime_last_error";
    config.backend = GpuBackend::OPENCL;
    config.device_id = 0;
    config.default_stream_count = 1;
    config.max_queue_size = 16;
    return config;
}

}  // namespace

TEST(OpenCLRuntimeLastError, CopyToDeviceFailureSetsLastErrorMessage) {
    auto& loader = OpenCLLoader::instance();
    if (!loader.load()) {
        GTEST_SKIP() << "OpenCL loader is not available";
    }

    OpenCLExecutor executor("opencl_runtime_last_error", make_opencl_config());
    if (!executor.start()) {
        GTEST_SKIP() << "OpenCL start failed: " << executor.get_status().last_error_message;
    }

    const size_t size = 16 * sizeof(float);
    std::vector<float> host_data(16, 1.0f);
    void* device_ptr = executor.allocate_device_memory(size);
    ASSERT_NE(device_ptr, nullptr);

    const auto original_write_buffer = loader.functions_.clEnqueueWriteBuffer;
    loader.functions_.clEnqueueWriteBuffer = failing_cl_enqueue_write_buffer;
    EXPECT_FALSE(executor.copy_to_device(device_ptr, host_data.data(), size));
    loader.functions_.clEnqueueWriteBuffer = original_write_buffer;

    const auto status = executor.get_status();
    EXPECT_NE(status.last_error_message.find("clEnqueueWriteBuffer"), std::string::npos)
        << status.last_error_message;
    EXPECT_NE(status.last_error_message.find("CL_INVALID_MEM_OBJECT"), std::string::npos)
        << status.last_error_message;
    EXPECT_NE(status.last_error_message.find("(" + std::to_string(kInjectedWriteBufferFailure) + ")"),
              std::string::npos)
        << status.last_error_message;

    executor.free_device_memory(device_ptr);
}
