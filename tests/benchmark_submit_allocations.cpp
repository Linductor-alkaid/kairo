/**
 * Submit-path allocation-count benchmark (v0.7.0 M0, CR-107).
 *
 * Counts heap allocations per task across the facade submit paths by
 * replacing global operator new. This is a baseline-capture benchmark for
 * CR-107 ("6-10 heap allocations per task, some avoidable"): the M0 goal is
 * to shrink the baseline before feedback wrapping adds more, so future
 * regressions (or improvements) are detectable as exact allocation counts,
 * not noisy timings.
 *
 * Measures submit+run lifecycle (submit -> worker executes -> future ready),
 * single producer, so the counts are stable integers per task:
 *   a) submit() void task, no args;
 *   b) submit() returning a value;
 *   c) submit() with runtime args (bind path);
 *   d) submit_with_handle() tracked, no args;
 *   e) submit_with_handle() tracked with runtime args.
 *
 * Config: env KAIRO_BENCHMARK_TASKS (default 20000 per phase).
 * Output: human-readable text (default) or JSON lines (--json).
 */

#include <kairo/executor.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

#if defined(_WIN32)
#include <malloc.h>  // _aligned_malloc/_aligned_free（MSVC 与 MinGW-w64）
#endif

using namespace kairo;

namespace {

std::atomic<size_t> g_alloc_count{0};
std::atomic<size_t> g_alloc_bytes{0};
std::atomic<bool> g_counting{false};

// 对齐分配的平台分派：POSIX 用 posix_memalign（free 释放）；Windows 用
// _aligned_malloc（必须配 _aligned_free）。两者必须与对应的
// aligned_deallocate 配对，不与 std::free 混用。
void* aligned_allocate(std::size_t size, std::size_t alignment) {
#if defined(_WIN32)
    return _aligned_malloc(size, alignment);
#else
    void* p = nullptr;
    if (posix_memalign(&p, alignment, size) != 0) p = nullptr;
    return p;
#endif
}

void aligned_deallocate(void* p) noexcept {
#if defined(_WIN32)
    _aligned_free(p);
#else
    std::free(p);
#endif
}

void* counted_allocate(std::size_t size, std::size_t alignment) {
    if (g_counting.load(std::memory_order_relaxed)) {
        g_alloc_count.fetch_add(1, std::memory_order_relaxed);
        g_alloc_bytes.fetch_add(size, std::memory_order_relaxed);
    }
    if (alignment <= __STDCPP_DEFAULT_NEW_ALIGNMENT__) {
        if (void* p = std::malloc(size)) return p;
    } else {
        if (void* p = aligned_allocate(size, alignment)) return p;
    }
    std::fputs("benchmark: out of memory\n", stderr);
    std::abort();
}

}  // namespace

void* operator new(std::size_t size) { return counted_allocate(size, 0); }
void* operator new[](std::size_t size) { return counted_allocate(size, 0); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    return counted_allocate(size, 0);
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return counted_allocate(size, 0);
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
void* operator new(std::size_t size, std::align_val_t alignment) {
    return counted_allocate(size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
    return counted_allocate(size, static_cast<std::size_t>(alignment));
}
void operator delete(void* p, std::align_val_t) noexcept { aligned_deallocate(p); }
void operator delete[](void* p, std::align_val_t) noexcept { aligned_deallocate(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { aligned_deallocate(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { aligned_deallocate(p); }

namespace {

constexpr size_t kDefaultTasks = 20000;
constexpr size_t kWarmupTasks = 2000;

struct Config {
    size_t tasks = kDefaultTasks;
    bool json_output = false;
};

Config parse_config(int argc, char** argv) {
    Config config;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--json") {
            config.json_output = true;
        }
    }
    if (const char* env = std::getenv("KAIRO_BENCHMARK_TASKS")) {
        if (env && *env) {
            try {
                const unsigned long value = std::stoul(env);
                if (value > 0) config.tasks = static_cast<size_t>(value);
            } catch (...) {
            }
        }
    }
    return config;
}

struct AllocWindow {
    AllocWindow() { g_counting.store(true, std::memory_order_relaxed); }
    ~AllocWindow() { g_counting.store(false, std::memory_order_relaxed); }

    void reset() {
        g_alloc_count.store(0, std::memory_order_relaxed);
        g_alloc_bytes.store(0, std::memory_order_relaxed);
    }
    size_t count() const { return g_alloc_count.load(std::memory_order_relaxed); }
    size_t bytes() const { return g_alloc_bytes.load(std::memory_order_relaxed); }
};

void report(const Config& config, const std::string& name, size_t tasks,
            const AllocWindow& window) {
    const double allocs = static_cast<double>(window.count()) /
                          static_cast<double>(tasks);
    const double bytes = static_cast<double>(window.bytes()) /
                         static_cast<double>(tasks);
    if (config.json_output) {
        std::printf(
            "{\"benchmark\": \"submit_allocations\", \"case\": \"%s\", "
            "\"ops\": %zu, \"allocs_per_task\": %.2f, \"bytes_per_task\": %.1f}\n",
            name.c_str(), tasks, allocs, bytes);
    } else {
        std::printf("%-46s %8zu tasks  %6.2f allocs/task  %8.1f bytes/task\n",
                    name.c_str(), tasks, allocs, bytes);
    }
}

template<typename Submit>
void measure(const Config& config, const std::string& name, Submit&& submit_one) {
    // 预热：惰性初始化（默认池创建、worker 启动、监控首条记录）后计数。
    for (size_t i = 0; i < kWarmupTasks; ++i) {
        submit_one(i).get();
    }
    AllocWindow window;
    window.reset();
    for (size_t i = 0; i < config.tasks; ++i) {
        submit_one(i).get();
    }
    report(config, name, config.tasks, window);
}

}  // namespace

int main(int argc, char** argv) {
    const Config config = parse_config(argc, argv);

    auto& executor = Executor::instance();
    if (!executor.initialize(ExecutorConfig{}).ok) {
        std::fputs("benchmark: executor initialize failed\n", stderr);
        return 1;
    }

    measure(config, "submit void no-args", [&](size_t) {
        return executor.submit([] {});
    });
    measure(config, "submit value-returning", [&](size_t) {
        return executor.submit([] { return 7; });
    });
    measure(config, "submit with runtime args", [&](size_t i) {
        const int a = static_cast<int>(i & 0xff);
        return executor.submit([](int lhs, int rhs) { return lhs + rhs; },
                               a, 1);
    });
    measure(config, "submit_with_handle tracked no-args", [&](size_t) {
        return executor.submit_with_handle([] {}).future;
    });
    measure(config, "submit_with_handle tracked with args", [&](size_t i) {
        const int a = static_cast<int>(i & 0xff);
        return executor.submit_with_handle(
            [](int lhs, int rhs) { return lhs + rhs; }, a, 1).future;
    });

    (void)executor.shutdown(true);
    return 0;
}
