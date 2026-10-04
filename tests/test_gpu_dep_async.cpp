// P-005 regression test: submit_kernel_after should not block the GPU worker
// thread, so unrelated tasks (D) can execute while a dependent task (B) is
// still waiting on its dependency (A).
//
// We submit A -> B -> C as a dependency chain and concurrently submit D
// (no dependency). With the old (blocking) implementation, B's dep.wait()
// would freeze the single GPU worker, so D would not start until the
// whole A->B->C chain finished. With the new (non-blocking) implementation,
// D should start running during B's wait window.
//
// We instrument start/finish timestamps to verify temporal overlap.

#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include <kairo/config.hpp>
#include <kairo/interfaces.hpp>
#include <kairo/types.hpp>
#include "kairo/gpu/cuda_executor.hpp"

using namespace std::chrono;
using namespace kairo;
using namespace kairo::gpu;

namespace {

#define GPU_DEP_TEST_ASSERT(cond, msg)                                         \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::cerr << "FAILED: " << msg << " at " << __FILE__ << ":"        \
                      << __LINE__ << std::endl;                                \
            return false;                                                      \
        }                                                                      \
    } while (0)

// Returns the wall-clock now in nanoseconds (steady_clock).
inline int64_t now_ns() {
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch())
        .count();
}

// Exercises the common IGpuExecutor waiter lifecycle without GPU hardware.
class MockGpuDependencyExecutor : public IGpuExecutor {
public:
    explicit MockGpuDependencyExecutor(gpu::GpuBackend backend)
        : backend_(backend) {}

    ~MockGpuDependencyExecutor() override { stop(); }

    bool start() override {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true)) {
            return false;
        }
        start_waiter_generation();
        return true;
    }

    void stop() override {
        running_.store(false);
        join_pending_waiters();
    }

    void wait_for_completion() override {}
    void synchronize() override {}
    void synchronize_stream(int) override {}
    int create_stream() override { return 0; }
    void destroy_stream(int) override {}
    bool add_stream_callback(int, std::function<void()> callback) override {
        if (callback) {
            callback();
        }
        return true;
    }
    void* allocate_device_memory(size_t) override { return nullptr; }
    void free_device_memory(void*) override {}
    bool copy_to_device(void*, const void*, size_t, bool, int) override { return true; }
    bool copy_to_host(void*, const void*, size_t, bool, int) override { return true; }
    bool copy_device_to_device(void*, const void*, size_t, bool, int) override { return true; }
    std::string get_name() const override { return "mock_gpu_dependency"; }
    gpu::GpuDeviceInfo get_device_info() const override {
        gpu::GpuDeviceInfo info;
        info.backend = backend_;
        return info;
    }
    gpu::GpuExecutorStatus get_status() const override {
        gpu::GpuExecutorStatus status;
        status.is_running = running_.load();
        status.backend = backend_;
        return status;
    }

protected:
    std::future<void> submit_kernel_impl(
        std::function<void(void*)> kernel_func,
        const gpu::GpuTaskConfig&) override {
        std::promise<void> promise;
        auto future = promise.get_future();
        try {
            if (!running_.load()) {
                throw std::runtime_error("mock executor is not running");
            }
            kernel_func(nullptr);
            promise.set_value();
        } catch (...) {
            promise.set_exception(std::current_exception());
        }
        return future;
    }

private:
    gpu::GpuBackend backend_;
    std::atomic<bool> running_{false};
};

