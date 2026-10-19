/// @file test_gpu_stop_mode_interface.cpp
/// CR-154（0.7.2 §4.3）GpuStopMode 接口默认实现的独立验证：
///
/// IGpuExecutor::stop(gpu::GpuStopMode) 的默认实现必须忽略模式、等价
/// stop()——第三方实现（不在库内覆写）行为与 0.7.1 完全一致。用最小
/// 派生类（所有纯虚函数为哑实现）直接验证，不依赖任何 GPU 后端/硬件。

#include <kairo/interfaces.hpp>
#include <kairo/types.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <gtest/gtest.h>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace kairo;

namespace {

/// 最小派生类：纯虚函数全部哑实现，不覆写 stop(mode)——考察默认实现。
class MinimalGpuExecutor : public IGpuExecutor {
public:
    // stop()/stop(mode) 两个重载都要可见（override stop() 会隐藏基类
    // 的 stop(mode)，与实现侧 CudaExecutor::stop 的做法一致）。
    using IGpuExecutor::stop;

    void* allocate_device_memory(size_t) override { return nullptr; }
    void free_device_memory(void*) override {}
    bool copy_to_device(void*, const void*, size_t, bool, int) override { return false; }
    bool copy_to_host(void*, const void*, size_t, bool, int) override { return false; }
    bool copy_device_to_device(void*, const void*, size_t, bool, int) override { return false; }
    void synchronize() override { ++synchronize_calls; }
    void synchronize_stream(int) override {}
    int create_stream() override { return 0; }
    void destroy_stream(int) override {}
    bool add_stream_callback(int, std::function<void()>) override { return false; }
    std::string get_name() const override { return "minimal"; }
    gpu::GpuDeviceInfo get_device_info() const override { return {}; }
    gpu::GpuExecutorStatus get_status() const override { return {}; }
    bool start() override { return true; }

    void stop() override {
        // 记录无参 stop() 被调用。
        stop_calls.fetch_add(1, std::memory_order_acq_rel);
        last_was_noarg.store(true, std::memory_order_release);
    }

    void wait_for_completion() override {}

    std::future<void> submit_kernel_impl(std::function<void(void*)>,
                                         const gpu::GpuTaskConfig&) override {
        std::promise<void> p;
        p.set_value();
        return p.get_future();
    }

    std::atomic<int> stop_calls{0};
    std::atomic<bool> last_was_noarg{false};
    int synchronize_calls = 0;
};

} // namespace

TEST(GpuStopModeCr154, DefaultStopModeImplementationDelegatesToPlainStop) {
    MinimalGpuExecutor executor;

    executor.stop(gpu::GpuStopMode::Drain);
    EXPECT_EQ(executor.stop_calls.load(), 1)
        << "default stop(mode) must delegate to stop()";
    EXPECT_TRUE(executor.last_was_noarg.load());

    executor.stop(gpu::GpuStopMode::Cancel);
    EXPECT_EQ(executor.stop_calls.load(), 2)
        << "default stop(mode) must ignore the mode value";

    executor.stop();
    EXPECT_EQ(executor.stop_calls.load(), 3);
}

TEST(GpuStopModeCr154, DerivedClassCanOverrideStopMode) {
    // 覆写 stop(mode) 的派生类：mode 必须原样到达实现（库内 CUDA/OpenCL
    // 后端的覆写契约）。
    struct RecordingExecutor : MinimalGpuExecutor {
        std::atomic<bool> saw_cancel{false};
        std::atomic<bool> saw_drain{false};
        using IGpuExecutor::stop;
        void stop(gpu::GpuStopMode mode) override {
            if (mode == gpu::GpuStopMode::Cancel) saw_cancel.store(true);
            if (mode == gpu::GpuStopMode::Drain) saw_drain.store(true);
        }
    } executor;

    EXPECT_FALSE(executor.saw_cancel.load());
    executor.stop(gpu::GpuStopMode::Cancel);
    EXPECT_TRUE(executor.saw_cancel.load());
    EXPECT_FALSE(executor.saw_drain.load());
    executor.stop(gpu::GpuStopMode::Drain);
    EXPECT_TRUE(executor.saw_drain.load());
    // 无参 stop() 不被 mode 覆写劫持。
    executor.stop();
    EXPECT_EQ(executor.stop_calls.load(), 1);
}

TEST(GpuStopModeCr154, GpuStopModeEnumValues) {
    // 枚举面钉死：Drain=0、Cancel=1（uint8_t 底层，跨 ABI 稳定）。
    static_assert(std::is_same_v<std::underlying_type_t<gpu::GpuStopMode>, std::uint8_t>);
    static_assert(static_cast<uint8_t>(gpu::GpuStopMode::Drain) == 0);
    static_assert(static_cast<uint8_t>(gpu::GpuStopMode::Cancel) == 1);
    SUCCEED();
}
