#include <gtest/gtest.h>
#include "kairo/gpu/opencl_executor.hpp"
#include "kairo/gpu/opencl_loader.hpp"
#include "kairo/types.hpp"
#include <atomic>
#include <stdexcept>
#include <thread>
#include <vector>

#include <chrono>
#include <future>
#include <thread>
#include <vector>

using namespace kairo::gpu;

class OpenCLExecutorTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 尝试加载 OpenCL
        auto& loader = OpenCLLoader::instance();
        opencl_available_ = loader.load();

        if (opencl_available_) {
            GpuExecutorConfig config;
            config.name = "test_opencl";
            config.backend = GpuBackend::OPENCL;
            config.device_id = 0;
            config.default_stream_count = 2;

            executor_ = std::make_unique<OpenCLExecutor>("test_opencl", config);
        }
    }

    void TearDown() override {
        executor_.reset();
    }

    bool opencl_available_ = false;
    std::unique_ptr<OpenCLExecutor> executor_;
};

TEST_F(OpenCLExecutorTest, LoaderTest) {
    auto& loader = OpenCLLoader::instance();

    if (loader.is_available()) {
        EXPECT_FALSE(loader.get_dll_path().empty());
        const auto& funcs = loader.get_functions();
        EXPECT_TRUE(funcs.is_complete());
    }
}

TEST_F(OpenCLExecutorTest, ExecutorCreation) {
    if (!opencl_available_) {
        GTEST_SKIP() << "OpenCL not available";
    }

    ASSERT_NE(executor_, nullptr);
    if (!executor_->start()) {
        GTEST_SKIP() << "OpenCL start failed";
    }
    EXPECT_EQ(executor_->get_name(), "test_opencl");
}

TEST_F(OpenCLExecutorTest, DeviceInfo) {
    if (!opencl_available_) {
        GTEST_SKIP() << "OpenCL not available";
    }

    if (!executor_->start()) {
        GTEST_SKIP() << "OpenCL start failed";
    }

    auto info = executor_->get_device_info();
    EXPECT_EQ(info.backend, GpuBackend::OPENCL);
    EXPECT_FALSE(info.name.empty());
}

TEST_F(OpenCLExecutorTest, MemoryAllocation) {
    if (!opencl_available_) {
        GTEST_SKIP() << "OpenCL not available";
    }

    if (!executor_->start()) {
        GTEST_SKIP() << "OpenCL start failed";
    }

    const size_t size = 1024 * sizeof(float);
    void* ptr = executor_->allocate_device_memory(size);
    ASSERT_NE(ptr, nullptr);

    executor_->free_device_memory(ptr);
}

TEST_F(OpenCLExecutorTest, MemoryCopy) {
    if (!opencl_available_) {
        GTEST_SKIP() << "OpenCL not available";
    }

    if (!executor_->start()) {
        GTEST_SKIP() << "OpenCL start failed";
    }

    const size_t size = 1024 * sizeof(float);
    std::vector<float> host_data(1024, 1.0f);
    std::vector<float> result_data(1024, 0.0f);

    void* device_ptr = executor_->allocate_device_memory(size);
    ASSERT_NE(device_ptr, nullptr);

    EXPECT_TRUE(executor_->copy_to_device(device_ptr, host_data.data(), size));
    EXPECT_TRUE(executor_->copy_to_host(result_data.data(), device_ptr, size));

    for (size_t i = 0; i < 1024; ++i) {
        EXPECT_FLOAT_EQ(result_data[i], 1.0f);
    }

    executor_->free_device_memory(device_ptr);
}

TEST_F(OpenCLExecutorTest, StreamManagement) {
    if (!opencl_available_) {
        GTEST_SKIP() << "OpenCL not available";
    }

    if (!executor_->start()) {
        GTEST_SKIP() << "OpenCL start failed";
    }

    int stream_id = executor_->create_stream();
    EXPECT_GE(stream_id, 0);

    executor_->synchronize_stream(stream_id);
    executor_->destroy_stream(stream_id);
}

TEST_F(OpenCLExecutorTest, StreamCallbackCapability) {
    if (!opencl_available_) {
        GTEST_SKIP() << "OpenCL not available; cannot validate stream callback capability";
    }

    ASSERT_NE(executor_, nullptr);
    if (!executor_->start()) {
        GTEST_SKIP() << "OpenCL start failed; cannot validate stream callback capability";
    }

    EXPECT_FALSE(executor_->supports_stream_callback());
    EXPECT_FALSE(executor_->add_stream_callback(0, [] {}));
    EXPECT_NE(executor_->get_status().last_error_message.find("not supported"),
              std::string::npos);
    EXPECT_FALSE(executor_->add_stream_callback(0, {}));
    EXPECT_NE(executor_->get_status().last_error_message.find("callback is null"),
              std::string::npos);
    EXPECT_FALSE(executor_->add_stream_callback(-1, [] {}));
    EXPECT_NE(executor_->get_status().last_error_message.find("invalid stream_id"),
              std::string::npos);
}

