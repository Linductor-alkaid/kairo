// CR-052 verification: TaskDependencyManager cycle detection
// (src/executor/task/task_dependency_manager.cpp):
//   (a) O(n^2): every add_dependency() runs has_cycle() -> DFS from the new
//       dependency over the whole existing graph, while holding the unique
//       write lock. Building a chain 1->2->...->n costs sum(i) node visits.
//   (b) Unbounded recursion depth: dfs_path_exists() recurses once per node
//       along the chain; a deep chain overflows the thread stack (SIGSEGV).
//
// Modes:
//   cr052_dependency_dfs scale               - add-chain timings n=2.5k..20k
//   cr052_dependency_dfs overflow-small N    - build chain of N inside a
//                                              256KB-stack thread; expect
//                                              SIGSEGV (exit code 139)
//   cr052_dependency_dfs overflow-default N  - build chain of N on the main
//                                              thread (8MB stack); expect
//                                              SIGSEGV if N deep enough
#include "kairo/task/task_dependency_manager.hpp"

#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <string>
#include <thread>
#include <atomic>

using kairo::TaskDependencyManager;

namespace {

double build_chain(TaskDependencyManager& mgr, size_t n) {
    using Clock = std::chrono::steady_clock;
    const auto t0 = Clock::now();
    for (size_t i = 1; i <= n; ++i) {
        mgr.add_dependency("t" + std::to_string(i), "t" + std::to_string(i - 1));
    }
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    return ms;
}

int run_scale() {
    std::printf("=== CR-052(a): chain add_dependency cost (write lock held during DFS) ===\n");
    double prev = 0;
    for (size_t n : {2500, 5000, 10000, 20000}) {
        TaskDependencyManager mgr;
        const double ms = build_chain(mgr, n);
        const double visits = static_cast<double>(n) * (n + 1) / 2.0;
        std::printf("n=%6zu  total=%9.1fms  per-edge=%7.1fus  ratio_vs_prev=%s  "
                    "(quadratic prediction: 4x per doubling)\n",
                    n, ms, ms * 1000.0 / n,
                    prev > 0 ? std::to_string(ms / prev).c_str() : "-");
        prev = ms;
    }
    std::printf("note: n=200000 would cost ~100x the n=20000 time and the final\n"
                "      add_dependency() alone recurses ~200000 frames deep.\n");
    return 0;
}

struct ThreadArg {
    size_t n;
};

void* small_stack_main(void* arg) {
    const size_t n = static_cast<ThreadArg*>(arg)->n;
    TaskDependencyManager mgr;
    const double ms = build_chain(mgr, n);
    std::printf("SMALLSTACK SURVIVED: chain of %zu built in %.1fms\n", n, ms);
    return nullptr;
}

int run_overflow_small(size_t n) {
    std::printf("=== CR-052(b): chain of %zu inside a 256KB-stack thread ===\n", n);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    pthread_t tid;
    ThreadArg arg{n};
    const int rc = pthread_create(&tid, &attr, small_stack_main, &arg);
    if (rc != 0) {
        std::printf("pthread_create failed: %s\n", std::strerror(rc));
        return 2;
    }
    pthread_join(tid, nullptr);
    std::printf("SMALLSTACK SURVIVED (no overflow at n=%zu)\n", n);
    return 0;
}

int run_overflow_default(size_t n) {
    std::printf("=== CR-052(b): chain of %zu on main thread (8MB stack) ===\n", n);
    TaskDependencyManager mgr;
    const double ms = build_chain(mgr, n);
    std::printf("DEFAULTSTACK SURVIVED: chain of %zu built in %.1fms\n", n, ms);
    return 0;
}

// Show that has_cycle's DFS runs while holding the exclusive write lock:
// a cheap independent add_dependency() issued while a deep DFS is in flight
// must wait for the whole DFS.
int run_lockhold(size_t chain) {
    std::printf("=== CR-052(c): DFS under exclusive lock (chain=%zu) ===\n", chain);
    TaskDependencyManager mgr;
    build_chain(mgr, chain);
    using Clock = std::chrono::steady_clock;
    double a_ms = 0, b_ms = 0;
    std::atomic<int> phase{0};
    auto a = std::thread([&] {
        while (phase.load() < 1) {}
        const auto t0 = Clock::now();
        mgr.add_dependency("x1", "t" + std::to_string(chain));  // DFS from chain tail over all chain nodes
        a_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        phase.store(2);
    });
    auto b = std::thread([&] {
        while (phase.load() < 1) {}  // start racing with A's add
        // issue the cheap add slightly later, while A surely holds the lock
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        const auto t0 = Clock::now();
        mgr.add_dependency("u1", "u2");  // two fresh nodes, 1-node DFS
        b_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        phase.store(3);
    });
    phase.store(1);
    a.join();
    b.join();
    std::printf("A (deep DFS add, ~%zu-node scan): %.3fms\n", chain, a_ms);
    std::printf("B (independent 1-node add, issued 200us after A started): %.3fms\n",
                b_ms);
    std::printf("lock-hold verdict: %s\n",
                b_ms > a_ms * 0.3 ? "CONFIRMED - cheap add blocked behind DFS"
                                  : "NOT DEMONSTRATED");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "scale";
    if (mode == "scale") return run_scale();
    if (mode == "overflow-small") {
        return run_overflow_small(std::strtoul(argv[2], nullptr, 10));
    }
    if (mode == "overflow-default") {
        return run_overflow_default(std::strtoul(argv[2], nullptr, 10));
    }
    if (mode == "lockhold") {
        return run_lockhold(std::strtoul(argv[2], nullptr, 10));
    }
    std::printf("unknown mode\n");
    return 2;
}
