#include <cstdlib>
#include <vector>

#include <gtest/gtest.h>

#include "kairo/gpu/gpu_memory_manager.hpp"

// Keep this test independent of CUDA availability: it exercises allocator bookkeeping with
// host callbacks and includes the implementation when the production GPU source is excluded.
#include "kairo/gpu/gpu_memory_manager.cpp"

namespace kairo::gpu {
namespace {

TEST(GpuMemoryManagerTest, DirectOverflowBlocksAreNotCoalesced) {
    constexpr size_t kPoolSize = 1024;
    size_t raw_alloc_count = 0;
    size_t raw_free_count = 0;

    GpuMemoryManager::RawAlloc raw_alloc = [&](size_t size) {
        ++raw_alloc_count;
        return std::malloc(size);
    };
    GpuMemoryManager::RawFree raw_free = [&](void* ptr) {
        ++raw_free_count;
        std::free(ptr);
    };

    GpuMemoryManager manager(raw_alloc, raw_free, kPoolSize);
    void* pooled_first = manager.allocate(128);
    void* pooled_second = manager.allocate(128);
    ASSERT_NE(pooled_first, nullptr);
    ASSERT_NE(pooled_second, nullptr);

    std::vector<void*> overflow_blocks;
    for (int index = 0; index < 3; ++index) {
        void* block = manager.allocate(kPoolSize);
        ASSERT_NE(block, nullptr);
        overflow_blocks.push_back(block);
    }
    EXPECT_EQ(raw_alloc_count, 4U);

    manager.free(overflow_blocks[1]);
    manager.free(pooled_first);
    manager.defragment();
    manager.free(overflow_blocks[0]);
    manager.free(pooled_second);
    manager.defragment();
    manager.free(overflow_blocks[2]);

    EXPECT_EQ(raw_free_count, 3U);
    const auto stats = manager.get_stats();
    EXPECT_EQ(stats.allocation_count, 0U);
    EXPECT_EQ(stats.total_allocated, 0U);
    EXPECT_EQ(stats.total_free, kPoolSize);

    void* reused_pool_block = manager.allocate(512);
    ASSERT_NE(reused_pool_block, nullptr);
    EXPECT_EQ(raw_alloc_count, 4U);
    manager.free(reused_pool_block);
}

}  // namespace
}  // namespace kairo::gpu