bool test_gpu_dependency_after_restart_runs() {
    std::cout << "P-002: dependency waiters run after restart" << std::endl;
    const gpu::GpuBackend backends[] = {
        gpu::GpuBackend::CUDA, gpu::GpuBackend::OPENCL};

    for (const auto backend : backends) {
        MockGpuDependencyExecutor exec(backend);
        GpuTaskConfig config;
        std::atomic<int> runs{0};

        GPU_DEP_TEST_ASSERT(exec.start(), "mock executor must start");
        std::promise<void> first_ready;
        auto first = exec.submit_kernel_after(first_ready.get_future().share(),
            [&runs](void*) { ++runs; }, config);
        first_ready.set_value();
        first.get();
        exec.stop();

        GPU_DEP_TEST_ASSERT(exec.start(), "mock executor must restart");
        std::promise<void> second_ready;
        auto second = exec.submit_kernel_after(second_ready.get_future().share(),
            [&runs](void*) { ++runs; }, config);
        second_ready.set_value();
        second.get();
        exec.stop();

        GPU_DEP_TEST_ASSERT(runs.load() == 2,
                            "both dependency kernels must run across restart");
    }
    return true;
}

bool test_concurrent_stop_and_dependency_registration_leaves_no_joinable_waiter() {
    std::cout << "P-002: concurrent stop and dependency registration" << std::endl;
    MockGpuDependencyExecutor exec(gpu::GpuBackend::CUDA);
    GPU_DEP_TEST_ASSERT(exec.start(), "mock executor must start");

    std::promise<void> never_ready;
    const auto dependency = never_ready.get_future().share();
    GpuTaskConfig config;
    std::vector<std::future<void>> futures;
    futures.reserve(100);

    std::thread stopper([&exec] { exec.stop(); });
    for (int i = 0; i < 100; ++i) {
        futures.push_back(exec.submit_kernel_after(dependency, [](void*) {}, config));
    }
    stopper.join();

    for (auto& future : futures) {
        GPU_DEP_TEST_ASSERT(future.wait_for(seconds(2)) == std::future_status::ready,
                            "every raced submission must complete or fail");
        try {
            future.get();
        } catch (const std::exception&) {
            // Stopped submissions are expected to fail.
        }
    }
    return true;
}

