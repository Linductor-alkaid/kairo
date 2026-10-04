// CR-060: GpuMemoryManager returns user pointers with only 8-byte alignment
// (kAlignment=256 is applied to internal block starts, but the returned
// pointer is block_start + sizeof(size_t) = block_start + 8).
//
// Setup: raw_alloc = std::aligned_alloc(256, ...) so every raw block start is
// 256-byte aligned. Then any misalignment observed in the returned pointer is
// attributable solely to the +8 header offset inside GpuMemoryManager.
#include "kairo/gpu/gpu_memory_manager.hpp"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>
#include <cstring>

using kairo::gpu::GpuMemoryManager;

namespace {

struct RawAllocBookkeeping {
    std::map<void*, size_t> sizes;

    void* alloc(size_t size) {
        // C11 aligned_alloc requires size to be a multiple of alignment.
        size_t rounded = (size + 255) / 256 * 256;
        void* p = std::aligned_alloc(256, rounded == 0 ? 256 : rounded);
        if (p != nullptr) {
            sizes[p] = rounded;
        }
        return p;
    }

    void free(void* p) {
        auto it = sizes.find(p);
        if (it != sizes.end()) {
            sizes.erase(it);
        }
        std::free(p);
    }
};

int run_case(size_t pool_size, const char* label) {
    RawAllocBookkeeping book;
    GpuMemoryManager mm(
        [&book](size_t s) { return book.alloc(s); },
        [&book](void* p) { book.free(p); },
        pool_size);

    const size_t sizes[] = {64, 128, 256, 600, 1000};
    constexpr int kRounds = 4;  // 20 allocations total
    std::vector<std::pair<void*, size_t>> allocs;
    int bad16 = 0, bad256 = 0;

    std::printf("\n=== %s (pool_size=%zu) ===\n", label, pool_size);
    std::printf("%-4s %-8s %-14s %-8s %-8s\n", "idx", "size", "ptr", "%16", "%256");
    int idx = 0;
    for (int r = 0; r < kRounds; ++r) {
        for (size_t sz : sizes) {
            void* p = mm.allocate(sz);
            if (p == nullptr) {
                std::printf("allocate(%zu) returned nullptr\n", sz);
                return 1;
            }
            const uintptr_t v = reinterpret_cast<uintptr_t>(p);
            std::printf("%-4d %-8zu %-14p %-8zu %-8zu\n",
                        idx, sz, p, v % 16, v % 256);
            if (v % 16 != 0) ++bad16;
            if (v % 256 != 0) ++bad256;
            allocs.emplace_back(p, sz);
            ++idx;
        }
    }

    std::printf("SUMMARY %s: %d allocations, misaligned-mod-16: %d, "
                "not-256-aligned: %d\n", label, idx, bad16, bad256);

    for (auto& [p, sz] : allocs) {
        mm.free(p);
    }
    return (bad16 > 0) ? 1 : 0;
}

}  // namespace

int main() {
    std::printf("kHeaderSize=sizeof(size_t)=%zu, kAlignment=256 (header claims 256)\n",
                sizeof(size_t));
    int pool_rc = run_case(1u << 20, "POOL 1MB");
    int direct_rc = run_case(0, "DIRECT (pool_size=0)");
    std::printf("\nVERDICT: %s\n",
                (pool_rc || direct_rc)
                    ? "CONFIRMED - user pointers not 16-byte aligned (only 8)"
                    : "NOT REPRODUCED");
    return (pool_rc || direct_rc) ? 0 : 1;  // exit 0 when the defect reproduces
}
