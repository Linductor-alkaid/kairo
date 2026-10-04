// CR-061 _fixed2 variant: reverse unordered_map iteration order.
//
// The base test (test_cr061_select_device.cpp) inserts the low-load device
// first in every scenario, so on libstdc++ (most recently inserted key at
// begin()) both of its scenarios iterate [dev1, dev0] — ONE iteration order.
// This variant inserts the high-load device LAST so begin() is the OTHER
// device, covering the reverse iteration order:
//   - case 1 mirrors base bug1 with begin() = dev0 (bare 100, +task 150);
//     the old buggy seed (bare 100) would beat dev1's 140 and pick dev0.
//   - case 2 extends to three devices with begin() = highest bare load.
// Ground truth in every case: argmin(load + task).
#include "kairo/gpu/task_scheduler_optimizer.hpp"

#include <cstdio>
#include <vector>

using kairo::gpu::TaskSchedulerOptimizer;
using kairo::gpu::DeviceLoad;
using kairo::gpu::GpuTaskNode;

namespace {

int failures = 0;

// loads: device_id -> load, inserted in the given order; last insert = begin().
void run_case(const std::vector<std::pair<int, size_t>>& insertion,
              size_t task_cost, const char* label) {
    TaskSchedulerOptimizer sched;
    for (const auto& [id, load] : insertion) {
        sched.update_device_load(DeviceLoad{id, 0, load, 0.0});
    }

    std::printf("\n=== %s ===\n", label);
    std::printf("insertion order:");
    for (const auto& [id, load] : insertion) std::printf(" dev%d(%zu)", id, load);
    std::printf("\nget_device_loads() iteration order:");
    for (const auto& l : sched.get_device_loads()) {
        std::printf(" [dev %d: bare=%zu, +task=%zu]", l.device_id,
                    l.estimated_total_cost, l.estimated_total_cost + task_cost);
    }
    std::printf("\n");

    GpuTaskNode task;
    task.task_id = "t";
    task.estimated_cost = task_cost;
    const int chosen = sched.select_best_device(task);

    // Ground truth: argmin(load + task); ties -> smaller id (implementation
    // keeps first-found minimum).
    int correct = -1;
    size_t best = ~size_t{0};
    for (const auto& l : sched.get_device_loads()) {
        const size_t total = l.estimated_total_cost + task_cost;
        if (total < best) {
            best = total;
            correct = l.device_id;
        }
    }

    std::printf("select_best_device(task=%zu) -> dev %d; correct = dev %d\n",
                task_cost, chosen, correct);
    if (chosen != correct) {
        std::printf("-> WRONG DEVICE (old-seed formula would pick begin()'s "
                    "bare load)\n");
        ++failures;
    } else {
        std::printf("-> correct (lowest total cost selected)\n");
    }
}

}  // namespace

int main() {
    // Case 1: reverse of base bug1 — begin() = dev0 (bare 100 > dev1's 90).
    // Old formula: seed = 100, dev1+task=140 never beats it -> picks dev0
    // (total 150) although dev1 (140) is cheaper.
    run_case({{1, 90}, {0, 100}}, 50,
             "reverse order: begin()=dev0 bare=100, dev1 bare=90, task=50");

    // Case 2: three devices, begin() = highest bare load.
    run_case({{0, 90}, {1, 95}, {2, 100}}, 50,
             "reverse order, 3 devices: begin()=dev2 bare=100, task=50");

    // Case 3: control — spread larger than task, begin() = highest.
    run_case({{0, 90}, {1, 1000}}, 50,
             "control: begin()=dev1 bare=1000, dev0 bare=90, task=50");

    std::printf("\nVERDICT: %s\n",
                failures ? "FAILED - wrong device under reverse iteration order"
                         : "PASSED - correct device under reverse iteration order");
    return failures ? 1 : 0;
}