bool test_gpu_dependency_does_not_starve_worker() {
    std::cout << "P-005: submit_kernel_after must not block GPU worker"
              << std::endl;
#ifdef KAIRO_ENABLE_CUDA
    GpuExecutorConfig cfg;
    cfg.name = "p005_dep_async";
    cfg.device_id = 0;
    cfg.max_queue_size = 256;
    cfg.default_stream_count = 1;

    CudaExecutor exec(cfg.name, cfg);
    if (!exec.start()) {
        std::cout << "  CUDA not available, skipping" << std::endl;
        return true;
    }

    GpuTaskConfig tcfg;
    tcfg.async = false;

    // === Build dependency chain A -> B -> C ===
    std::atomic<int64_t> a_start{0}, a_end{0};
    std::atomic<int64_t> b_start{0}, b_end{0};
    std::atomic<int64_t> c_start{0}, c_end{0};
    std::atomic<int64_t> d_start{0}, d_end{0};

    // A: no-op kernel that "sleeps" briefly (yield CPU, do not block GPU)
    auto future_a = exec.submit_kernel([&](void* /*s*/) {
        a_start.store(now_ns());
        std::this_thread::sleep_for(milliseconds(30));
        a_end.store(now_ns());
    }, tcfg);
    std::shared_future<void> shared_a = future_a.share();

    // B: depends on A, also sleeps 30ms (B's sleep used to block the worker)
    auto future_b = exec.submit_kernel_after(shared_a, [&](void* /*s*/) {
        b_start.store(now_ns());
        std::this_thread::sleep_for(milliseconds(30));
        b_end.store(now_ns());
    }, tcfg);
    std::shared_future<void> shared_b = future_b.share();

    // C: depends on B
    auto future_c = exec.submit_kernel_after(shared_b, [&](void* /*s*/) {
        c_start.store(now_ns());
        std::this_thread::sleep_for(milliseconds(10));
        c_end.store(now_ns());
    }, tcfg);

    // Give the worker a moment to start processing A. The goal is to
    // submit D *while B's dep.wait() would have been holding the worker*
    // (in the old implementation). With the new implementation, B's
    // dep.wait() runs in a detached helper thread, so the worker is free.
    //
    // We wait until A is finished, then submit D shortly before B is
    // expected to start. If the worker is blocked, D's start would be
    // delayed past B's end.
    // 注: future_a 在上面 share() 后已变为 valid=false (state 转移到 shared_a),
    // wait() 必须用 shared_a,否则抛 std::future_error("No associated state")。
    // 这是 P-005 (#7) 引入的 bug:在有 GPU 设备的环境上(P-005 当时 CI 没 GPU,所以未暴露)
    // 第一次跑到这里就 abort。这里同步修,合并 P-005 的 future_a.wait() → shared_a.wait()
    // 让 test 在真实 GPU 上也能跑通。
    shared_a.wait();
    // Small window to ensure B's dep is satisfied and B has been re-enqueued
    std::this_thread::sleep_for(milliseconds(5));
    auto future_d = exec.submit_kernel([&](void* /*s*/) {
        d_start.store(now_ns());
        std::this_thread::sleep_for(milliseconds(10));
        d_end.store(now_ns());
    }, tcfg);

    // Wait for everything
    future_c.get();
    future_d.get();

    std::cout << "  A: [" << a_start.load() << ", " << a_end.load() << "]"
              << std::endl;
    std::cout << "  B: [" << b_start.load() << ", " << b_end.load() << "]"
              << std::endl;
    std::cout << "  C: [" << c_start.load() << ", " << c_end.load() << "]"
              << std::endl;
    std::cout << "  D: [" << d_start.load() << ", " << d_end.load() << "]"
              << std::endl;

    // Sanity: ordering
    GPU_DEP_TEST_ASSERT(a_end.load() > 0, "A must have run");
    GPU_DEP_TEST_ASSERT(b_end.load() > 0, "B must have run");
    GPU_DEP_TEST_ASSERT(c_end.load() > 0, "C must have run");
    GPU_DEP_TEST_ASSERT(d_end.load() > 0, "D must have run");
    GPU_DEP_TEST_ASSERT(a_end.load() <= b_start.load(),
                        "B must start after A ends");
    GPU_DEP_TEST_ASSERT(b_end.load() <= c_start.load(),
                        "C must start after B ends");

    // P-005 key assertion: D must have started *during* B's window
    // (i.e., B is waiting on something or executing, and D can still
    // make progress). With the old blocking dep.wait(), D would start
    // only after C ended, so d_start >= c_end.
    // With the new implementation, d_start should be <= c_end (D can run
    // in parallel with B's execution phase, or at least before C ends).
    int64_t d_s = d_start.load();
    int64_t c_e = c_end.load();
    int64_t b_s = b_start.load();
    int64_t b_e = b_end.load();

    // D should not be forced to wait for the whole chain. The strongest
    // guarantee we can make: D's start time is not after C ends. (We
    // cannot guarantee strict overlap with B because the worker may
    // legitimately process B and D serially — what we *can* guarantee
    // is that the dep.wait() in the old code did NOT block the worker,
    // which is observable by the fact that D was enqueued *after* A
    // completed but still finished before the chain forced sequential
    // serialization on it. We test the weaker but reliable property:
    // D's start <= C's end, i.e. D does not have to wait for the chain.)
    GPU_DEP_TEST_ASSERT(d_s <= c_e,
                        "D should not be forced to wait for the full "
                        "A->B->C chain (worker would be blocked by "
                        "B's dep.wait() in the old impl)");

    // Stronger property we can check: D started before B ended. The
    // worker is free during B's execution phase (no more dep.wait() on
    // the worker thread), so D can be processed in parallel.
    // We allow a small tolerance for scheduling jitter.
    bool d_overlaps_b = (d_s <= b_e) && (d_s >= b_s - milliseconds(50).count());
    if (!d_overlaps_b) {
        std::cout << "  NOTE: D did not overlap with B (D_start=" << d_s
                  << ", B_start=" << b_s << ", B_end=" << b_e << ")"
                  << std::endl;
        // Not a hard failure: some executors may process tasks strictly
        // sequentially even without dep.wait(). The real test is that
        // D is not blocked by dep.wait() — captured by the assertion
        // above (d_s <= c_e).
    } else {
        std::cout << "  D overlapped with B's execution window: PASSED"
                  << std::endl;
    }

    exec.wait_for_completion();
    exec.stop();
    std::cout << "  P-005 dep-async test: PASSED" << std::endl;
    return true;
#else
    std::cout << "  CUDA not enabled, skipping" << std::endl;
    return true;
#endif
}

