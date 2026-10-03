#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <future>
#include <stdexcept>
#include <string>
#include <vector>

#define private public
#include "kairo/gpu/opencl_executor.hpp"
#include "kairo/gpu/opencl_loader.hpp"
#undef private

using kairo::gpu::GpuBackend;
using kairo::gpu::GpuExecutorConfig;
using kairo::gpu::GpuTaskConfig;
using kairo::gpu::OpenCLExecutor;
using kairo::gpu::OpenCLFunctionPointers;
using kairo::gpu::OpenCLLoader;

namespace {

std::atomic<int> write_enqueue_count{0};
std::atomic<int> copy_enqueue_count{0};
std::atomic<int> finish_count{0};
std::atomic<cl_int> finish_result{CL_SUCCESS};
std::atomic<std::uintptr_t> next_buffer{0x5};

cl_int fake_cl_get_platform_ids(cl_uint num_entries,
                                cl_platform_id* platforms,
                                cl_uint* num_platforms) {
    if (num_platforms != nullptr) {
        *num_platforms = 1;
    }
    if (platforms != nullptr && num_entries > 0) {
        platforms[0] = reinterpret_cast<cl_platform_id>(0x1);
    }
    return CL_SUCCESS;
}

cl_int fake_cl_get_device_ids(cl_platform_id,
                              cl_device_type,
                              cl_uint num_entries,
                              cl_device_id* devices,
                              cl_uint* num_devices) {
    if (num_devices != nullptr) {
        *num_devices = 1;
    }
    if (devices != nullptr && num_entries > 0) {
        devices[0] = reinterpret_cast<cl_device_id>(0x2);
    }
    return CL_SUCCESS;
}

cl_int fake_cl_get_device_info(cl_device_id,
                               cl_device_info,
                               size_t param_value_size,
                               void* param_value,
                               size_t* param_value_size_ret) {
    constexpr cl_ulong kMemorySize = 1024 * 1024;
    if (param_value != nullptr && param_value_size >= sizeof(kMemorySize)) {
        std::memcpy(param_value, &kMemorySize, sizeof(kMemorySize));
    }
    if (param_value_size_ret != nullptr) {
        *param_value_size_ret = sizeof(kMemorySize);
    }
    return CL_SUCCESS;
}

cl_context fake_cl_create_context(const cl_context_properties*,
                                  cl_uint,
                                  const cl_device_id*,
                                  void (*)(const char*, const void*, size_t, void*),
                                  void*,
                                  cl_int* errcode_ret) {
    if (errcode_ret != nullptr) {
        *errcode_ret = CL_SUCCESS;
    }
    return reinterpret_cast<cl_context>(0x3);
}

cl_int fake_cl_release_context(cl_context) {
    return CL_SUCCESS;
}

cl_command_queue fake_cl_create_command_queue(cl_context,
                                              cl_device_id,
                                              cl_command_queue_properties,
                                              cl_int* errcode_ret) {
    if (errcode_ret != nullptr) {
        *errcode_ret = CL_SUCCESS;
    }
    return reinterpret_cast<cl_command_queue>(0x4);
}

cl_int fake_cl_release_command_queue(cl_command_queue) {
    return CL_SUCCESS;
}

cl_mem fake_cl_create_buffer(cl_context, cl_mem_flags, size_t, void*, cl_int* errcode_ret) {
    if (errcode_ret != nullptr) {
        *errcode_ret = CL_SUCCESS;
    }
    return reinterpret_cast<cl_mem>(next_buffer.fetch_add(1));
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
    write_enqueue_count.fetch_add(1);
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
    copy_enqueue_count.fetch_add(1);
    return CL_SUCCESS;
}

cl_int fake_cl_finish(cl_command_queue) {
    finish_count.fetch_add(1);
    return finish_result.load();
}

cl_int fake_cl_flush(cl_command_queue) {
    return CL_SUCCESS;
}

class FakeOpenCLLoaderScope {
public:
    FakeOpenCLLoaderScope()
        : loader_(OpenCLLoader::instance())
        , original_is_loaded_(loader_.is_loaded_)
        , original_dll_handle_(loader_.dll_handle_)
        , original_dll_path_(loader_.dll_path_)
        , original_functions_(loader_.functions_) {
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

        loader_.is_loaded_ = true;
        loader_.dll_handle_ = nullptr;
        loader_.dll_path_ = "fake-opencl";
        loader_.functions_ = functions;
        write_enqueue_count = 0;
        copy_enqueue_count = 0;
        finish_count = 0;
        finish_result = CL_SUCCESS;
    }

    ~FakeOpenCLLoaderScope() {
        loader_.functions_ = original_functions_;
        loader_.dll_path_ = original_dll_path_;
        loader_.dll_handle_ = original_dll_handle_;
        loader_.is_loaded_ = original_is_loaded_;
    }

    FakeOpenCLLoaderScope(const FakeOpenCLLoaderScope&) = delete;
    FakeOpenCLLoaderScope& operator=(const FakeOpenCLLoaderScope&) = delete;

private:
    OpenCLLoader& loader_;
    bool original_is_loaded_;
    void* original_dll_handle_;
    std::string original_dll_path_;
    OpenCLFunctionPointers original_functions_;
};

GpuExecutorConfig make_opencl_config() {
    GpuExecutorConfig config;
    config.name = "opencl_kernel_last_error";
    config.backend = GpuBackend::OPENCL;
    config.device_id = 0;
    config.default_stream_count = 1;
    config.max_queue_size = 16;
    return config;
}

bool contains(const std::string& message, const std::string& needle) {
    return message.find(needle) != std::string::npos;
}

}  // namespace

