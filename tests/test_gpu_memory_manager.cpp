#include <cassert>
#include <chrono>
#include <iostream>
#include <limits>
#include <unordered_map>
#include <vector>

#include <kairo/config.hpp>
#include <kairo/types.hpp>
#include "kairo/gpu/gpu_memory_manager.hpp"

using namespace kairo;
using namespace kairo::gpu;

#define TEST_ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            std::cerr << "FAILED: " << message << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
            return false; \
        } \
    } while(0)

static bool test_pool_disabled_passthrough() {
    std::cout << "Testing GpuMemoryManager with pool_size=0 (passthrough)..." << std::endl;
    size_t alloc_count = 0;
    size_t free_count = 0;
    std::unordered_map<void*, size_t> mock_alloced;

    GpuMemoryManager::RawAlloc raw_alloc = [&](size_t size) {
        alloc_count++;
        void* p = ::malloc(size);
        if (p) mock_alloced[p] = size;
        return p;
    };
    GpuMemoryManager::RawFree raw_free = [&](void* ptr) {
        free_count++;
        if (mock_alloced.erase(ptr) == 0) {
            std::cerr << "free unknown ptr" << std::endl;
        }
        ::free(ptr);
    };

    GpuMemoryManager mgr(raw_alloc, raw_free, 0);

    void* p1 = mgr.allocate(100);
    TEST_ASSERT(p1 != nullptr, "allocate 100");
    TEST_ASSERT(alloc_count == 1, "one raw_alloc for first allocate");

    void* p2 = mgr.allocate(200);
    TEST_ASSERT(p2 != nullptr, "allocate 200");
    TEST_ASSERT(alloc_count == 2, "two raw_allocs");

    auto stats = mgr.get_stats();
    TEST_ASSERT(stats.allocation_count == 2, "two allocations");
    TEST_ASSERT(stats.total_allocated >= 100 + 200, "total allocated");

    mgr.free(p1);
    TEST_ASSERT(free_count == 1, "one raw_free after first free");
    mgr.free(p2);
    TEST_ASSERT(free_count == 2, "two raw_frees");

    stats = mgr.get_stats();
    TEST_ASSERT(stats.allocation_count == 0, "no allocations after all freed");

    std::cout << "  pool_size=0 passthrough: PASSED" << std::endl;
    return true;
}

static bool test_pool_enabled_from_pool() {
    std::cout << "Testing GpuMemoryManager with pool (alloc from pool)..." << std::endl;
    size_t raw_alloc_count = 0;
    size_t raw_free_count = 0;
    std::vector<void*> raw_ptrs;

    GpuMemoryManager::RawAlloc raw_alloc = [&](size_t size) {
        raw_alloc_count++;
        void* p = ::malloc(size);
        if (p) raw_ptrs.push_back(p);
        return p;
    };
    GpuMemoryManager::RawFree raw_free = [&](void* ptr) {
        raw_free_count++;
        ::free(ptr);
    };

    const size_t pool_size = 4096;
    GpuMemoryManager mgr(raw_alloc, raw_free, pool_size);

    void* p1 = mgr.allocate(256);
    TEST_ASSERT(p1 != nullptr, "first allocate");
    TEST_ASSERT(raw_alloc_count == 1, "one raw_alloc for pool backing");

    void* p2 = mgr.allocate(512);
    TEST_ASSERT(p2 != nullptr, "second allocate");
    TEST_ASSERT(raw_alloc_count == 1, "still one raw_alloc (from pool)");

    void* p3 = mgr.allocate(300);
    TEST_ASSERT(p3 != nullptr, "third allocate");
    TEST_ASSERT(raw_alloc_count == 1, "still one raw_alloc");

    auto stats = mgr.get_stats();
    TEST_ASSERT(stats.allocation_count == 3, "three allocations");

    mgr.free(p2);
    mgr.free(p1);
    mgr.free(p3);

    stats = mgr.get_stats();
    TEST_ASSERT(stats.allocation_count == 0, "all freed");
    TEST_ASSERT(raw_free_count == 0, "no raw_free yet (pool still holds backing)");

    void* p4 = mgr.allocate(128);
    TEST_ASSERT(p4 != nullptr, "allocate after all freed");
    TEST_ASSERT(raw_alloc_count == 1, "still one raw_alloc (reuse pool)");

    mgr.free(p4);
    std::cout << "  pool alloc/free: PASSED" << std::endl;
    return true;
}

