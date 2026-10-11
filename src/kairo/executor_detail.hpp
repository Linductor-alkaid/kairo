#pragma once

// Executor facade 各编译单元共享的内部辅助（原 executor.cpp 匿名命名空间，
// v0.7.0 拆分 executor.cpp 时提取；inline 保持外部无符号泄漏）。

#include "kairo/executor.hpp"

namespace kairo {
namespace detail {

inline ExecutorResult make_failure(ExecutorErrorCode code, const std::string& message) {
    return ExecutorResult::failure(code, message);
}

inline ExecutorResult validate_executor_config(const ExecutorConfig& config) {
    if (config.min_threads != 0 && config.max_threads != 0 &&
        config.min_threads > config.max_threads) {
        return make_failure(
            ExecutorErrorCode::InvalidConfig,
            "ExecutorConfig invalid: min_threads must be <= max_threads");
    }
    return ExecutorResult::success();
}

inline ExecutorResult validate_realtime_config(const std::string& name,
                                        const RealtimeThreadConfig& config) {
    if (name.empty()) {
        return make_failure(
            ExecutorErrorCode::InvalidConfig,
            "Realtime executor name must not be empty");
    }
    if (config.thread_name.empty()) {
        return make_failure(
            ExecutorErrorCode::InvalidConfig,
            "RealtimeThreadConfig invalid: thread_name must not be empty");
    }
    if (config.cycle_period_ns <= 0) {
        return make_failure(
            ExecutorErrorCode::InvalidConfig,
            "RealtimeThreadConfig invalid: cycle_period_ns must be greater than 0");
    }
    return ExecutorResult::success();
}

inline ExecutorResult validate_blocking_io_config(const std::string& name,
                                           const BlockingIoConfig& config,
                                           const IBlockingIoWorker* worker) {
    if (name.empty()) {
        return make_failure(ExecutorErrorCode::InvalidConfig,
                            "Blocking I/O executor name must not be empty");
    }
    if (config.thread_name.empty()) {
        return make_failure(ExecutorErrorCode::InvalidConfig,
                            "BlockingIoConfig invalid: thread_name must not be empty");
    }
    if (config.startup_timeout.count() < 0) {
        return make_failure(ExecutorErrorCode::InvalidConfig,
                            "BlockingIoConfig invalid: startup_timeout must not be negative");
    }
    if (!worker) {
        return make_failure(ExecutorErrorCode::InvalidConfig,
                            "Blocking I/O worker must not be null");
    }
    return ExecutorResult::success();
}

inline ExecutorResult validate_gpu_config_for_facade(
    const std::string& name,
    const gpu::GpuExecutorConfig& config) {
    if (name.empty()) {
        return make_failure(
            ExecutorErrorCode::InvalidConfig,
            "GPU executor name must not be empty");
    }
    if (config.name.empty()) {
        return make_failure(
            ExecutorErrorCode::InvalidConfig,
            "GpuExecutorConfig invalid: config.name must not be empty");
    }
    if (config.max_queue_size == 0) {
        return make_failure(
            ExecutorErrorCode::InvalidConfig,
            "GpuExecutorConfig invalid: max_queue_size must be greater than 0");
    }
    if (config.device_id < 0) {
        return make_failure(
            ExecutorErrorCode::InvalidConfig,
            "GpuExecutorConfig invalid: device_id must be non-negative");
    }
    if (config.default_stream_count < 1) {
        return make_failure(
            ExecutorErrorCode::InvalidConfig,
            "GpuExecutorConfig invalid: default_stream_count must be at least 1");
    }
    return ExecutorResult::success();
}

inline ExecutorResult check_gpu_backend_available(const gpu::GpuExecutorConfig& config) {
#ifndef KAIRO_ENABLE_GPU
    (void)config;
    return make_failure(
        ExecutorErrorCode::BackendUnavailable,
        "GPU support is not enabled in this build");
#else
    switch (config.backend) {
    case gpu::GpuBackend::CUDA:
#ifndef KAIRO_ENABLE_CUDA
        return make_failure(
            ExecutorErrorCode::BackendUnavailable,
            "CUDA backend is not enabled in this build");
#else
        return ExecutorResult::success();
#endif
    case gpu::GpuBackend::OPENCL:
#ifndef KAIRO_ENABLE_OPENCL
        return make_failure(
            ExecutorErrorCode::BackendUnavailable,
            "OpenCL backend is not enabled in this build");
#else
        return ExecutorResult::success();
#endif
    case gpu::GpuBackend::SYCL:
        return make_failure(
            ExecutorErrorCode::BackendUnavailable,
            "SYCL backend is not implemented in this build");
    case gpu::GpuBackend::HIP:
        return make_failure(
            ExecutorErrorCode::BackendUnavailable,
            "HIP backend is not implemented in this build");
    default:
        return make_failure(
            ExecutorErrorCode::BackendUnavailable,
            "Requested GPU backend is unavailable");
    }
#endif
}

}  // namespace detail
}  // namespace kairo
