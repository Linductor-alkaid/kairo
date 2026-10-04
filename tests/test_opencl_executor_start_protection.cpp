#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

#define private public
#include "kairo/gpu/opencl_executor.hpp"
#include "kairo/gpu/opencl_loader.hpp"
#undef private

using kairo::gpu::GpuBackend;
using kairo::gpu::GpuExecutorConfig;
using kairo::gpu::OpenCLExecutor;
using kairo::gpu::OpenCLFunctionPointers;
using kairo::gpu::OpenCLLoader;

namespace {

cl_platform_id fake_platform() {
    return reinterpret_cast<cl_platform_id>(0x1001);
}

cl_device_id fake_device() {
    return reinterpret_cast<cl_device_id>(0x1002);
}

cl_context fake_context() {
    return reinterpret_cast<cl_context>(0x1003);
}

cl_command_queue fake_queue() {
    return reinterpret_cast<cl_command_queue>(0x1004);
}

cl_int fake_cl_get_platform_ids(cl_uint num_entries,
                                cl_platform_id* platforms,
                                cl_uint* num_platforms) {
    if (num_platforms) {
        *num_platforms = 1;
    }
    if (num_entries > 0 && platforms) {
        platforms[0] = fake_platform();
    }
    return CL_SUCCESS;
}

cl_int fake_cl_get_device_ids(cl_platform_id,
                              cl_device_type,
                              cl_uint num_entries,
                              cl_device_id* devices,
                              cl_uint* num_devices) {
    if (num_devices) {
        *num_devices = 1;
    }
    if (num_entries > 0 && devices) {
        devices[0] = fake_device();
    }
    return CL_SUCCESS;
}

cl_int fake_cl_get_device_info(cl_device_id,
                               cl_device_info,
                               size_t param_value_size,
                               void* param_value,
                               size_t* param_value_size_ret) {
    constexpr cl_ulong kMemorySize = 64 * 1024 * 1024;
    constexpr const char* kDeviceName = "fake-opencl-device";

    if (param_value_size == sizeof(cl_ulong) && param_value) {
        *static_cast<cl_ulong*>(param_value) = kMemorySize;
        if (param_value_size_ret) {
            *param_value_size_ret = sizeof(cl_ulong);
        }
        return CL_SUCCESS;
    }

    if (param_value && param_value_size > 0) {
        std::strncpy(static_cast<char*>(param_value), kDeviceName, param_value_size - 1);
        static_cast<char*>(param_value)[param_value_size - 1] = '\0';
    }
    if (param_value_size_ret) {
        *param_value_size_ret = std::strlen(kDeviceName) + 1;
    }
    return CL_SUCCESS;
}

cl_context fake_cl_create_context(const cl_context_properties*,
                                  cl_uint,
                                  const cl_device_id*,
                                  void (*)(const char*, const void*, size_t, void*),
                                  void*,
                                  cl_int* errcode_ret) {
    if (errcode_ret) {
        *errcode_ret = CL_SUCCESS;
    }
    return fake_context();
}

cl_int fake_cl_release_context(cl_context) {
    return CL_SUCCESS;
}

cl_command_queue fake_cl_create_command_queue(cl_context,
                                              cl_device_id,
                                              cl_command_queue_properties,
                                              cl_int* errcode_ret) {
    if (errcode_ret) {
        *errcode_ret = CL_SUCCESS;
    }
    return fake_queue();
}

cl_int fake_cl_release_command_queue(cl_command_queue) {
    return CL_SUCCESS;
}

cl_mem fake_cl_create_buffer(cl_context, cl_mem_flags, size_t, void*, cl_int* errcode_ret) {
    if (errcode_ret) {
        *errcode_ret = CL_SUCCESS;
    }
    return reinterpret_cast<cl_mem>(0x1005);
}

cl_int fake_cl_release_mem_object(cl_mem) {
    return CL_SUCCESS;
}

cl_int fake_cl_enqueue_read_buffer(cl_command_queue,
                                   cl_mem,
                                   cl_bool,
                                   size_t,
                                   size_t,
                                   void*,
                                   cl_uint,
                                   const cl_event*,
                                   cl_event*) {
    return CL_SUCCESS;
}

cl_int fake_cl_enqueue_write_buffer(cl_command_queue,
                                    cl_mem,
                                    cl_bool,
                                    size_t,
                                    size_t,
                                    const void*,
                                    cl_uint,
                                    const cl_event*,
                                    cl_event*) {
    return CL_SUCCESS;
}

cl_int fake_cl_enqueue_copy_buffer(cl_command_queue,
                                   cl_mem,
                                   cl_mem,
                                   size_t,
                                   size_t,
                                   size_t,
                                   cl_uint,
                                   const cl_event*,
                                   cl_event*) {
    return CL_SUCCESS;
}

cl_int fake_cl_finish(cl_command_queue) {
    return CL_SUCCESS;
}

cl_int fake_cl_flush(cl_command_queue) {
    return CL_SUCCESS;
}

OpenCLFunctionPointers make_fake_opencl_functions() {
    OpenCLFunctionPointers functions;
    functions.clGetPlatformIDs = fake_cl_get_platform_ids;
    functions.clGetDeviceIDs = fake_cl_get_device_ids;
    functions.clGetDeviceInfo = fake_cl_get_device_info;
    functions.clCreateContext = fake_cl_create_context;
    functions.clReleaseContext = fake_cl_release_context;
    functions.clCreateCommandQueue = fake_cl_create_command_queue;
    functions.clReleaseCommandQueue = fake_cl_release_command_queue;
    functions.clCreateBuffer = fake_cl_create_buffer;
    functions.clReleaseMemObject = fake_cl_release_mem_object;
    functions.clEnqueueReadBuffer = fake_cl_enqueue_read_buffer;
    functions.clEnqueueWriteBuffer = fake_cl_enqueue_write_buffer;
    functions.clEnqueueCopyBuffer = fake_cl_enqueue_copy_buffer;
    functions.clFinish = fake_cl_finish;
    functions.clFlush = fake_cl_flush;
    return functions;
}

class FakeOpenCLLoaderScope {
public:
    FakeOpenCLLoaderScope() {
        auto& loader = OpenCLLoader::instance();
        std::lock_guard<std::mutex> lock(loader.mutex_);
        original_is_loaded_ = loader.is_loaded_;
        original_dll_handle_ = loader.dll_handle_;
        original_dll_path_ = loader.dll_path_;
        original_functions_ = loader.functions_;

        loader.is_loaded_ = true;
        loader.dll_handle_ = reinterpret_cast<void*>(0x1);
        loader.dll_path_ = "fake-opencl";
        loader.functions_ = make_fake_opencl_functions();
    }

