// SPDX-License-Identifier: MIT
// P-007 test: verifies CudaExecutor::validate_memory_range gates foreign and
// oversized pointers before the CUDA driver is invoked. Runs WITHOUT a CUDA
// SDK / loader present (the validation path itself never reaches the driver).

#include <gtest/gtest.h>

#include <array>
#include <mutex>
#include <string>

#include "kairo/config.hpp"
#define private public
#include "kairo/gpu/cuda_executor.hpp"
#undef private

using kairo::gpu::CudaExecutor;
using kairo::gpu::GpuExecutorConfig;

namespace {

GpuExecutorConfig make_cuda_config() {
    GpuExecutorConfig config;
    config.name = "cuda_memory_validation";
    config.backend = kairo::gpu::GpuBackend::CUDA;
    config.device_id = 0;
    config.max_queue_size = 16;
    return config;
}

}  // namespace

TEST(CudaExecutorMemoryValidationTest, ForeignPointerRejected) {
    CudaExecutor executor("cuda_memory_validation", make_cuda_config());

    std::array<unsigned char, 16> owned{};
    std::array<unsigned char, 16> foreign{};
    {
        std::lock_guard<std::mutex> lk(executor.memory_mutex_);
        executor.allocated_memory_.emplace(
            owned.data(),
            CudaExecutor::AllocationRecord{
                owned.size(), CudaExecutor::AllocationKind::Owned, true});
    }

    EXPECT_FALSE(executor.validate_memory_range(foreign.data(), foreign.size(), "dst"));
    const std::string err = executor.get_last_error();
    EXPECT_NE(err.find("not allocated by"), std::string::npos)
        << "expected 'not allocated by' in error, got: " << err;
}

TEST(CudaExecutorMemoryValidationTest, OversizedRangeRejected) {
    CudaExecutor executor("cuda_memory_validation", make_cuda_config());

    std::array<unsigned char, 16> owned{};
    {
        std::lock_guard<std::mutex> lk(executor.memory_mutex_);
        executor.allocated_memory_.emplace(
            owned.data(),
            CudaExecutor::AllocationRecord{
                owned.size(), CudaExecutor::AllocationKind::Owned, true});
    }

    EXPECT_FALSE(executor.validate_memory_range(owned.data(), owned.size() + 1, "dst"));
    const std::string err = executor.get_last_error();
    EXPECT_NE(err.find("overflow"), std::string::npos)
        << "expected 'overflow' in error, got: " << err;
}

TEST(CudaExecutorMemoryValidationTest, ExternalOptInPointerAccepted) {
    CudaExecutor executor("cuda_memory_validation", make_cuda_config());

    std::array<unsigned char, 32> external{};
    {
        std::lock_guard<std::mutex> lk(executor.memory_mutex_);
        executor.allocated_memory_.emplace(
            external.data(),
            CudaExecutor::AllocationRecord{
                external.size(), CudaExecutor::AllocationKind::ExternalOptIn, false});
    }

    // An ExternalOptIn record must pass validate_memory_range.
    EXPECT_TRUE(executor.validate_memory_range(external.data(), external.size() / 2, "dst"));
    // Range overflow is still rejected even for opted-in memory.
    EXPECT_FALSE(executor.validate_memory_range(external.data(), external.size() + 1, "dst"));
}
