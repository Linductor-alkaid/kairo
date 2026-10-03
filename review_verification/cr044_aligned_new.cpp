// CR-044 verification: the realtime allocation guard's operator new
// replacement (src/executor/comm/realtime_memory.cpp, guarded by
// KAIRO_ENABLE_REALTIME_ALLOCATION_GUARD) covers only the plain
// operator new / new[] and NOT the over-aligned C++17 variants
// operator new(size_t, align_val_t) / operator new[](size_t, align_val_t).
//
// Consequence: any allocation of a type with alignment > 16 (e.g. cache-line
// padded payloads) performed while a RealtimeAllocationGuard is active is
// invisible to the guard: no allocation_count, no bytes, no Abort.
//
// The shipped build/src/libexecutor.a is built with the guard OFF
// (CMakeCache: KAIRO_ENABLE_REALTIME_ALLOCATION_GUARD:BOOL=OFF), so this
// test compiles the library's own realtime_memory.cpp with the official
// option macro enabled (no source modification) and links it ahead of the
// archive. Run modes:
//   cr044_aligned_new probe    - counting checks (default)
//   cr044_aligned_new abort    - Abort policy + over-aligned allocation;
//                                prints SURVIVED if the guard missed it
//   cr044_aligned_new default  - link layout probe against stock libexecutor.a
#include <kairo/comm/realtime_memory.hpp>

#include <cstdio>
#include <memory>
#include <new>

using kairo::comm::RealtimeAllocationGuard;
using kairo::comm::RealtimeAllocationViolationPolicy;

namespace {

struct Plain {
    char data[32];
};

struct alignas(64) OverAligned {
    char data[128];
};

// Prevent C++14 allocation elision: the freshly allocated pointer must escape.
volatile void* g_sink;

}  // namespace

int run_probe() {
    std::printf("__STDCPP_DEFAULT_NEW_ALIGNMENT__=%d\n",
                static_cast<int>(__STDCPP_DEFAULT_NEW_ALIGNMENT__));
    std::printf("guard is_enabled=%d\n", RealtimeAllocationGuard::is_enabled() ? 1 : 0);
    if (!RealtimeAllocationGuard::is_enabled()) {
        std::printf("guard not compiled in; probe meaningless\n");
        return 2;
    }

    // Control: plain allocation IS counted.
    RealtimeAllocationGuard::reset_current_thread_stats();
    {
        RealtimeAllocationGuard guard("cr044", "control");
        auto p = std::make_unique<Plain>();
        std::printf("control addr=%p\n", (void*)p.get());  // defeats new-elision
        g_sink = p.get();
        const auto stats = RealtimeAllocationGuard::current_thread_stats();
        std::printf("control plain make_unique(32B, align 8): count=%llu bytes=%llu\n",
                    (unsigned long long)stats.allocation_count,
                    (unsigned long long)stats.allocated_bytes);
    }

    // Bug: over-aligned allocation is NOT counted.
    RealtimeAllocationGuard::reset_current_thread_stats();
    bool aligned_counted = false;
    {
        RealtimeAllocationGuard guard("cr044", "aligned-object");
        auto p = std::make_unique<OverAligned>();
        g_sink = p.get();
        const auto stats = RealtimeAllocationGuard::current_thread_stats();
        aligned_counted = stats.allocation_count > 0;
        std::printf("over-aligned make_unique(128B, alignas(64)): count=%llu bytes=%llu\n",
                    (unsigned long long)stats.allocation_count,
                    (unsigned long long)stats.allocated_bytes);
    }

    // Same for the aligned array variant.
    RealtimeAllocationGuard::reset_current_thread_stats();
    {
        RealtimeAllocationGuard guard("cr044", "aligned-array");
        auto arr = std::make_unique<OverAligned[]>(4);
        g_sink = arr.get();
        const auto stats = RealtimeAllocationGuard::current_thread_stats();
        std::printf("over-aligned new[](4x128B, alignas(64)): count=%llu bytes=%llu\n",
                    (unsigned long long)stats.allocation_count,
                    (unsigned long long)stats.allocated_bytes);
    }

    std::printf("verdict: %s\n",
                aligned_counted
                    ? "NOT REPRODUCED - aligned allocations are counted"
                    : "CONFIRMED - over-aligned allocations bypass the guard");
    return aligned_counted ? 1 : 0;
}

int run_abort_probe() {
    if (!RealtimeAllocationGuard::is_enabled()) return 2;
    RealtimeAllocationGuard::reset_current_thread_stats();
    RealtimeAllocationGuard guard(
        "cr044", "abort-probe", RealtimeAllocationViolationPolicy::Abort);
    auto p = std::make_unique<OverAligned>();  // would abort() if guarded
    g_sink = p.get();
    const auto stats = RealtimeAllocationGuard::current_thread_stats();
    std::printf("SURVIVED Abort policy with over-aligned allocation: count=%llu\n",
                (unsigned long long)stats.allocation_count);
    std::printf("verdict: CONFIRMED - Abort policy cannot fire for over-aligned allocations\n");
    return 0;
}

int run_default_build_probe() {
    // Stock archive build: guard compiled out entirely.
    std::printf("stock libexecutor.a guard is_enabled=%d\n",
                RealtimeAllocationGuard::is_enabled() ? 1 : 0);
    return 0;
}

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "probe";
    if (mode == "abort") return run_abort_probe();
    if (mode == "default") return run_default_build_probe();
    return run_probe();
}
