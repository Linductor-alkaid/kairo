#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <future>
#include <mutex>
#include <string>
#include <thread>
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

std::atomic<bool> block_copy{false};
std::atomic<bool> copy_entered{false};
std::atomic<bool> release_copy{false};

cl_platform_id fake_platform() { return reinterpret_cast<cl_platform_id>(0x2001); }
cl_device_id fake_device() { return reinterpret_cast<cl_device_id>(0x2002); }
cl_context fake_context() { return reinterpret_cast<cl_context>(0x2003); }
cl_command_queue fake_queue() { return reinterpret_cast<cl_command_queue>(0x2004); }
cl_mem fake_buffer() { return reinterpret_cast<cl_mem>(0x2005); }

cl_int fake_get_platform_ids(cl_uint count, cl_platform_id* platforms, cl_uint* total) {
    if (total) *total = 1;
    if (count && platforms) platforms[0] = fake_platform();
    return CL_SUCCESS;
}
cl_int fake_get_device_ids(cl_platform_id, cl_device_type, cl_uint count,
                           cl_device_id* devices, cl_uint* total) {
    if (total) *total = 1;
    if (count && devices) devices[0] = fake_device();
    return CL_SUCCESS;
}
cl_int fake_get_device_info(cl_device_id, cl_device_info, size_t size, void* value, size_t*) {
    if (size == sizeof(cl_ulong)) *static_cast<cl_ulong*>(value) = 64 * 1024 * 1024;
    else if (value && size) std::strncpy(static_cast<char*>(value), "lifecycle-opencl", size - 1);
    return CL_SUCCESS;
}
cl_context fake_create_context(const cl_context_properties*, cl_uint, const cl_device_id*,
                               void (*)(const char*, const void*, size_t, void*), void*, cl_int* err) {
    if (err) *err = CL_SUCCESS;
    return fake_context();
}
cl_int fake_release_context(cl_context) { return CL_SUCCESS; }
cl_command_queue fake_create_queue(cl_context, cl_device_id, cl_command_queue_properties, cl_int* err) {
    if (err) *err = CL_SUCCESS;
    return fake_queue();
}
cl_int fake_release_queue(cl_command_queue) { return CL_SUCCESS; }
cl_mem fake_create_buffer(cl_context, cl_mem_flags, size_t, void*, cl_int* err) {
    if (err) *err = CL_SUCCESS;
    return fake_buffer();
}
cl_int fake_release_memory(cl_mem) { return CL_SUCCESS; }
cl_int fake_read(cl_command_queue, cl_mem, cl_bool, size_t, size_t, void*, cl_uint, const cl_event*, cl_event*) { return CL_SUCCESS; }
cl_int fake_write(cl_command_queue, cl_mem, cl_bool, size_t, size_t, const void*, cl_uint, const cl_event*, cl_event*) {
    if (block_copy.load(std::memory_order_acquire)) {
        copy_entered.store(true, std::memory_order_release);
        while (!release_copy.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }
    return CL_SUCCESS;
}
cl_int fake_copy(cl_command_queue, cl_mem, cl_mem, size_t, size_t, size_t, cl_uint, const cl_event*, cl_event*) { return CL_SUCCESS; }
cl_int fake_finish(cl_command_queue) { return CL_SUCCESS; }
cl_int fake_flush(cl_command_queue) { return CL_SUCCESS; }

OpenCLFunctionPointers fake_functions() {
    OpenCLFunctionPointers functions;
    functions.clGetPlatformIDs = fake_get_platform_ids;
    functions.clGetDeviceIDs = fake_get_device_ids;
    functions.clGetDeviceInfo = fake_get_device_info;
    functions.clCreateContext = fake_create_context;
    functions.clReleaseContext = fake_release_context;
    functions.clCreateCommandQueue = fake_create_queue;
    functions.clReleaseCommandQueue = fake_release_queue;
    functions.clCreateBuffer = fake_create_buffer;
    functions.clReleaseMemObject = fake_release_memory;
    functions.clEnqueueReadBuffer = fake_read;
    functions.clEnqueueWriteBuffer = fake_write;
    functions.clEnqueueCopyBuffer = fake_copy;
    functions.clFinish = fake_finish;
    functions.clFlush = fake_flush;
    return functions;
}

class FakeOpenCLLoaderScope {
public:
    FakeOpenCLLoaderScope() {
        auto& loader = OpenCLLoader::instance();
        std::lock_guard<std::mutex> lock(loader.mutex_);
        loaded_ = loader.is_loaded_;
        handle_ = loader.dll_handle_;
        path_ = loader.dll_path_;
        functions_ = loader.functions_;
        loader.is_loaded_ = true;
        loader.dll_handle_ = reinterpret_cast<void*>(0x1);
        loader.dll_path_ = "fake-opencl";
        loader.functions_ = fake_functions();
    }
    ~FakeOpenCLLoaderScope() {
        auto& loader = OpenCLLoader::instance();
        std::lock_guard<std::mutex> lock(loader.mutex_);
        loader.is_loaded_ = loaded_;
        loader.dll_handle_ = handle_;
        loader.dll_path_ = path_;
        loader.functions_ = functions_;
    }
private:
    bool loaded_ = false;
    void* handle_ = nullptr;
    std::string path_;
    OpenCLFunctionPointers functions_;
};

GpuExecutorConfig make_config() {
    GpuExecutorConfig config;
    config.name = "opencl_lifecycle";
    config.backend = GpuBackend::OPENCL;
    config.device_id = 0;
    config.default_stream_count = 1;
    config.max_queue_size = 16;
    return config;
}

}  // namespace

