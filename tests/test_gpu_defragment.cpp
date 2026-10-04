// P-009: GpuMemoryManager::defragment() correctness tests.
//
// 背景:
//   src/executor/gpu/gpu_memory_manager.cpp 的 defragment() 实现负责合并空闲块以
//   减少碎片。原 tests/test_gpu_memory_manager.cpp 中只覆盖了非常薄弱的场景
//   (释放后 total_free 不变、空池总空闲 == pool_size),并未验证 "coalesce 真的把相邻
//   空闲块合并成单个连续区间" 这一核心正确性命题,也没有验证 defragment 不会破坏仍存活
//   指针的所属关系。
//
// 本测试文件:
//   * 完全独立(mock-based + 直接 include 实现 cpp),不依赖 CUDA toolkit 或 libexecutor
//     提供 GpuMemoryManager 符号,避免被 tests/CMakeLists.txt 中针对
//     `gpu_memory_manager` 的 EXCLUDE 过滤误伤;
//   * 文件名不匹配 `.*gpu_memory_manager.*`,会被 tests/CMakeLists.txt 自动纳入构建;
//   * 包含 3 个测试:def 合并后能装下大块、def 不破坏存活指针、def 多次调用幂等。

#include <cassert>
#include <iostream>
#include <unordered_map>

#include "kairo/gpu/gpu_memory_manager.hpp"

// 直接 include 实现以避免依赖 libexecutor 提供 GpuMemoryManager 符号
// (在没启用 CUDA 时,src/CMakeLists.txt 会把 gpu_memory_manager.cpp 排除出生产库,
// 但本测试用例只关心逻辑正确性,Mock 即可,无需任何 CUDA 头文件。)
#include "kairo/gpu/gpu_memory_manager.cpp"

using namespace kairo::gpu;

#define TEST_ASSERT(condition, message)                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::cerr << "FAILED: " << message << " at " << __FILE__ << ":"    \
                      << __LINE__ << std::endl;                                \
            return false;                                                      \
        }                                                                      \
    } while (0)

namespace {
constexpr size_t kAlignment = 256;
// CR-060: header 已提升为一个完整对齐单位（此前 sizeof(size_t)=8 导致用户
// 指针只有 8 字节对齐），此处的池布局镜像与实现保持同步。
constexpr size_t kHeader = kAlignment;
inline size_t aligned_size(size_t s) { return (s + kAlignment - 1) & ~(kAlignment - 1); }
inline size_t block_size(size_t s) { return aligned_size(s) + kHeader; }
}  // namespace

