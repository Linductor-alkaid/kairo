// CR-062: TaskSchedulerOptimizer
//  (a) remove_task() on an uncompleted dependency makes downstream tasks look
//      ready: get_ready_tasks() only blocks on deps that are in completed_tasks_
//      OR still in task_graph_; a removed (erased) dep satisfies neither, so
//      the dependent is reported ready.
//  (b) completed_tasks_ grows without bound: mark_completed() inserts into it
//      and no public API ever erases from it. Measured via process VmRSS.
#include "kairo/gpu/task_scheduler_optimizer.hpp"

#include <cstdio>
#include <cstring>
#include <string>

using kairo::gpu::TaskSchedulerOptimizer;
using kairo::gpu::GpuTaskNode;

namespace {

// Reads VmRSS (kB) from /proc/self/status.
long vm_rss_kb() {
    FILE* f = std::fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[256];
    long kb = -1;
    while (std::fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, "VmRSS:", 6) == 0) {
            std::sscanf(line + 6, "%ld", &kb);
            break;
        }
    }
    std::fclose(f);
    return kb;
}

void part_a() {
    std::printf("\n--- (a) remove_task breaks dependency accounting ---\n");
    TaskSchedulerOptimizer sched;

    GpuTaskNode a;
    a.task_id = "A";
    GpuTaskNode b;
    b.task_id = "B";
    b.dependencies = {"A"};
    bool ok = sched.add_task(a) && sched.add_task(b);
    std::printf("add A, add B(depends on A): %s\n", ok ? "ok" : "FAILED");

    // Baseline sanity: with A still pending, B must NOT be ready.
    auto ready0 = sched.get_ready_tasks();
    bool b_ready_before = false, a_ready_before = false;
    for (auto& t : ready0) {
        if (t.task_id == "A") a_ready_before = true;
        if (t.task_id == "B") b_ready_before = true;
    }
    std::printf("baseline: ready = {A:%s, B:%s} (expected A=true B=false)\n",
                a_ready_before ? "true" : "false", b_ready_before ? "true" : "false");

    // Control: mark_completed on A properly releases B.
    {
        TaskSchedulerOptimizer ctrl;
        GpuTaskNode ca; ca.task_id = "A";
        GpuTaskNode cb; cb.task_id = "B"; cb.dependencies = {"A"};
        ctrl.add_task(ca);
        ctrl.add_task(cb);
        ctrl.mark_completed("A");
        bool b_released = false;
        for (auto& t : ctrl.get_ready_tasks()) {
            if (t.task_id == "B") b_released = true;
        }
        std::printf("control mark_completed(A): B ready = %s (expected true)\n",
                    b_released ? "true" : "false");
    }

    // The bug: remove (never completed) A, B becomes "ready".
    sched.remove_task("A");
    bool b_ready_after = false;
    for (auto& t : sched.get_ready_tasks()) {
        if (t.task_id == "B") b_ready_after = true;
    }
    std::printf("after remove_task(A) (A never completed): B ready = %s\n",
                b_ready_after ? "true" : "false");
    std::printf("(a) %s\n", b_ready_after
        ? "CONFIRMED - downstream task reported ready although its dependency "
          "was removed without completing"
        : "NOT REPRODUCED");
}

void part_b() {
    std::printf("\n--- (b) completed_tasks_ unbounded growth (1M add+complete) ---\n");
    TaskSchedulerOptimizer sched;
    const int kIters = 1'000'000;

    const long rss0 = vm_rss_kb();
    long prev = rss0;
    for (int i = 1; i <= kIters; ++i) {
        GpuTaskNode t;
        t.task_id = "task-" + std::to_string(i);
        sched.add_task(t);
        sched.mark_completed(t.task_id);
        if (i % 250'000 == 0) {
            const long rss = vm_rss_kb();
            std::printf("after %7d add+mark_completed: VmRSS=%ld kB (delta %+ld kB)\n",
                        i, rss, rss - prev);
            prev = rss;
        }
    }
    const long rss1 = vm_rss_kb();
    std::printf("pending_count()=%zu (graph itself empty), "
                "get_stats().total_tasks_scheduled=%zu\n",
                sched.pending_count(), sched.get_stats().total_tasks_scheduled);
    std::printf("RSS before=%ld kB, after=%ld kB, growth=%ld kB (~%.1f MB)\n",
                rss0, rss1, rss1 - rss0, (rss1 - rss0) / 1024.0);
    std::printf("Cleanup API check: header offers reset_stats() (stats only); "
                "no method clears completed_tasks_ (no clear_completed()/"
                "reset()/prune). mark_completed() at .cpp:34-43 only inserts.\n");
    std::printf("(b) %s\n", (rss1 - rss0) > 8 * 1024
        ? "CONFIRMED - RSS grows monotonically with completed task count; "
          "completed_tasks_ is never reclaimed"
        : "NOT REPRODUCED (growth below threshold)");
}

}  // namespace

int main() {
    part_a();
    part_b();
    return 0;
}
