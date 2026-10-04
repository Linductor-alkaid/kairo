// CR-052 FIXED-direction adaptation of cr052_dependency_dfs.cpp.
//
// Original test reproduced: recursive dfs_path_exists overflowing the thread
// stack (256KB stack, n=4000 chain => SIGSEGV exit 139). After the Phase 1
// fix (iterative explicit-stack DFS), expected:
//
//   overflow-small N  : chain of N built inside a 256KB-stack thread without
//                       crashing (n=4000 and n=10000); timing reported.
//   cycle             : cycle DETECTION CORRECTNESS unchanged —
//                       A->B->C then adding C->A must be REJECTED;
//                       closing a 1000-long chain (t1000 -> t1) rejected;
//                       self-dependency rejected; after the closing edge is
//                       rejected the graph stays acyclic (a fresh legal edge
//                       still accepted). is_ready semantics preserved.
//   scale N           : chain build timing on the normal stack, so the
//                       remaining O(n^2) chain-building cost can be reported
//                       (explicitly out of scope for Phase 1 — do NOT treat
//                       slowness as fix failure).
//
// Build (normal):
//   g++ -std=c++20 -O1 -g -DKAIRO_THREAD_POOL_TEST_HOOKS -I include -I src
//     review_verification/cr052_dependency_dfs_fixed.cpp
//     build/src/libexecutor.a -lpthread -ldl
//     -o review_verification/cr052_dependency_dfs_fixed
#include "kairo/task/task_dependency_manager.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <pthread.h>
#include <cstring>
#include <string>

using kairo::TaskDependencyManager;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

double build_chain(TaskDependencyManager& mgr, size_t n, bool* all_ok) {
    using Clock = std::chrono::steady_clock;
    const auto t0 = Clock::now();
    for (size_t i = 1; i <= n; ++i) {
        if (!mgr.add_dependency("t" + std::to_string(i),
                                "t" + std::to_string(i - 1))) {
            *all_ok = false;
        }
    }
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

struct ThreadArg {
    size_t n;
    int failures = 0;
};

void* small_stack_main(void* arg) {
    auto* a = static_cast<ThreadArg*>(arg);
    TaskDependencyManager mgr;
    bool ok = true;
    const double ms = build_chain(mgr, a->n, &ok);
    std::printf("[SMALLSTACK] chain of %zu built in %.1fms%s\n", a->n, ms,
                ok ? "" : " (add_dependency returned false!)");
    if (!ok) a->failures = 1;
    return nullptr;
}

int run_overflow_small(size_t n) {
    std::printf("=== CR-052 fixed: chain of %zu inside a 256KB-stack thread "
                "(pre-fix: SIGSEGV at n=4000) ===\n", n);
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
    check(arg.failures == 0,
          "overflow-small: survived n=" + std::to_string(n) +
              " on a 256KB stack (iterative DFS)");
    return 0;
}

void run_cycle() {
    std::printf("=== CR-052 fixed: cycle detection correctness ===\n");
    {
        TaskDependencyManager mgr;
        check(mgr.add_dependency("A", "B"), "cycle: add A->B accepted");
        check(mgr.add_dependency("B", "C"), "cycle: add B->C accepted");
        check(!mgr.add_dependency("C", "A"),
              "cycle: closing edge C->A REJECTED (real 3-cycle)");
        // graph must remain acyclic and usable after the rejection
        check(mgr.add_dependency("D", "C"),
              "cycle: fresh legal edge D->C still accepted after rejection");
        check(!mgr.add_dependency("C", "C"), "cycle: self-dependency rejected");
        check(!mgr.is_ready("A"), "cycle: A not ready (depends on B, uncompleted)");
        mgr.mark_completed("B");
        mgr.mark_completed("C");
        mgr.mark_completed("D");
        check(mgr.is_ready("A"), "cycle: A ready once B completed");
        check(!mgr.is_completed("A"), "cycle: A still not marked completed");
    }
    {
        // chain semantics: add_dependency(t_i, t_{i-1}) == "t_i depends on t_{i-1}",
        // edges point downward. The real cycle-closing edge is t0 -> t1000
        // ("t0 depends on t1000": path t1000->t999->...->t1->t0 exists).
        // The long-range downward edge t1000 -> t1 is transitive-but-acyclic
        // and must stay ACCEPTED.
        TaskDependencyManager mgr;
        bool ok = true;
        build_chain(mgr, 1000, &ok);
        check(ok, "cycle-long: 1000-chain built with all adds accepted");
        check(mgr.add_dependency("t1000", "t1"),
              "cycle-long: transitive-but-acyclic edge t1000->t1 accepted");
        check(!mgr.add_dependency("t0", "t1000"),
              "cycle-long: closing edge t0->t1000 REJECTED (real 1001-cycle, "
              "DFS must traverse the whole chain)");
        check(!mgr.add_dependency("t500", "t900"),
              "cycle-long: back-edge t500->t900 REJECTED (t900 reaches t500 "
              "down the chain)");
    }
}

int run_scale(size_t n) {
    std::printf("=== CR-052 fixed: chain cost (informational, O(n^2) build "
                "cost is deferred to a later phase) ===\n");
    TaskDependencyManager mgr;
    bool ok = true;
    const double ms = build_chain(mgr, n, &ok);
    check(ok, "scale: chain of " + std::to_string(n) + " built, all adds ok");
    std::printf("scale: n=%zu total=%.1fms per-edge=%.1fus\n", n, ms,
                ms * 1000.0 / n);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "cycle";
    if (mode == "cycle") {
        run_cycle();
    } else if (mode == "overflow-small") {
        return run_overflow_small(std::strtoul(argv[2], nullptr, 10));
    } else if (mode == "scale") {
        return run_scale(std::strtoul(argv[2], nullptr, 10));
    } else {
        std::printf("unknown mode\n");
        return 2;
    }
    std::printf("CR-052-FIXED VERDICT: %s (%d checks failed)\n",
                g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