// 测试 1: 释放中间 + 末尾后,coalesce 必须把两块合成单个连续区间,
// 后续能装下一个原本放不进任何单块的尺寸 —— 这是 coalescing 行为正确的关键证据。
//
// 池布局 (pool_size = a_block + b_block + c_block,无尾随空闲):
//   [hdr+a(128K+overhead)] [hdr+b(256K+overhead)] [hdr+c(128K+overhead)] [池尾]
//   free b -> free list = [(b_start, b_block)]         (coalesce no-op)
//   free c -> free list = [(b_start, b_block), (c_start, c_block)]
//                          -> coalesce: b 在 c 之前且 b_end == c_start -> 合并成
//                          -> free list = [(b_start, b_block + c_block)]
//
// 然后 alloc BIG = aligned(b_size) + aligned(c_size) = b_block + c_block - kHeader
// (= b_block + c_block - 8),略小于合并后的 free 区间但大于任一单块。
//
// 若 coalesce 正确:free list 只有 1 项,b_end == c_start 合并后 = b_block + c_block,
// first-fit 命中且切走 BIG。
// 若 coalesce 错误:free list 是 [b, c] 两项,first-fit 看 b 不够、c 不够,
//   fallback 走 raw_alloc 路径,raw_alloc_count 会 +1。
//   本测试断言 raw_alloc_count 不增加,即可捕获 coalesce 错误。
static bool test_defragment_coalesce_then_big_alloc() {
    std::cout << "Testing GpuMemoryManager::defragment coalesce + big alloc..." << std::endl;

    size_t raw_alloc_count = 0;
    size_t raw_free_count = 0;
    GpuMemoryManager::RawAlloc raw_alloc = [&](size_t size) {
        raw_alloc_count++;
        return ::malloc(size);
    };
    GpuMemoryManager::RawFree raw_free = [&](void* ptr) {
        raw_free_count++;
        ::free(ptr);
    };

    const size_t SIZE_A = 128 * 1024;
    const size_t SIZE_B = 256 * 1024;
    const size_t SIZE_C = 128 * 1024;

    const size_t pool_size = block_size(SIZE_A) + block_size(SIZE_B) + block_size(SIZE_C);
    std::cerr << "[debug] pool_size=" << pool_size
              << " block_size_A=" << block_size(SIZE_A)
              << " block_size_B=" << block_size(SIZE_B)
              << " block_size_C=" << block_size(SIZE_C) << "\n";
    GpuMemoryManager mgr(raw_alloc, raw_free, pool_size);

    void* a = mgr.allocate(SIZE_A);
    void* b = mgr.allocate(SIZE_B);
    void* c = mgr.allocate(SIZE_C);
    TEST_ASSERT(a && b && c, "alloc a/b/c");
    TEST_ASSERT(raw_alloc_count == 1, "only one raw_alloc (the pool backing)");

    // 释放 b 和 c,触发 free() 内的 coalesce。
    mgr.free(b);
    mgr.free(c);

    // 关键 sanity:合并后的 free 区间应当正好是 b_block + c_block。
    GpuMemoryManager::MemoryStats s_after_free = mgr.get_stats();
    const size_t expected_freed = block_size(SIZE_B) + block_size(SIZE_C);
    std::cerr << "[debug] s_after_free.total_free=" << s_after_free.total_free
              << " allocation_count=" << s_after_free.allocation_count
              << " expected_freed=" << expected_freed << "\n";
    TEST_ASSERT(s_after_free.allocation_count == 1, "only a remains");
    TEST_ASSERT(s_after_free.total_allocated == SIZE_A, "total_allocated only counts a");
    TEST_ASSERT(s_after_free.total_free == expected_freed,
                "total_free == b_block + c_block (must be one merged region)");

    // 显式调用 defragment —— 应当是 no-op,但不应破坏状态。
    mgr.defragment();
    GpuMemoryManager::MemoryStats s_after_def = mgr.get_stats();
    TEST_ASSERT(s_after_def.total_free == s_after_free.total_free,
                "defragment preserves total_free");
    TEST_ASSERT(s_after_def.allocation_count == 1, "defragment preserves allocation_count");

    // 现在尝试分配一个小于 (b+c) 合并尺寸的块。
    // 注意:BIG 必须保证 aligned_size(BIG) + kHeader < free_region_size,
    //      否则 first-fit 找不到足够大的 free 区间,会走 raw_alloc 路径。
    // 这里 BIG 取 block_size(SIZE_B) - kHeader,加上 header 后绝对装得下合并区间。
    const size_t raw_alloc_before = raw_alloc_count;
    const size_t BIG = (block_size(SIZE_B) > kHeader) ? (block_size(SIZE_B) - kHeader) : block_size(SIZE_B) / 2;
    const size_t BIG_NEED = aligned_size(BIG) + kHeader;
    std::cerr << "[debug] BIG=" << BIG << " BIG_NEED=" << BIG_NEED
              << " free_region_size=" << s_after_free.total_free
              << " raw_alloc_before=" << raw_alloc_before << "\n";
    void* big = mgr.allocate(BIG);
    std::cerr << "[debug] big=" << (void*)big << " raw_alloc_count_after=" << raw_alloc_count << "\n";
    TEST_ASSERT(big != nullptr, "big alloc after defragment must succeed");
    TEST_ASSERT(raw_alloc_count == raw_alloc_before,
                "big alloc must come from merged free region, NOT raw_alloc "
                "(would mean coalesce failed)");

    // 清理
    mgr.free(big);
    mgr.free(a);
    mgr.defragment();
    GpuMemoryManager::MemoryStats s_final = mgr.get_stats();
    TEST_ASSERT(s_final.allocation_count == 0, "all freed");
    TEST_ASSERT(s_final.total_free == pool_size, "pool fully reclaimed after coalesce");
    TEST_ASSERT(raw_free_count == 0, "raw_free never called (all from pool)");

    std::cout << "  defragment coalesce + big alloc: PASSED" << std::endl;
    return true;
}

