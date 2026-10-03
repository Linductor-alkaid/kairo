#include <atomic>
#include <chrono>
#include <fstream>
#include <future>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <kairo/interfaces.hpp>

namespace {

class MockGpuDependencyExecutor : public kairo::IGpuExecutor {
public:
    ~MockGpuDependencyExecutor() override { stop(); }

    bool start() override {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true)) {
            return false;
        }
        start_waiter_generation();
        return true;
    }

    void stop() override {
        running_.store(false);
        join_pending_waiters();
    }

    void wait_for_completion() override {}
    void synchronize() override {}
    void synchronize_stream(int) override {}
    int create_stream() override { return 0; }
    void destroy_stream(int) override {}
    bool add_stream_callback(int, std::function<void()> callback) override {
        if (callback) {
            callback();
        }
        return true;
    }
    void* allocate_device_memory(size_t) override { return nullptr; }
    void free_device_memory(void*) override {}
    bool copy_to_device(void*, const void*, size_t, bool, int) override { return true; }
    bool copy_to_host(void*, const void*, size_t, bool, int) override { return true; }
    bool copy_device_to_device(void*, const void*, size_t, bool, int) override { return true; }
    std::string get_name() const override { return "mock_gpu_dependency"; }
    kairo::gpu::GpuDeviceInfo get_device_info() const override { return {}; }
    kairo::gpu::GpuExecutorStatus get_status() const override { return {}; }

protected:
    std::future<void> submit_kernel_impl(
        std::function<void(void*)> kernel_func,
        const kairo::gpu::GpuTaskConfig&) override {
        std::promise<void> promise;
        auto future = promise.get_future();
        try {
            if (!running_.load()) {
                throw std::runtime_error("mock executor is not running");
            }
            kernel_func(nullptr);
            promise.set_value();
        } catch (...) {
            promise.set_exception(std::current_exception());
        }
        return future;
    }

private:
    std::atomic<bool> running_{false};
};

int process_thread_count() {
#if defined(__linux__) && !defined(__ANDROID__)
    std::ifstream status("/proc/self/status");
    std::string field;
    while (status >> field) {
        if (field == "Threads:") {
            int count = 0;
            status >> count;
            return count;
        }
        status.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    }
#else
    // Android 不保证应用可读 /proc/self/status；返回 -1 时测试自动跳过线程数比较。
#endif
    return -1;
}

TEST(GpuDependencyWaiterTest, BoundedPendingWaiters) {
    MockGpuDependencyExecutor executor;
    executor.set_max_pending_waiters(4);
    ASSERT_TRUE(executor.start());

    const int threads_before = process_thread_count();
    std::promise<void> never_ready;
    const auto dependency = never_ready.get_future().share();
    kairo::gpu::GpuTaskConfig config;

    std::vector<std::future<void>> accepted;
    for (int index = 0; index < 4; ++index) {
        accepted.push_back(executor.submit_kernel_after(dependency, [](void*) {}, config));
    }

    constexpr int excess_submissions = 32;
    for (int index = 0; index < excess_submissions; ++index) {
        auto future = executor.submit_kernel_after(dependency, [](void*) {}, config);
        EXPECT_EQ(future.wait_for(std::chrono::seconds(0)), std::future_status::ready);
        EXPECT_THROW(future.get(), std::runtime_error);
    }

    const int threads_during = process_thread_count();
    if (threads_before >= 0 && threads_during >= 0) {
        EXPECT_LE(threads_during, threads_before + 1);
    }

    executor.stop();
    for (auto& future : accepted) {
        EXPECT_EQ(future.wait_for(std::chrono::seconds(1)), std::future_status::ready);
        EXPECT_THROW(future.get(), std::runtime_error);
    }

    const int threads_after = process_thread_count();
    if (threads_before >= 0 && threads_after >= 0) {
        EXPECT_LE(threads_after, threads_before);
    }
}

}  // namespace