static bool test_pool_overflow_raw() {
    std::cout << "Testing GpuMemoryManager pool overflow (raw alloc)..." << std::endl;
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

    const size_t pool_size = 1024;
    GpuMemoryManager mgr(raw_alloc, raw_free, pool_size);

    void* small = mgr.allocate(64);
    TEST_ASSERT(small != nullptr, "small alloc");
    TEST_ASSERT(raw_alloc_count == 1, "pool backing");

    void* huge = mgr.allocate(2048);
    TEST_ASSERT(huge != nullptr, "huge alloc");
    TEST_ASSERT(raw_alloc_count == 2, "overflow raw_alloc");

    mgr.free(huge);
    TEST_ASSERT(raw_free_count == 1, "free overflow block");
    mgr.free(small);
    TEST_ASSERT(raw_free_count == 1, "pool block not raw_freed yet");

    std::cout << "  pool overflow: PASSED" << std::endl;
    return true;
}

static bool test_huge_allocation_overflow_returns_null() {
    std::cout << "Testing GpuMemoryManager huge allocation overflow returns null..." << std::endl;

    const size_t max_size = std::numeric_limits<size_t>::max();
    const size_t overflow_values[] = {max_size, max_size - 128};
    const size_t pool_sizes[] = {0, 4096};

    for (size_t pool_size : pool_sizes) {
        for (size_t huge_size : overflow_values) {
            size_t raw_alloc_count = 0;
            size_t raw_free_count = 0;

            GpuMemoryManager::RawAlloc raw_alloc = [&](size_t) {
                raw_alloc_count++;
                return reinterpret_cast<void*>(0x1000);
            };
            GpuMemoryManager::RawFree raw_free = [&](void*) {
                raw_free_count++;
            };

            GpuMemoryManager mgr(raw_alloc, raw_free, pool_size);

            void* p = mgr.allocate(huge_size);
            TEST_ASSERT(p == nullptr, "huge allocation returns nullptr");
            TEST_ASSERT(raw_alloc_count == 0, "huge allocation does not call raw_alloc");
            TEST_ASSERT(raw_free_count == 0, "huge allocation does not call raw_free");

            auto stats = mgr.get_stats();
            TEST_ASSERT(stats.allocation_count == 0, "huge allocation does not update count");
            TEST_ASSERT(stats.total_allocated == 0, "huge allocation does not update total");
        }
    }

    const size_t boundary_size = max_size - 1024;
    const size_t boundary_need = ((boundary_size + 255) & ~static_cast<size_t>(255)) + sizeof(size_t);
    for (size_t pool_size : pool_sizes) {
        alignas(256) unsigned char pool_storage[4096];
        std::vector<size_t> raw_alloc_sizes;
        size_t raw_free_count = 0;

        GpuMemoryManager::RawAlloc raw_alloc = [&](size_t size) {
            raw_alloc_sizes.push_back(size);
            if (pool_size != 0 && size == pool_size) {
                return static_cast<void*>(pool_storage);
            }
            return static_cast<void*>(nullptr);
        };
        GpuMemoryManager::RawFree raw_free = [&](void*) {
            raw_free_count++;
        };

        {
            GpuMemoryManager mgr(raw_alloc, raw_free, pool_size);

            void* p = mgr.allocate(boundary_size);
            TEST_ASSERT(p == nullptr, "SIZE_MAX - 1024 allocation returns nullptr when raw_alloc fails");

            auto stats = mgr.get_stats();
            TEST_ASSERT(stats.allocation_count == 0, "boundary allocation does not update count");
            TEST_ASSERT(stats.total_allocated == 0, "boundary allocation does not update total");
        }

        if (pool_size == 0) {
            TEST_ASSERT(raw_alloc_sizes.size() == 1, "passthrough boundary calls raw_alloc once");
            TEST_ASSERT(raw_alloc_sizes[0] == boundary_need, "passthrough boundary raw size does not wrap");
            TEST_ASSERT(raw_free_count == 0, "passthrough failed boundary allocation does not raw_free");
        } else {
            TEST_ASSERT(raw_alloc_sizes.size() == 2, "pool boundary calls raw_alloc for pool and overflow");
            TEST_ASSERT(raw_alloc_sizes[0] == pool_size, "pool boundary initializes pool");
            TEST_ASSERT(raw_alloc_sizes[1] == boundary_need, "pool boundary raw size does not wrap");
            TEST_ASSERT(raw_free_count == 1, "pool boundary frees pool backing in destructor");
        }
    }

    std::cout << "  huge allocation overflow: PASSED" << std::endl;
    return true;
}

