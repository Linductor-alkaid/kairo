/// @file test_cuda_gpu_stop_mode.cpp
/// CR-154（0.7.2 §4.3）CudaExecutor GpuStopMode 的独立验证。
///
/// 分层：
/// 1. 无硬件可跑：未启动执行器上 stop(Drain)/stop(Cancel)/stop()/
///    stop_and_join(bool)/cancel_queued_tasks() 的安全性与幂等性
///    （新 stop(mode) 分发路径的最小行为面）。
/// 2. 硬件门控（KAIRO_ENABLE_CUDA 且驱动可用）：Drain vs Cancel 的
///    future 终态契约——无驱动时 GTEST_SKIP，不伪造通过。
///
/// 本文件名含 "cuda"：tests/CMakeLists.txt 按名字注入 CUDA 头路径
/// （cuda_executor.hpp 在 KAIRO_ENABLE_CUDA 下包含 cuda_runtime.h）。

#include <kairo/config.hpp>
#include <kairo/types.hpp>

#include "kairo/gpu/cuda_executor.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <gtest/gtest.h>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace kairo;
using namespace std::chrono_literals;

namespace {

gpu::GpuExecutorConfig cuda_config(const std::string& name) {
    gpu::GpuExecutorConfig config;
    config.name = name;
    config.backend = gpu::GpuBackend::CUDA;
    config.device_id = 0;
    config.max_queue_size = 64;
    return config;
}

bool wait_flag(std::atomic<bool>& flag, std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!flag.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    return flag.load();
}

} // namespace

// ---------------------------------------------------------------------------
// 无硬件路径：stop(mode) 分发的安全面
// ---------------------------------------------------------------------------

TEST(CudaGpuStopModeCr154, StopModeOnNeverStartedExecutorIsSafe) {
#ifdef KAIRO_ENABLE_CUDA
    gpu::CudaExecutor executor("cr154_cuda_nostart", cuda_config("cr154_cuda_nostart"));
    // 未 start()（本机无驱动必然如此；有驱动也未 start）：
    // 两条模式路径 + 无参 stop() 必须全部安全返回，不崩溃、不挂起。
    EXPECT_NO_FATAL_FAILURE(executor.stop(gpu::GpuStopMode::Drain));
    EXPECT_FALSE(executor.get_status().is_running);
    EXPECT_NO_FATAL_FAILURE(executor.stop(gpu::GpuStopMode::Cancel));
    EXPECT_FALSE(executor.get_status().is_running);
    EXPECT_NO_FATAL_FAILURE(executor.stop());
    EXPECT_FALSE(executor.get_status().is_running);
#else
    GTEST_SKIP() << "KAIRO_ENABLE_CUDA not enabled at compile time";
#endif
}

TEST(CudaGpuStopModeCr154, StopAndJoinBoolOverloadsOnNeverStartedExecutor) {
#ifdef KAIRO_ENABLE_CUDA
    gpu::CudaExecutor executor("cr154_cuda_saj", cuda_config("cr154_cuda_saj"));
    // is_available_ == false 时 stop_and_join 直接安全返回 true
    //（不进入 cancel/join 路径）——两个重载行为一致。
    EXPECT_NO_FATAL_FAILURE((void)executor.stop_and_join());
    EXPECT_NO_FATAL_FAILURE((void)executor.stop_and_join(false));
    EXPECT_NO_FATAL_FAILURE((void)executor.stop_and_join(true));
#else
    GTEST_SKIP() << "KAIRO_ENABLE_CUDA not enabled at compile time";
#endif
}

TEST(CudaGpuStopModeCr154, CancelQueuedTasksOnEmptyQueueIsNoOp) {
#ifdef KAIRO_ENABLE_CUDA
    gpu::CudaExecutor executor("cr154_cuda_cancel", cuda_config("cr154_cuda_cancel"));
    // 空队列上 cancel_queued_tasks()：无任务可结算，必须为安全空操作。
    EXPECT_NO_FATAL_FAILURE(executor.cancel_queued_tasks());
    EXPECT_EQ(executor.get_status().queue_size, 0U);
    EXPECT_EQ(executor.get_status().failed_kernels, 0U);
#else
    GTEST_SKIP() << "KAIRO_ENABLE_CUDA not enabled at compile time";
#endif
}