TEST_F(OpenCLExecutorTest, InvalidStreamIdDoesNotFallbackToDefault) {
    if (!opencl_available_) {
        GTEST_SKIP() << "OpenCL not available";
    }

    if (!executor_->start()) {
        GTEST_SKIP() << "OpenCL start failed";
    }

    const size_t size = 16 * sizeof(float);
    std::vector<float> host_data(16, 1.0f);
    std::vector<float> result_data(16, 0.0f);

    void* device_ptr = executor_->allocate_device_memory(size);
    ASSERT_NE(device_ptr, nullptr);

    auto expect_invalid_stream = [&](int stream_id) {
        EXPECT_FALSE(executor_->copy_to_device(device_ptr, host_data.data(), size, false, stream_id))
            << "stream_id=" << stream_id;
        EXPECT_FALSE(executor_->copy_to_host(result_data.data(), device_ptr, size, false, stream_id))
            << "stream_id=" << stream_id;

        GpuTaskConfig config;
        config.stream_id = stream_id;
        bool executed = false;
        auto future = executor_->submit_kernel([&executed](void*) {
            executed = true;
        }, config);

        EXPECT_THROW(future.get(), std::runtime_error) << "stream_id=" << stream_id;
        EXPECT_FALSE(executed) << "stream_id=" << stream_id;
    };

    expect_invalid_stream(-1);
    expect_invalid_stream(9999);

    int stream_id = executor_->create_stream();
    ASSERT_GE(stream_id, 0);
    executor_->destroy_stream(stream_id);
    expect_invalid_stream(stream_id);

    executor_->free_device_memory(device_ptr);
}

TEST_F(OpenCLExecutorTest, DestroyStreamRaceWithSubmit) {
    if (!opencl_available_) {
        GTEST_SKIP() << "OpenCL not available";
    }

    if (!executor_->start()) {
        GTEST_SKIP() << "OpenCL start failed";
    }

    constexpr int kIterations = 1000;
    const size_t element_count = 64;
    const size_t size = element_count * sizeof(float);
    std::vector<float> host_data(element_count, 1.0f);

    void* device_ptr = executor_->allocate_device_memory(size);
    ASSERT_NE(device_ptr, nullptr);

    int initial_stream = executor_->create_stream();
    if (initial_stream < 0) {
        executor_->free_device_memory(device_ptr);
        FAIL() << "Failed to create OpenCL stream";
    }
    std::atomic<int> stream_id{initial_stream};

    std::atomic<bool> start{false};
    std::atomic<bool> create_failed{false};

    std::thread submitter([&]() {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        for (int i = 0; i < kIterations; ++i) {
            GpuTaskConfig config;
            config.stream_id = stream_id.load(std::memory_order_acquire);
            auto future = executor_->submit_kernel([](void*) {}, config);
            executor_->copy_to_device(device_ptr, host_data.data(), size, true, config.stream_id);
            future.wait();
        }
    });

    std::thread destroyer([&]() {
        start.store(true, std::memory_order_release);
        for (int i = 0; i < kIterations; ++i) {
            int current_stream = stream_id.load(std::memory_order_acquire);
            executor_->synchronize_stream(current_stream);
            executor_->destroy_stream(current_stream);

            int new_stream = executor_->create_stream();
            if (new_stream < 0) {
                create_failed.store(true, std::memory_order_release);
                break;
            }
            stream_id.store(new_stream, std::memory_order_release);
        }
    });

    submitter.join();
    destroyer.join();

    EXPECT_FALSE(create_failed.load(std::memory_order_acquire));
    executor_->destroy_stream(stream_id.load(std::memory_order_acquire));
    executor_->free_device_memory(device_ptr);
}

TEST_F(OpenCLExecutorTest, KernelSubmission) {
    if (!opencl_available_) {
        GTEST_SKIP() << "OpenCL not available";
    }

    if (!executor_->start()) {
        GTEST_SKIP() << "OpenCL start failed";
    }

    GpuTaskConfig config;
    config.stream_id = 0;

    bool executed = false;
    auto future = executor_->submit_kernel(
        [&executed](void*) { executed = true; },
        config
    );

    future.wait();
    EXPECT_TRUE(executed);
}