TEST(OpenCLExecutorLifecycleTest, ConcurrentStopWithCopyAllocateAndStatus) {
    FakeOpenCLLoaderScope fake_loader;
    OpenCLExecutor executor("opencl_lifecycle", make_config());
    ASSERT_TRUE(executor.start());

    constexpr size_t kSize = 64;
    std::vector<unsigned char> host(kSize, 7);
    void* initial_buffer = executor.allocate_device_memory(kSize);
    ASSERT_NE(initial_buffer, nullptr);

    std::atomic<bool> begin{false};
    std::atomic<bool> stop_requested{false};
    block_copy.store(true, std::memory_order_release);
    copy_entered.store(false, std::memory_order_release);
    release_copy.store(false, std::memory_order_release);
    std::thread caller([&] {
        while (!begin.load(std::memory_order_acquire)) std::this_thread::yield();
        do {
            executor.copy_to_device(initial_buffer, host.data(), host.size(), false);
            void* buffer = executor.allocate_device_memory(kSize);
            if (buffer) executor.free_device_memory(buffer);
            (void)executor.get_status();
            (void)executor.get_device_info();
            int stream = executor.create_stream();
            if (stream >= 0) {
                executor.synchronize_stream(stream);
                executor.destroy_stream(stream);
            }
        } while (!stop_requested.load(std::memory_order_acquire));
    });

    begin.store(true, std::memory_order_release);
    while (!copy_entered.load(std::memory_order_acquire)) std::this_thread::yield();
    std::thread stopper([&] {
        stop_requested.store(true, std::memory_order_release);
        executor.stop();
    });
    release_copy.store(true, std::memory_order_release);

    stopper.join();
    caller.join();
    block_copy.store(false, std::memory_order_release);
    EXPECT_FALSE(executor.get_status().is_running);
}

TEST(OpenCLExecutorLifecycleTest, StopFromInsideKernelFinalizesWithoutTerminate) {
    // 回归：kernel（运行在 worker 线程上）内部调用 stop() 时，
    // 旧实现 worker_.join() 会 join 自己抛 EDEADLK，异常逃逸后 stopping_
    // 永久为 true（start 全部失败），且析构时 joinable 的 std::thread 触发
    // std::terminate。修复后自停只请求停止，join 由后续外部 stop() 兜底。
    FakeOpenCLLoaderScope fake_loader;
    OpenCLExecutor executor("opencl_selfstop", make_config());
    ASSERT_TRUE(executor.start());

    std::atomic<bool> self_stop_returned{false};
    GpuTaskConfig config;
    auto future = executor.submit_kernel([&](void*) {
        executor.stop();
        self_stop_returned.store(true, std::memory_order_release);
    }, config);

    ASSERT_EQ(future.wait_for(std::chrono::seconds(10)), std::future_status::ready);
    future.get();  // 自停本身不应向 kernel future 注入异常
    EXPECT_TRUE(self_stop_returned.load(std::memory_order_acquire));
    EXPECT_FALSE(executor.get_status().is_running);

    // 自停后 worker 已退出但仍待 join：外部 stop() 必须完成 finalize，
    // 且 restart 不被卡死或拒绝。
    executor.stop();
    EXPECT_TRUE(executor.start());
    executor.stop();
    EXPECT_FALSE(executor.get_status().is_running);
}