// 测试 2: defragment 不应破坏仍存活指针的元数据 / 不会误回收。
//
// 场景:分配若干块,free 一块后 defragment,然后:
//   * 第二次 free 同一指针必须 graceful(no-op,不会 raw_free 同一指针两次)
//   * 仍存活的指针在 defragment 前后仍可正常 free,行为一致
static bool test_defragment_preserves_live_pointers() {
    std::cout << "Testing GpuMemoryManager::defragment preserves live pointers..." << std::endl;

    size_t raw_free_count = 0;
    GpuMemoryManager::RawAlloc raw_alloc = [](size_t size) { return ::malloc(size); };
    GpuMemoryManager::RawFree raw_free = [&](void* ptr) {
        raw_free_count++;
        ::free(ptr);
    };

    const size_t pool_size = 64 * 1024;
    GpuMemoryManager mgr(raw_alloc, raw_free, pool_size);

    void* a = mgr.allocate(1024);
    void* b = mgr.allocate(2048);
    void* c = mgr.allocate(4096);
    TEST_ASSERT(a && b && c, "alloc a/b/c");

    // 释放 b 后 defragment —— b 已被 free() 内 coalesce 处理。
    mgr.free(b);
    mgr.defragment();

    // 第二次 free 同一指针应被 gracefully 忽略(在 block_size_map_ 中已 erase),
    // 不应再 raw_free。
    size_t raw_free_before = raw_free_count;
    mgr.free(b);
    TEST_ASSERT(raw_free_count == raw_free_before,
                "double-free of released pointer must NOT trigger raw_free");

    // a 和 c 仍存活:defragment 之后再次 free 它们(从池里分配的不会 raw_free)。
    mgr.free(a);
    mgr.free(c);
    GpuMemoryManager::MemoryStats s = mgr.get_stats();
    TEST_ASSERT(s.allocation_count == 0, "all allocations freed after defragment + later free");
    TEST_ASSERT(raw_free_count == raw_free_before,
                "freeing pool-allocated blocks must NOT trigger raw_free");

    std::cout << "  defragment preserves live pointers: PASSED" << std::endl;
    return true;
}

// 测试 3: defragment 多次调用应当幂等;空 free list 上调用不应崩溃。
static bool test_defragment_idempotent_and_empty() {
    std::cout << "Testing GpuMemoryManager::defragment idempotent + empty..." << std::endl;

    GpuMemoryManager::RawAlloc raw_alloc = [](size_t size) { return ::malloc(size); };
    GpuMemoryManager::RawFree raw_free = [](void* ptr) { ::free(ptr); };

    GpuMemoryManager mgr(raw_alloc, raw_free, 4096);

    // 全新 manager 上调用:不应崩溃。
    mgr.defragment();
    mgr.defragment();

    void* a = mgr.allocate(256);
    void* b = mgr.allocate(256);
    mgr.defragment();
    mgr.defragment();

    mgr.free(a);
    mgr.free(b);

    // 多次连续 defragment 必须幂等。
    GpuMemoryManager::MemoryStats s0 = mgr.get_stats();
    mgr.defragment();
    mgr.defragment();
    mgr.defragment();
    GpuMemoryManager::MemoryStats s1 = mgr.get_stats();
    TEST_ASSERT(s0.total_free == s1.total_free,
                "defragment idempotent on already-merged state");
    TEST_ASSERT(s1.total_free == 4096, "fully freed pool = pool_size after coalesce");

    std::cout << "  defragment idempotent + empty: PASSED" << std::endl;
    return true;
}

int main() {
    std::cout << "=== GpuMemoryManager::defragment tests (P-009) ===" << std::endl;
    bool ok = true;
    ok = test_defragment_coalesce_then_big_alloc() && ok;
    ok = test_defragment_preserves_live_pointers() && ok;
    ok = test_defragment_idempotent_and_empty() && ok;
    std::cout << (ok ? "=== All defragment tests PASSED ===" : "=== Some tests FAILED ===")
              << std::endl;
    return ok ? 0 : 1;
}