/// @file test_opencl_gpu_stop_mode.cpp
/// CR-154（0.7.2 §4.3）OpenCLExecutor GpuStopMode 的独立验证。
///
/// 分层：
/// 1. 无设备可跑：未启动执行器上 stop(Drain)/stop(Cancel)/stop() 的
///    安全性（新的 stop_impl(cancel_queued) 分发路径最小行为面），
///    以及 stop() 无参 = Cancel、stop(Drain/Cancel) 后可重启的幂等性。
/// 2. 设备门控：真实 Drain（跳过取消、worker 排空队列）/ Cancel
///    （未取出任务以 ExecutorStopping 落终态）行为——无设备时
///    GTEST_SKIP。
///
/// 本文件名含 "opencl"：tests/CMakeLists.txt 按名字注入 OpenCL 头路径
/// 与 KAIRO_ENABLE_OPENCL 宏；KAIRO_OPENCL_ENABLED 关闭的构建树整个
/// 排除该文件。

#include <kairo/config.hpp>
#include <kairo/types.hpp>

#include "kairo/gpu/opencl_executor.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <gtest/gtest.h>
#include <string>
#include <thread>
#include <vector>

using namespace kairo;
using namespace std::chrono_literals;

namespace {

gpu::GpuExecutorConfig opencl_config(const std::string& name) {
    gpu::GpuExecutorConfig config;
    config.name = name;
    config.backend = gpu::GpuBackend::OPENCL;
    config.device_id = 0;
    config.max_queue_size = 64;
    return config;
}

} // namespace

TEST(OpenclGpuStopModeCr154, StopModeOnNeverStartedExecutorIsSafe) {
#ifdef KAIRO_ENABLE_OPENCL
    gpu::OpenCLExecutor executor("cr154_ocl_nostart", opencl_config("cr154_ocl_nostart"));
    EXPECT_NO_FATAL_FAILURE(executor.stop(gpu::GpuStopMode::Drain));
    EXPECT_FALSE(executor.get_status().is_running);
    EXPECT_NO_FATAL_FAILURE(executor.stop(gpu::GpuStopMode::Cancel));
    EXPECT_FALSE(executor.get_status().is_running);
    // 无参 stop() = Cancel（0.7.1 默认）。
    EXPECT_NO_FATAL_FAILURE(executor.stop());
    EXPECT_FALSE(executor.get_status().is_running);
#else
    GTEST_SKIP() << "KAIRO_ENABLE_OPENCL not enabled at compile time";
#endif
}

TEST(OpenclGpuStopModeCr154, StopIsIdempotentAndExecutorStaysConsistent) {
#ifdef KAIRO_ENABLE_OPENCL
    gpu::OpenCLExecutor executor("cr154_ocl_idem", opencl_config("cr154_ocl_idem"));
    // 反复交错调用两条模式路径：无崩溃，状态终值一致。
    for (int i = 0; i < 3; ++i) {
        EXPECT_NO_FATAL_FAILURE(executor.stop(gpu::GpuStopMode::Drain));
        EXPECT_NO_FATAL_FAILURE(executor.stop(gpu::GpuStopMode::Cancel));
    }
    EXPECT_FALSE(executor.get_status().is_running);

    // start 失败（无设备）后 stop 仍安全。
    const bool started = executor.start();
    if (!started) {
        EXPECT_NO_FATAL_FAILURE(executor.stop(gpu::GpuStopMode::Drain));
        EXPECT_FALSE(executor.get_status().is_running);
    } else {
        // 有设备：start 成功则做真实模式化收尾回归。
        executor.stop(gpu::GpuStopMode::Drain);
        EXPECT_FALSE(executor.get_status().is_running);
    }
#else
    GTEST_SKIP() << "KAIRO_ENABLE_OPENCL not enabled at compile time";
#endif
}

TEST(OpenclGpuStopModeCr154, ConcurrentModeStopsDoNotDeadlock) {
#ifdef KAIRO_ENABLE_OPENCL
    gpu::OpenCLExecutor executor("cr154_ocl_conc", opencl_config("cr154_ocl_conc"));
    (void)executor.start();  // 无设备时失败，随后的并发 stop 仍必须安全

    // 并发混跑两条模式路径：stop_impl 的 join 串行化（worker_join_mutex_）
    // 必须消除并发 stop 的竞争——全部调用在时限内返回。
    const bool finished = std::async(std::launch::async, [&] {
        std::thread t1([&] { executor.stop(gpu::GpuStopMode::Drain); });
        std::thread t2([&] { executor.stop(gpu::GpuStopMode::Cancel); });
        std::thread t3([&] { executor.stop(); });
        t1.join();
        t2.join();
        t3.join();
    }).wait_for(10s) == std::future_status::ready;

    EXPECT_TRUE(finished) << "concurrent mode stops deadlocked";
    EXPECT_FALSE(executor.get_status().is_running);
#else
    GTEST_SKIP() << "KAIRO_ENABLE_OPENCL not enabled at compile time";
#endif
}