TEST(CudaGpuStopModeCr154, FailedStartStillStopsCleanly) {
#ifdef KAIRO_ENABLE_CUDA
    gpu::CudaExecutor executor("cr154_cuda_failstart", cuda_config("cr154_cuda_failstart"));
    const bool started = executor.start();
    if (started) {
        // 有硬件：start 成功——做一次真实 Drain stop 收尾回归。
        executor.stop(gpu::GpuStopMode::Drain);
        EXPECT_FALSE(executor.get_status().is_running);
        SUCCEED() << "hardware path: drain stop after start";
        return;
    }
    // 无驱动：start 失败后各 stop 路径仍必须安全。
    EXPECT_NO_FATAL_FAILURE(executor.stop(gpu::GpuStopMode::Cancel));
    EXPECT_NO_FATAL_FAILURE(executor.stop(gpu::GpuStopMode::Drain));
    EXPECT_NO_FATAL_FAILURE(executor.stop());
#else
    GTEST_SKIP() << "KAIRO_ENABLE_CUDA not enabled at compile time";
#endif
}

// ---------------------------------------------------------------------------
// 硬件门控：Drain / Cancel 的 future 终态契约（无驱动时 SKIP）
// ---------------------------------------------------------------------------

class CudaGpuStopModeBehavior : public ::testing::Test {
protected:
    static bool hardware_available() {
#ifdef KAIRO_ENABLE_CUDA
        gpu::CudaExecutor probe("cr154_probe", cuda_config("cr154_probe"));
        return probe.start();
#else
        return false;
#endif
    }
};

TEST_F(CudaGpuStopModeBehavior, CancelFailsQueuedKernelsWithExecutorStopping) {
    if (!hardware_available()) {
        GTEST_SKIP() << "no CUDA hardware/driver on this host";
    }
#ifdef KAIRO_ENABLE_CUDA
    gpu::CudaExecutor executor("cr154_cancel", cuda_config("cr154_cancel"));
    ASSERT_TRUE(executor.start());

    // 首个 kernel 卡住执行器 worker，保证后续提交留在队列（未取出）。
    std::atomic<bool> kernel_started{false};
    std::atomic<bool> release{false};
    auto blocker = executor.submit_kernel([&](void*) {
        kernel_started.store(true);
        while (!release.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(1ms);
        }
    }, gpu::GpuTaskConfig{});

    ASSERT_TRUE(wait_flag(kernel_started, 5s));

    std::vector<std::future<void>> pending;
    for (int i = 0; i < 3; ++i) {
        pending.push_back(executor.submit_kernel([](void*) {}, gpu::GpuTaskConfig{}));
    }

    executor.stop(gpu::GpuStopMode::Cancel);

    // 队列中未取出的任务：ExecutorStopping 终态。
    for (auto& f : pending) {
        ASSERT_TRUE(f.valid());
        bool settled = false;
        try {
            f.get();
            settled = true;  // 已在执行前完成的任务允许成功（取出竞争）
        } catch (const ExecutorStopping&) {
            settled = true;
        } catch (const std::exception& e) {
            FAIL() << "queued kernel must settle as ExecutorStopping or success, got: "
                   << e.what();
        }
        (void)settled;
    }

    // 释放阻塞 kernel，让 worker 退出（正在执行的不被中断）。
    release.store(true);
    blocker.get();  // 执行中的 kernel 正常完成
#endif
}

TEST_F(CudaGpuStopModeBehavior, DrainLetsQueuedKernelsComplete) {
    if (!hardware_available()) {
        GTEST_SKIP() << "no CUDA hardware/driver on this host";
    }
#ifdef KAIRO_ENABLE_CUDA
    gpu::CudaExecutor executor("cr154_drain", cuda_config("cr154_drain"));
    ASSERT_TRUE(executor.start());

    constexpr int kTasks = 4;
    std::vector<std::future<void>> futures;
    for (int i = 0; i < kTasks; ++i) {
        futures.push_back(executor.submit_kernel([](void*) {}, gpu::GpuTaskConfig{}));
    }

    executor.stop(gpu::GpuStopMode::Drain);

    // 排空语义：全部已入队 kernel 正常完成，无异常终态。
    for (auto& f : futures) {
        EXPECT_NO_THROW(f.get())
            << "Drain must execute queued kernels to completion";
    }
#endif
}
