// CR-001 FIXED-direction adaptation of cr001_singleton_exit_uaf.cpp.
//
// Original test (cr001_singleton_exit_uaf.cpp) was written to REPRODUCE the
// defect: singleton ~Executor did not drain, so a tracked in-flight task woke
// inside the static-destruction window and touched already-destroyed facade
// members (ASAN heap-use-after-free / SEGV).
//
// After the Phase 1 fix (~Executor singleton path calls shutdown(true) under
// has_default_async_executor() guard), the expected behavior is:
//   - ~Executor (function-local static of Executor::instance()) drains the
//     in-flight tracked task BEFORE global objects are destroyed;
//   - the global checker below is constructed before main (=> destroyed AFTER
//     the function-local static), so if the task flag is not set by then the
//     drain did not happen -> FAIL;
//   - process exits 0 with no ASAN report.
//
// CRITICAL: the task must go through the TRACKED path (submit_with_handle);
// submit_auto/submit legacy paths never touch the registry/task-dependencies
// members involved in CR-001.
//
// Build (ASAN):
//   g++ -std=c++20 -O1 -g -fsanitize=address -fno-omit-frame-pointer
//     -DKAIRO_THREAD_POOL_TEST_HOOKS -I include -I src
//     review_verification/cr001_singleton_exit_uaf_fixed.cpp
//     build-asan/src/libexecutor.a -lpthread -ldl
//     -o review_verification/cr001_singleton_exit_uaf_fixed
#include <kairo/executor.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

using namespace kairo;
using namespace std::chrono_literals;

namespace {

std::atomic<bool> g_armed{false};
std::atomic<bool> g_task_ran{false};

// Constructed before main => destroyed AFTER the function-local static
// Executor::instance() object during static destruction (reverse order).
// If the singleton dtor drained, the task flag is already set here.
struct ExitChecker {
    ~ExitChecker() {
        if (!g_armed.load()) {
            std::printf("[checker] not armed, skipping\n");
            return;
        }
        if (g_task_ran.load()) {
            std::printf("[checker] PASS: tracked task completed during "
                        "~Executor drain (before global destruction)\n");
        } else {
            std::printf("[checker] FAIL: reached global destruction without "
                        "the tracked task having run (no drain)\n");
            std::fflush(stdout);
            std::_Exit(3);  // hard-fail without risking secondary crashes
        }
        std::fflush(stdout);
    }
};

ExitChecker g_checker;

}  // namespace

int main() {
    auto& ex = Executor::instance();
    if (!ex.initialize(ExecutorConfig{})) {
        std::printf("initialize failed\n");
        return 1;
    }
    auto payload = std::make_shared<std::string>("heap-payload");
    g_armed.store(true);
    // tracked path (submit_with_handle) — required to trigger CR-001 surface
    auto sub = ex.submit_with_handle([payload] {
        std::this_thread::sleep_for(2s);
        g_task_ran.store(true);
        std::printf("[task] ran during static destruction window, payload=%s\n",
                    payload->c_str());
        std::fflush(stdout);
    });
    (void)sub;
    std::printf("[main] tracked task submitted (2s sleep), returning "
                "immediately\n");
    std::fflush(stdout);
    return 0;  // ~Executor must shutdown(true)-drain; then checker verifies
}