    ~FakeOpenCLLoaderScope() {
        auto& loader = OpenCLLoader::instance();
        std::lock_guard<std::mutex> lock(loader.mutex_);
        loader.is_loaded_ = original_is_loaded_;
        loader.dll_handle_ = original_dll_handle_;
        loader.dll_path_ = original_dll_path_;
        loader.functions_ = original_functions_;
    }

private:
    bool original_is_loaded_ = false;
    void* original_dll_handle_ = nullptr;
    std::string original_dll_path_;
    OpenCLFunctionPointers original_functions_;
};

GpuExecutorConfig make_config() {
    GpuExecutorConfig config;
    config.name = "opencl_start_protection";
    config.backend = GpuBackend::OPENCL;
    config.device_id = 0;
    config.default_stream_count = 1;
    config.max_queue_size = 16;
    return config;
}

}  // namespace

TEST(OpenCLExecutorStartProtection, ConcurrentStartCreatesSingleWorker) {
    FakeOpenCLLoaderScope fake_loader;
    OpenCLExecutor executor("opencl_start_protection", make_config());

    std::atomic<int> worker_create_count{0};
    executor.worker_thread_factory_for_test_ = [&worker_create_count](OpenCLExecutor* self) {
        worker_create_count.fetch_add(1, std::memory_order_acq_rel);
        return std::thread(&OpenCLExecutor::worker_thread, self);
    };

    constexpr int kThreadCount = 8;
    std::atomic<int> ready_count{0};
    std::atomic<bool> start_gate{false};
    std::atomic<int> true_count{0};
    std::vector<std::thread> callers;
    callers.reserve(kThreadCount);

    for (int i = 0; i < kThreadCount; ++i) {
        callers.emplace_back([&]() {
            ready_count.fetch_add(1, std::memory_order_acq_rel);
            while (!start_gate.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            if (executor.start()) {
                true_count.fetch_add(1, std::memory_order_acq_rel);
            }
        });
    }

    while (ready_count.load(std::memory_order_acquire) != kThreadCount) {
        std::this_thread::yield();
    }
    start_gate.store(true, std::memory_order_release);

    for (auto& caller : callers) {
        caller.join();
    }

    EXPECT_EQ(worker_create_count.load(std::memory_order_acquire), 1);
    EXPECT_EQ(true_count.load(std::memory_order_acquire), kThreadCount);
    EXPECT_TRUE(executor.get_status().is_running);
    executor.stop();
}

TEST(OpenCLExecutorStartProtection, ThreadCreationFailureRollsBackRunningState) {
    FakeOpenCLLoaderScope fake_loader;
    OpenCLExecutor executor("opencl_start_protection", make_config());
    executor.worker_thread_factory_for_test_ = [](OpenCLExecutor*) -> std::thread {
        throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again),
                                "injected thread creation failure");
    };

    EXPECT_FALSE(executor.start());

    auto status = executor.get_status();
    EXPECT_FALSE(status.is_running);
    EXPECT_NE(status.last_error_message.find("OpenCL worker thread creation failed"),
              std::string::npos)
        << status.last_error_message;

    executor.worker_thread_factory_for_test_ = [](OpenCLExecutor* self) {
        return std::thread(&OpenCLExecutor::worker_thread, self);
    };

    EXPECT_TRUE(executor.start()) << executor.get_status().last_error_message;
    EXPECT_TRUE(executor.get_status().is_running);
    executor.stop();
}