TEST_F(OpenCLExecutorTest, StopDrainsOrFailsPendingFutures) {
    if (!opencl_available_) {
        GTEST_SKIP() << "OpenCL not available";
    }

    if (!executor_->start()) {
        GTEST_SKIP() << "OpenCL platform/device not available";
    }

    std::atomic<bool> first_started{false};
    std::atomic<bool> release_first{false};

    GpuTaskConfig config;
    config.stream_id = 0;

    auto first = executor_->submit_kernel([&](void*) {
        first_started.store(true, std::memory_order_release);
        while (!release_first.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }, config);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!first_started.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(first_started.load(std::memory_order_acquire));

    constexpr int kTrailingTasks = 15;
    std::vector<std::future<void>> trailing_futures;
    trailing_futures.reserve(kTrailingTasks);
    for (int i = 0; i < kTrailingTasks; ++i) {
        trailing_futures.push_back(executor_->submit_kernel([](void*) {}, config));
    }

    std::thread stopper([&] { executor_->stop(); });
    for (auto& future : trailing_futures) {
        EXPECT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    }

    auto status = executor_->get_status();
    EXPECT_FALSE(status.is_running);
    EXPECT_EQ(status.queue_size, 0U);
    EXPECT_GE(status.failed_kernels, kTrailingTasks);
    EXPECT_NE(status.last_error_message.find("cancelled"), std::string::npos);

    release_first.store(true, std::memory_order_release);
    stopper.join();

    EXPECT_NO_THROW(first.get());
    for (auto& future : trailing_futures) {
        EXPECT_THROW(future.get(), kairo::ExecutorStopping);
    }
}

TEST_F(OpenCLExecutorTest, OpenCLExecutorRespectsMaxQueueSize) {
    if (!opencl_available_) {
        GTEST_SKIP() << "OpenCL not available";
    }

    GpuExecutorConfig config;
    config.name = "bounded_opencl";
    config.backend = GpuBackend::OPENCL;
    config.device_id = 0;
    config.default_stream_count = 1;
    config.max_queue_size = 1;

    OpenCLExecutor executor("bounded_opencl", config);
    if (!executor.start()) {
        GTEST_SKIP() << "OpenCL start failed";
    }

    std::atomic<bool> first_started{false};
    std::atomic<bool> release_first{false};
    std::atomic<bool> second_started{false};
    std::atomic<bool> release_second{false};

    auto wait_until = [](const std::function<bool()>& predicate,
                         std::chrono::milliseconds timeout) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (predicate()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return predicate();
    };

    auto release_all = [&]() {
        release_first.store(true, std::memory_order_release);
        release_second.store(true, std::memory_order_release);
    };

    GpuTaskConfig task_config;
    task_config.stream_id = 0;

    auto first = executor.submit_kernel([&](void*) {
        first_started.store(true, std::memory_order_release);
        while (!release_first.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }, task_config);

    if (!wait_until([&]() {
            return first_started.load(std::memory_order_acquire);
        }, std::chrono::seconds(2))) {
        release_all();
        first.wait();
        FAIL() << "First OpenCL kernel did not start";
    }

    auto second = executor.submit_kernel([&](void*) {
        second_started.store(true, std::memory_order_release);
        while (!release_second.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }, task_config);

    if (!wait_until([&]() {
            return executor.get_status().queue_size == 1;
        }, std::chrono::seconds(2))) {
        release_all();
        first.wait();
        second.wait();
        FAIL() << "Second OpenCL kernel was not queued";
    }

    auto third_submit = std::async(std::launch::async, [&executor, task_config]() {
        return executor.submit_kernel([](void*) {}, task_config);
    });

    EXPECT_EQ(third_submit.wait_for(std::chrono::milliseconds(100)),
              std::future_status::timeout);
    EXPECT_LE(executor.get_status().queue_size, 1u);

    release_first.store(true, std::memory_order_release);

    if (third_submit.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        release_all();
        executor.stop();
        third_submit.wait();
        FAIL() << "Third OpenCL submit did not resume after queue space became available";
    }
    auto third = third_submit.get();

    if (!wait_until([&]() {
            return second_started.load(std::memory_order_acquire);
        }, std::chrono::seconds(2))) {
        release_all();
        first.wait();
        second.wait();
        third.wait();
        FAIL() << "Second OpenCL kernel did not start";
    }

    release_second.store(true, std::memory_order_release);
    first.get();
    second.get();
    third.get();
}

TEST_F(OpenCLExecutorTest, OpenCLExecutorStatusReportsQueueAndMemory) {
    if (!opencl_available_) {
        GTEST_SKIP() << "OpenCL not available";
    }

    if (!executor_->start()) {
        GTEST_SKIP() << "OpenCL start failed";
    }

    auto before = executor_->get_status();
    EXPECT_EQ(before.queue_size, 0u);

    const size_t size = 4096;
    void* ptr = executor_->allocate_device_memory(size);
    if (ptr == nullptr) {
        GTEST_SKIP() << "OpenCL device memory allocation failed";
    }

    auto after_alloc = executor_->get_status();
    EXPECT_EQ(after_alloc.queue_size, 0u);
    EXPECT_GE(after_alloc.memory_used_bytes, before.memory_used_bytes + size);
    EXPECT_GT(after_alloc.memory_total_bytes, 0u);
    EXPECT_GT(after_alloc.memory_usage_percent, 0.0);

    executor_->free_device_memory(ptr);

    auto after_free = executor_->get_status();
    EXPECT_LE(after_free.memory_used_bytes, after_alloc.memory_used_bytes - size);
}

TEST_F(OpenCLExecutorTest, Status) {
    if (!opencl_available_) {
        GTEST_SKIP() << "OpenCL not available";
    }

    if (!executor_->start()) {
        GTEST_SKIP() << "OpenCL start failed";
    }

    auto status = executor_->get_status();
    EXPECT_EQ(status.name, "test_opencl");
    EXPECT_TRUE(status.is_running);
    EXPECT_EQ(status.backend, GpuBackend::OPENCL);
}
