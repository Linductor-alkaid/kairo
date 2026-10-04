#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

namespace kairo {
namespace gpu {

/**
 * @brief GPU 内存管理器
 *
 * 提供统一的内存管理接口，支持内存池和直接分配两种模式。
 * 使用 Raw 分配器回调（不依赖 IGpuExecutor），便于单测与集成。
 */
class GpuMemoryManager {
public:
    using RawAlloc = std::function<void*(size_t)>;
    using RawFree = std::function<void(void*)>;

    /**
     * @brief 内存使用统计
     */
    struct MemoryStats {
        size_t total_allocated = 0;   // 当前已分配字节（用户可见）
        size_t total_free = 0;        // 池内空闲字节
        size_t allocation_count = 0;   // 当前分配个数
    };

    /**
     * @brief 构造函数
     *
     * @param raw_alloc 底层分配回调（如 cudaMalloc）
     * @param raw_free  底层释放回调（如 cudaFree）
     * @param pool_size 内存池大小（字节）；0 表示不使用内存池，全部直通 raw
     */
    GpuMemoryManager(RawAlloc raw_alloc, RawFree raw_free, size_t pool_size = 0);

    ~GpuMemoryManager();

    GpuMemoryManager(const GpuMemoryManager&) = delete;
    GpuMemoryManager& operator=(const GpuMemoryManager&) = delete;
    GpuMemoryManager(GpuMemoryManager&&) = delete;
    GpuMemoryManager& operator=(GpuMemoryManager&&) = delete;

    /**
     * @brief 分配设备内存
     *
     * @param size 内存大小（字节）
     * @return 设备内存指针，失败返回 nullptr
     */
    void* allocate(size_t size);

    /**
     * @brief 释放设备内存
     *
     * @param ptr 设备内存指针（须为 allocate 返回的指针）
     */
    void free(void* ptr);

    /**
     * @brief 获取内存使用统计
     */
    MemoryStats get_stats() const;

    /**
     * @brief 合并空闲块，减少碎片（不移动已分配内存）
     */
    void defragment();

private:
    struct FreeBlock {
        void* ptr = nullptr;
        size_t size = 0;
    };

    void coalesce_free_blocks();
    size_t align_up(size_t size, size_t alignment) const;

    RawAlloc raw_alloc_;
    RawFree raw_free_;
    size_t pool_size_;
    mutable std::mutex mutex_;

    void* pool_backing_ = nullptr;   // 池后备块（仅 pool_size_ > 0 时使用）
    std::vector<FreeBlock> free_blocks_;
    std::set<void*> raw_alloced_blocks_;  // 经 raw_alloc 直接分配且未回收到池的块首地址
    // Pool overflow blocks are returned to raw_free directly and never enter free_blocks_,
    // so coalescing only operates on subranges of the single pool_backing_ allocation.
    std::set<void*> direct_alloced_blocks_;
    std::unordered_map<void*, size_t> block_size_map_;  // block_start -> size，用于 free 时合并
    size_t total_allocated_ = 0;
    size_t allocation_count_ = 0;

    static constexpr size_t kAlignment = 256;  // CUDA 设备内存常用对齐
    // CR-060: header 尺寸必须与对齐一致。此前 header = sizeof(size_t) = 8，
    // 用户指针 = 块起始 + 8，且池内逐次切割使余块起点漂移——实际只剩 8 字节
    // 对齐，float4/uint4 等 16B 对齐访存会触发 CUDA misaligned address。
    // header 取一个完整对齐单位后：need 恒为对齐单位的倍数，池内切割边界
    // 不漂移，用户指针 = 块起始 + kHeaderSize 恒满足 kAlignment。
    // 代价是每次分配最多 256 字节的头部开销（相对设备内存可接受）。
    static constexpr size_t kHeaderSize = kAlignment;
};

}  // namespace gpu
}  // namespace kairo
