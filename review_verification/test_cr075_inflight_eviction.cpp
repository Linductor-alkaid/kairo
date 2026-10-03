// CR-075: shrinking in-flight capacity evicts entries and counts them as
// "dropped"; dropped_count>0 latches in_flight_diagnostics_incomplete() true
// forever, even after every remaining task completes normally.
#include "kairo/monitor/task_monitor.hpp"

#include <cstdio>
#include <string>

using kairo::TaskLifecycleState;
using kairo::monitor::TaskMonitor;

int main() {
    TaskMonitor m;
    m.set_in_flight_capacity(1000);

    // Fill 1000 in-flight entries (queued, never completed yet).
    for (int i = 0; i < 1000; ++i) {
        m.record_task_queued("task_" + std::to_string(i), "work", "default");
    }
    auto d0 = m.get_in_flight_diagnostics();
    std::printf("STEP1 full: count=%zu dropped=%zu incomplete=%d\n",
                d0.count, d0.dropped_count, d0.incomplete ? 1 : 0);

    // Shrink capacity to 10 -> evicts 990 oldest, each ++dropped.
    m.set_in_flight_capacity(10);
    auto d1 = m.get_in_flight_diagnostics();
    std::printf("STEP2 shrink to 10: count=%zu dropped=%zu incomplete=%d\n",
                d1.count, d1.dropped_count, d1.incomplete ? 1 : 0);

    // Recover: restore capacity, complete ALL remaining tasks normally.
    m.set_in_flight_capacity(1000);
    for (int i = 990; i < 1000; ++i) {
        m.record_task_start("task_" + std::to_string(i), "work");
        m.record_task_complete("task_" + std::to_string(i), true, 100);
    }
    auto d2 = m.get_in_flight_diagnostics();
    std::printf("STEP3 after full recovery: count=%zu dropped=%zu incomplete=%d\n",
                d2.count, d2.dropped_count, d2.incomplete ? 1 : 0);
    std::printf("in_flight_diagnostics_incomplete()=%d get_in_flight_dropped_count()=%zu\n",
                  m.in_flight_diagnostics_incomplete() ? 1 : 0,
                  m.get_in_flight_dropped_count());

    if (d1.dropped_count > 0 && d1.incomplete) {
        std::printf("EVICTION: evictions counted as dropped and incomplete=true -> CONFIRMED\n");
    }
    if (d2.count == 0 && d2.incomplete) {
        std::printf("LATCH: empty table still reports incomplete=true (never resets) -> CONFIRMED\n");
    }
    return 0;
}