TEST(OpenCLKernelExceptionLastError, InvalidStreamSetsLastError) {
    FakeOpenCLLoaderScope fake_loader;
    OpenCLExecutor executor("opencl_kernel_last_error", make_opencl_config());
    ASSERT_TRUE(executor.start()) << executor.get_status().last_error_message;

    GpuTaskConfig task_config;
    task_config.stream_id = 9999;

    auto future = executor.submit_kernel([](void*) {}, task_config);
    EXPECT_THROW(future.get(), std::runtime_error);

    const auto status = executor.get_status();
    EXPECT_EQ(status.failed_kernels, 1U);
    EXPECT_TRUE(contains(status.last_error_message, "submit_kernel"))
        << status.last_error_message;
    EXPECT_TRUE(contains(status.last_error_message, "invalid stream_id 9999"))
        << status.last_error_message;
}

TEST(OpenCLKernelExceptionLastError, KernelFuncExceptionSetsLastError) {
    FakeOpenCLLoaderScope fake_loader;
    OpenCLExecutor executor("opencl_kernel_last_error", make_opencl_config());
    ASSERT_TRUE(executor.start()) << executor.get_status().last_error_message;

    GpuTaskConfig task_config;
    task_config.stream_id = 0;

    auto future = executor.submit_kernel([](void*) {
        throw std::runtime_error("user-fault");
    }, task_config);

    try {
        future.get();
        FAIL() << "Expected kernel exception";
    } catch (const std::runtime_error& ex) {
        EXPECT_TRUE(contains(ex.what(), "user-fault")) << ex.what();
    } catch (...) {
        FAIL() << "Expected std::runtime_error";
    }

    const auto status = executor.get_status();
    EXPECT_EQ(status.failed_kernels, 1U);
    EXPECT_TRUE(contains(status.last_error_message, "submit_kernel: kernel_func exception"))
        << status.last_error_message;
    EXPECT_TRUE(contains(status.last_error_message, "user-fault"))
        << status.last_error_message;
}

TEST(OpenCLCopyValidation, CopyRejectsOversizedDeviceRange) {
    FakeOpenCLLoaderScope fake_loader;
    OpenCLExecutor executor("opencl_copy_validation", make_opencl_config());
    ASSERT_TRUE(executor.start()) << executor.get_status().last_error_message;

    void* device_ptr = executor.allocate_device_memory(1024);
    ASSERT_NE(device_ptr, nullptr);
    std::vector<std::byte> host_buffer(2048);

    EXPECT_FALSE(executor.copy_to_device(device_ptr, host_buffer.data(), host_buffer.size()));
    EXPECT_TRUE(contains(executor.get_status().last_error_message, "requested 2048 bytes"));
    EXPECT_EQ(write_enqueue_count.load(), 0);
}

TEST(OpenCLCopyValidation, CopyRejectsNullHostBuffer) {
    FakeOpenCLLoaderScope fake_loader;
    OpenCLExecutor executor("opencl_copy_validation", make_opencl_config());
    ASSERT_TRUE(executor.start()) << executor.get_status().last_error_message;

    void* device_ptr = executor.allocate_device_memory(1024);
    ASSERT_NE(device_ptr, nullptr);

    EXPECT_FALSE(executor.copy_to_device(device_ptr, nullptr, 1024));
    EXPECT_TRUE(contains(executor.get_status().last_error_message, "host source pointer must not be null"));
    EXPECT_EQ(write_enqueue_count.load(), 0);
}

TEST(OpenCLCopyValidation, DeviceToDeviceRejectsSmallerSourceOrDestination) {
    FakeOpenCLLoaderScope fake_loader;
    OpenCLExecutor executor("opencl_copy_validation", make_opencl_config());
    ASSERT_TRUE(executor.start()) << executor.get_status().last_error_message;

    void* small = executor.allocate_device_memory(1024);
    void* large = executor.allocate_device_memory(2048);
    ASSERT_NE(small, nullptr);
    ASSERT_NE(large, nullptr);

    EXPECT_FALSE(executor.copy_device_to_device(large, small, 2048));
    EXPECT_TRUE(contains(executor.get_status().last_error_message, "source"));
    EXPECT_EQ(copy_enqueue_count.load(), 0);

    EXPECT_FALSE(executor.copy_device_to_device(small, large, 2048));
    EXPECT_TRUE(contains(executor.get_status().last_error_message, "destination"));
    EXPECT_EQ(copy_enqueue_count.load(), 0);
}

TEST(OpenCLCopyDeviceToDevice, HonorsAsync) {
    FakeOpenCLLoaderScope fake_loader;
    OpenCLExecutor executor("opencl_device_copy_async", make_opencl_config());
    ASSERT_TRUE(executor.start()) << executor.get_status().last_error_message;

    void* source = executor.allocate_device_memory(1024);
    void* destination = executor.allocate_device_memory(1024);
    ASSERT_NE(source, nullptr);
    ASSERT_NE(destination, nullptr);

    EXPECT_TRUE(executor.copy_device_to_device(destination, source, 1024, false));
    EXPECT_EQ(copy_enqueue_count.load(), 1);
    EXPECT_EQ(finish_count.load(), 1);

    EXPECT_TRUE(executor.copy_device_to_device(destination, source, 1024, true));
    EXPECT_EQ(copy_enqueue_count.load(), 2);
    EXPECT_EQ(finish_count.load(), 1);

    finish_result = CL_INVALID_COMMAND_QUEUE;
    EXPECT_FALSE(executor.copy_device_to_device(destination, source, 1024, false));
    EXPECT_EQ(finish_count.load(), 2);
    EXPECT_TRUE(contains(executor.get_status().last_error_message, "clFinish"));
}