static bool test_defragment() {
    std::cout << "Testing GpuMemoryManager defragment..." << std::endl;
    GpuMemoryManager::RawAlloc raw_alloc = [](size_t size) { return ::malloc(size); };
    GpuMemoryManager::RawFree raw_free = [](void* ptr) { ::free(ptr); };

    const size_t pool_size = 8192;
    GpuMemoryManager mgr(raw_alloc, raw_free, pool_size);

    void* p1 = mgr.allocate(256);
    void* p2 = mgr.allocate(256);
    void* p3 = mgr.allocate(256);
    TEST_ASSERT(p1 && p2 && p3, "three allocs");

    mgr.free(p2);
    auto stats_before = mgr.get_stats();
    mgr.defragment();
    auto stats_after = mgr.get_stats();
    TEST_ASSERT(stats_after.total_free == stats_before.total_free, "total_free unchanged by defragment");
    TEST_ASSERT(stats_after.allocation_count == 2, "two allocations remain");

    mgr.free(p1);
    mgr.free(p3);
    mgr.defragment();
    stats_after = mgr.get_stats();
    TEST_ASSERT(stats_after.allocation_count == 0, "all freed");
    TEST_ASSERT(stats_after.total_free == pool_size, "all pool free after coalesce");

    std::cout << "  defragment: PASSED" << std::endl;
    return true;
}

static bool test_get_stats() {
    std::cout << "Testing GpuMemoryManager get_stats..." << std::endl;
    GpuMemoryManager::RawAlloc raw_alloc = [](size_t size) { return ::malloc(size); };
    GpuMemoryManager::RawFree raw_free = [](void* ptr) { ::free(ptr); };

    GpuMemoryManager mgr(raw_alloc, raw_free, 4096);
    auto s0 = mgr.get_stats();
    TEST_ASSERT(s0.allocation_count == 0 && s0.total_allocated == 0, "initial stats");

    void* p = mgr.allocate(100);
    TEST_ASSERT(p != nullptr, "alloc");
    auto s1 = mgr.get_stats();
    TEST_ASSERT(s1.allocation_count == 1, "count 1");
    TEST_ASSERT(s1.total_allocated >= 100, "total_allocated >= 100");
    TEST_ASSERT(s1.total_free >= 0, "total_free");

    mgr.free(p);
    auto s2 = mgr.get_stats();
    TEST_ASSERT(s2.allocation_count == 0, "count 0 after free");
    TEST_ASSERT(s2.total_allocated == 0, "total_allocated 0");

    std::cout << "  get_stats: PASSED" << std::endl;
    return true;
}

