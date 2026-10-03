// Regression test for P-260618-001: CudaExecutor::wait_for_completion() UAF TOCTOU.
//
// The worker_thread_func previously bumped active_kernels_ AFTER releasing
// queue_mutex_, so wait_for_completion()'s predicate
// (task_queue_.empty() && active_kernels_.load()==0) could observe a false
// quiescent state between queue.pop() and the in-flight count bump, returning
// to the caller while a freshly-popped kernel was about to run.
//
// This test hammers the worker / queue / counter / wait_for_completion path
// with concurrent submitters and a concurrent waiter and asserts the
// post-conditions the fix must guarantee: after a final drain no kernel is
// in flight (active_kernels_==0) and every submission is accounted for
// (completed + failed == N*M). It is a no-op when KAIRO_ENABLE_CUDA is off
// so it builds everywhere, and skips at runtime when no CUDA runtime/device
// is available (start() fails).

#include <atomic>
#include <chrono>
#include <exception>
#include <iostream>
#include <thread>
#include <vector>

#include <kairo/config.hpp>
#include <kairo/types.hpp>
#include <kairo/interfaces.hpp>
#include "kairo/gpu/cuda_executor.hpp"

using namespace kairo;
using namespace kairo::gpu;

#define TEST_ASSERT(condition, message)                                       \
    do {                                                                      \
        if (!(condition)) {                                                   \
            std::cerr << "FAILED: " << message << " at " << __FILE__ << ":"   \
                      << __LINE__ << std::endl;                               \
            return false;                                                     \
        }                                                                     \
    } while (0)

bool test_wait_for_completion_race();

bool test_wait_for_completion_race() {
    std::cout << "Testing CudaExecutor wait_for_completion TOCTOU race..." << std::endl;

#ifdef KAIRO_ENABLE_CUDA
    const int N = 8;    // submitter threads
    const int M = 200;  // submissions per thread

    GpuExecutorConfig config;
    config.name = "test_cuda_wait_race";
    config.backend = GpuBackend::CUDA;
    config.device_id = 0;
    // Size the queue so submits never block on backpressure (we want to
    // stress the pop/active++ window, not the not-full CV).
    config.max_queue_size = static_cast<size_t>(N) * static_cast<size_t>(M) + 1024;
    config.default_stream_count = 1;

    CudaExecutor executor(config.name, config);

    // start() spins up the single worker thread and returns false when the
    // CUDA runtime/device is unavailable — that still exercises nothing, so
    // skip cleanly in that case.
    if (!executor.start()) {
        std::cout << "  CUDA not available, skipping wait_for_completion race test"
                  << std::endl;
        return true;
    }

    std::atomic<bool> stop_waiter{false};

    // Concurrent waiter: call wait_for_completion() in a tight loop for the
    // whole run. With the unfixed code this repeatedly races the worker's
    // pop/active++ window and could observe a false quiescent state.
    std::thread waiter([&]() {
        while (!stop_waiter.load()) {
            (void)executor.wait_for_completion(std::chrono::seconds{300});
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    });

    // Concurrent submitters: each loops M times submitting a trivial kernel.
    std::vector<std::thread> submitters;
    submitters.reserve(static_cast<size_t>(N));
    for (int t = 0; t < N; ++t) {
        submitters.emplace_back([&executor]() {
            GpuTaskConfig tc;
            tc.async = false;
            auto empty_kernel = [](void*) {};
            for (int i = 0; i < M; ++i) {
                executor.submit_kernel(empty_kernel, tc);
            }
        });
    }

    for (auto& th : submitters) {
        th.join();
    }

    // One final drain after every submission is queued.
    (void)executor.wait_for_completion(std::chrono::seconds{300});

    // Stop the waiter only after the queue is drained so its in-flight
    // wait_for_completion() unblocks promptly.
    stop_waiter.store(true);
    waiter.join();

    const size_t total = static_cast<size_t>(N) * static_cast<size_t>(M);
    auto status = executor.get_status();

    // Post-condition the fix must guarantee: wait_for_completion() may only
    // return when no kernel is in flight.
    TEST_ASSERT(status.active_kernels == 0u,
                "active_kernels must be 0 after wait_for_completion drains");
    // Every submitted kernel must be accounted for.
    TEST_ASSERT(status.completed_kernels + status.failed_kernels == total,
                "completed + failed kernels must equal total submissions");

    executor.stop();
    std::cout << "  CudaExecutor wait_for_completion race: PASSED" << std::endl;
    return true;
#else
    std::cout << "  CUDA support not enabled at compile time, skipping test" << std::endl;
    return true;
#endif
}

int main() {
    try {
        std::cout << "Starting CudaExecutor wait_for_completion race test..." << std::endl;
        bool ok = test_wait_for_completion_race();
        std::cout << (ok ? "PASSED" : "FAILED") << std::endl;
        return ok ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "FATAL: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "FATAL: unknown exception" << std::endl;
        return 1;
    }
}