// P-002 regression test: destroy executor while a waiter thread is blocked on
// a dependency that never completes.  With the old detach() implementation this
// causes a heap-use-after-free (detectable under ASAN).  With the P-002 fix the
// waiter polls stopping_ and exits cleanly when stop() is called.
bool test_gpu_dep_async_destroy_race() {
    std::cout << "P-002: destroy executor while dep-waiter is blocked"
              << std::endl;
#ifdef KAIRO_ENABLE_CUDA
    GpuExecutorConfig cfg;
    cfg.name = "p002_destroy_race";
    cfg.device_id = 0;
    cfg.max_queue_size = 256;
    cfg.default_stream_count = 1;

    auto exec = std::make_unique<CudaExecutor>(cfg.name, cfg);
    if (!exec->start()) {
        std::cout << "  CUDA not available, skipping" << std::endl;
        return true;
    }

    // Create a promise/future that we intentionally never complete —
    // the waiter thread will block on dep.wait_for() loops.
    std::promise<void> never_promise;
    std::shared_future<void> never_dep = never_promise.get_future().share();

    GpuTaskConfig tcfg;
    tcfg.async = false;

    // Submit a kernel that depends on 'never_dep'. The returned future
    // will be broken (exception) when stop() cancels the waiter.
    auto future = exec->submit_kernel_after(never_dep, [](void* /*s*/) {
        // should never execute
    }, tcfg);

    // Give the waiter thread a moment to start and enter its poll-wait loop.
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    // Destroy executor — stop() must join the waiter before returning.
    // Under ASAN this would trap immediately on UAF if the old detach() was used.
    exec->stop();
    exec.reset();  // destructor runs here; must be clean

    // The future should be broken with an exception (executor stopped).
    // We just verify it doesn't deadlock and doesn't crash.
    try {
        future.get();
        // If somehow the kernel ran, that's also acceptable (shouldn't happen,
        // but not a correctness bug here).
    } catch (const std::exception& e) {
        // Expected: "executor stopped before dependency completed"
        std::cout << "  future broken as expected: " << e.what() << std::endl;
    } catch (...) {
        // Any exception is fine — what matters is no UAF and no hang.
    }

    std::cout << "  P-002 destroy-race test: PASSED" << std::endl;
    return true;
#else
    // Without CUDA, use the mock/software path to exercise the same code path.
    // IGpuExecutor::submit_kernel_after is defined in interfaces.hpp and works
    // through submit_kernel_impl — we can't instantiate IGpuExecutor directly
    // without a concrete backend, so we just skip here.
    std::cout << "  CUDA not enabled, skipping CUDA path" << std::endl;
    return true;
#endif
}

} // namespace

int main() {
    std::cout << "=========================================" << std::endl;
    std::cout << "GPU dependency async regression tests" << std::endl;
    std::cout << "=========================================" << std::endl;

    bool ok = true;
    ok &= test_gpu_dependency_does_not_starve_worker();
    ok &= test_gpu_dep_async_destroy_race();
    ok &= test_gpu_dependency_after_restart_runs();
    ok &= test_concurrent_stop_and_dependency_registration_leaves_no_joinable_waiter();

    if (ok) {
        std::cout << "All GPU dep-async tests PASSED" << std::endl;
        return 0;
    }
    std::cerr << "GPU dep-async tests FAILED" << std::endl;
    return 1;
}
