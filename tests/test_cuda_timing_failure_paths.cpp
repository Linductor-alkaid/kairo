// 0.7.2 §4.2 CudaExecutor 计时失败交付路径测试（独立验证代理编写，
// 不修改库代码）。
//
// 无 CUDA 硬件/驱动的环境可执行的部分：提交侧失败路径（执行器未运行）
// 的"每提交恰好一条记录"契约。需要真实硬件的路径（async stream 回调、
// sync 交付、stop 排空）不在本文件覆盖范围内。
//
// 本文件按 `.*cuda.*` 命名约定：
//  - 常规无 CUDA 构建树（如 build/v072full）：被 tests/CMakeLists.txt 的
//    GLOB 过滤排除（CudaExecutor 未编入库，无法链接）；
//  - KAIRO_ENABLE_CUDA=ON 的 stub 头编译树：编译并运行（构造/提交路径
//    不需要驱动）；
//  - 仍按既有模式保留 GTEST_SKIP 兜底。

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#ifndef KAIRO_ENABLE_CUDA
TEST(CudaTimingFailurePaths, SkippedWithoutCudaBuild) {
    GTEST_SKIP() << "CUDA support not enabled";
}
#else

#include "kairo/gpu/cuda_executor.hpp"

using kairo::gpu::CudaExecutor;
using kairo::gpu::GpuBackend;
using kairo::gpu::GpuExecutorConfig;
using kairo::gpu::GpuTaskConfig;
using kairo::gpu::GpuKernelTimingRecord;

namespace {

GpuExecutorConfig make_cuda_config() {
    GpuExecutorConfig config;
    config.name = "cuda_timing_probe";
    config.backend = GpuBackend::CUDA;
    config.device_id = 0;
    config.max_queue_size = 16;
    config.default_stream_count = 1;
    return config;
}

}  // namespace

// CUDA 编译进库时执行器声明计时能力（与 IGpuExecutor 默认 false 相反）。
TEST(CudaTimingFailurePaths, ReportsCompletionTimingSupport) {
    GpuExecutorConfig config = make_cuda_config();
    CudaExecutor executor(config.name, config);
    EXPECT_TRUE(executor.supports_completion_timing());
}

// 提交侧失败（执行器未运行——无驱动/未 start）：未来落异常，sink 恰好
// 交付一条 success == false 记录，completion_ns 为宿主时点（非 0），
// completed_on_stream == false，stream_id 透传，kernel 未被执行。
TEST(CudaTimingFailurePaths, SubmitBeforeStartDeliversExactlyOneFailureRecord) {
    GpuExecutorConfig config = make_cuda_config();
    CudaExecutor executor(config.name, config);

    std::atomic<int> sink_calls{0};
    GpuKernelTimingRecord captured;
    bool kernel_ran = false;

    GpuTaskConfig task_config;
    task_config.stream_id = 0;
    task_config.async = true;

    auto future = executor.submit_kernel_with_timing(
        [&kernel_ran](void*) { kernel_ran = true; }, task_config,
        [&](const GpuKernelTimingRecord& record) {
            sink_calls.fetch_add(1, std::memory_order_relaxed);
            captured = record;
        });

    bool exception_observed = false;
    try {
        future.get();
    } catch (const std::exception&) {
        exception_observed = true;
    }
    EXPECT_TRUE(exception_observed)
        << "submission to a non-running executor must fail the future";
    EXPECT_FALSE(kernel_ran);

    ASSERT_EQ(sink_calls.load(), 1)
        << "exactly one timing record per submission, also on failure";
    EXPECT_FALSE(captured.success);
    EXPECT_FALSE(captured.completed_on_stream);
    EXPECT_GT(captured.completion_ns, 0)
        << "submission-side failures carry a host observation timestamp";
    EXPECT_EQ(captured.stream_id, task_config.stream_id);
}

// 失败交付不得重复计数：对同一执行器连续多次失败提交，逐条恰好一次。
TEST(CudaTimingFailurePaths, RepeatedFailedSubmissionsDeliverOneRecordEach) {
    GpuExecutorConfig config = make_cuda_config();
    CudaExecutor executor(config.name, config);

    std::atomic<int> sink_calls{0};
    for (int i = 0; i < 5; ++i) {
        GpuTaskConfig task_config;
        task_config.stream_id = 0;
        auto future = executor.submit_kernel_with_timing(
            [](void*) {}, task_config,
            [&sink_calls](const GpuKernelTimingRecord&) {
                sink_calls.fetch_add(1, std::memory_order_relaxed);
            });
        EXPECT_THROW(future.get(), std::exception);
    }
    EXPECT_EQ(sink_calls.load(), 5)
        << "each submission must deliver exactly one record";
}

#endif  // KAIRO_ENABLE_CUDA
