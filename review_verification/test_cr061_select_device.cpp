// CR-061: select_best_device (task_scheduler_optimizer.cpp:155-163) seeds
// min_cost with the first-iterated device's RAW load (estimated_total_cost,
// line 155-156) WITHOUT adding task.estimated_cost, then compares every
// device's load+task against it (line 159). Consequence: when the
// first-iterated device F and some device G satisfy
//     load(G) < load(F)                (G truly cheaper including task)
//     load(G) + task > load(F)         (G cannot beat the bare seed)
// the function returns F even though F+task > G+task. Correct pick is G.
//
// unordered_map iteration order is unspecified; on libstdc++ the most
// recently inserted distinct-bucket key sits at the front. We therefore run
// both insertion orders and assert the decision STRUCTURE, not a fixed
// device number: whichever device begin() lands on wins whenever its bare
// load is smallest, even if its load+task is NOT smallest.
#include "kairo/gpu/task_scheduler_optimizer.hpp"

#include <cstdio>

using kairo::gpu::TaskSchedulerOptimizer;
using kairo::gpu::DeviceLoad;
using kairo::gpu::GpuTaskNode;

namespace {

// Returns 1 if the defect reproduced, 0 otherwise.
int run_scenario(int first_id, size_t first_load, int second_id, size_t second_load,
                 size_t task_cost, const char* label) {
    TaskSchedulerOptimizer sched;  // default: enable_load_balancing = true
    sched.update_device_load(DeviceLoad{first_id, 0, first_load, 0.0});
    sched.update_device_load(DeviceLoad{second_id, 0, second_load, 0.0});

    std::printf("\n=== %s ===\n", label);
    std::printf("insertion order: dev %d (load %zu) then dev %d (load %zu)\n",
                first_id, first_load, second_id, second_load);
    std::printf("get_device_loads() iteration order:");
    for (const auto& l : sched.get_device_loads()) {
        std::printf(" [dev %d: bare=%zu, +task=%zu]", l.device_id,
                    l.estimated_total_cost, l.estimated_total_cost + task_cost);
    }
    std::printf("\n");

    GpuTaskNode task;
    task.task_id = "t";
    task.estimated_cost = task_cost;
    const int chosen = sched.select_best_device(task);

    // Ground truth: device minimizing load + task.
    const size_t cost_first = first_load + task_cost;
    const size_t cost_second = second_load + task_cost;
    const int correct = (cost_first <= cost_second) ? first_id : second_id;

    std::printf("select_best_device(estimated_cost=%zu) -> dev %d "
                "(cost %zu); correct = dev %d (cost %zu)\n",
                task_cost, chosen,
                chosen == first_id ? cost_first : cost_second,
                correct, correct == first_id ? cost_first : cost_second);

    if (chosen != correct) {
        std::printf("-> WRONG DEVICE SELECTED (seed used bare load of the "
                    "first-iterated device, excluding task cost)\n");
        return 1;
    }
    std::printf("-> selection happens to be correct for this iteration order "
                "(begin() already holds the cheapest total)\n");
    return 0;
}

}  // namespace

int main() {
    // High load inserted last -> it becomes begin(); its bare load is the
    // seed and its +task total is the worst. This must pick the other device.
    int bug1 = run_scenario(0, 90, 1, 100, 50,
                            "begin() = high-load device (dev1 load=100, dev0 load=90, task=50)");

    // Mirror order: low-load device is begin(); correct-by-luck.
    int bug2 = run_scenario(0, 100, 1, 90, 50,
                            "begin() = low-load device (dev0 load=100, dev1 load=90, task=50)");

    // Larger spread where even +task cannot flip: control (should be right).
    int bug3 = run_scenario(0, 90, 1, 1000, 50,
                            "control: dev1 load=1000 vs dev0 load=90, task=50");

    std::printf("\nVERDICT: %s\n",
                bug1 ? "CONFIRMED - select_best_device returned the device with "
                       "the HIGHEST load+task because the initial min_cost "
                       "omitted task.estimated_cost"
                     : "NOT REPRODUCED");
    return bug1 ? 0 : 1;  // exit 0 == defect reproduced
}