static bool test_performance_comparison() {
    std::cout << "Performance comparison: direct vs memory pool..." << std::endl;

    const size_t num_rounds = 1000;
    const size_t block_size = 512;
    const size_t pool_size = 1024 * 1024;

    size_t direct_alloc_count = 0;
    size_t direct_free_count = 0;
    GpuMemoryManager::RawAlloc raw_alloc_direct = [&](size_t size) {
        direct_alloc_count++;
        return ::malloc(size);
    };
    GpuMemoryManager::RawFree raw_free_direct = [&](void* ptr) {
        direct_free_count++;
        ::free(ptr);
    };

    GpuMemoryManager mgr_direct(raw_alloc_direct, raw_free_direct, 0);
    auto t0 = std::chrono::steady_clock::now();
    for (size_t r = 0; r < num_rounds; ++r) {
        std::vector<void*> ptrs;
        for (int i = 0; i < 50; ++i) {
            void* p = mgr_direct.allocate(block_size);
            if (p) ptrs.push_back(p);
        }
        for (void* p : ptrs) {
            mgr_direct.free(p);
        }
    }
    auto t1 = std::chrono::steady_clock::now();
    auto direct_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

    size_t pool_alloc_count = 0;
    size_t pool_free_count = 0;
    GpuMemoryManager::RawAlloc raw_alloc_pool = [&](size_t size) {
        pool_alloc_count++;
        return ::malloc(size);
    };
    GpuMemoryManager::RawFree raw_free_pool = [&](void* ptr) {
        pool_free_count++;
        ::free(ptr);
    };

    GpuMemoryManager mgr_pool(raw_alloc_pool, raw_free_pool, pool_size);
    auto t2 = std::chrono::steady_clock::now();
    for (size_t r = 0; r < num_rounds; ++r) {
        std::vector<void*> ptrs;
        for (int i = 0; i < 50; ++i) {
            void* p = mgr_pool.allocate(block_size);
            if (p) ptrs.push_back(p);
        }
        for (void* p : ptrs) {
            mgr_pool.free(p);
        }
    }
    auto t3 = std::chrono::steady_clock::now();
    auto pool_us = std::chrono::duration_cast<std::chrono::microseconds>(t3 - t2).count();

    std::cout << "  Rounds: " << num_rounds << " x 50 alloc+free per round, block_size=" << block_size << std::endl;
    std::cout << "  Direct (pool_size=0): raw_alloc=" << direct_alloc_count << ", raw_free=" << direct_free_count
              << ", time=" << direct_us << " us" << std::endl;
    std::cout << "  Pool (pool_size=" << pool_size << "): raw_alloc=" << pool_alloc_count << ", raw_free=" << pool_free_count
              << ", time=" << pool_us << " us" << std::endl;
    if (direct_alloc_count > 0) {
        double pct = 100.0 * (1.0 - static_cast<double>(pool_alloc_count) / static_cast<double>(direct_alloc_count));
        std::cout << "  Pool reduces raw_alloc calls by " << (direct_alloc_count - pool_alloc_count) << " ("
                  << static_cast<int>(pct) << "% fewer)" << std::endl;
    }
    std::cout << "  Performance comparison: PASSED" << std::endl;
    return true;
}

int main() {
    std::cout << "=== GpuMemoryManager unit tests ===" << std::endl;
    bool ok = true;
    ok = test_pool_disabled_passthrough() && ok;
    ok = test_pool_enabled_from_pool() && ok;
    ok = test_pool_overflow_raw() && ok;
    ok = test_huge_allocation_overflow_returns_null() && ok;
    ok = test_defragment() && ok;
    ok = test_get_stats() && ok;
    ok = test_performance_comparison() && ok;
    std::cout << (ok ? "=== All GpuMemoryManager tests PASSED ===" : "=== Some tests FAILED ===") << std::endl;
    return ok ? 0 : 1;
}
