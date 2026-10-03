// CR-053 verification: RealtimeThreadExecutor queue capacity rounding is
// inconsistent with the object pool capacity.
//
// Constructor (realtime_thread_executor.cpp:50-51):
//   lockfree_queue_(queue_capacity, ...)  -> LockFreeQueue rounds 1000 -> 1024
//                                            (lockfree_queue.hpp round_to_power_of_two)
//   task_pool_(queue_capacity)            -> ObjectPool keeps raw 1000
// get_status() reports status.queue_capacity = lockfree_queue_.capacity() = 1024.
//
// Observable consequence: with queue_capacity=1000 the status reports 1024
// usable slots, but pushes fail with pool exhaustion after 1000 in-flight
// tasks (pool_empty) BEFORE the queue-full path (1024) can ever engage.
//
// The RT cycle callback blocks on a gate so the consumer never drains while
// we push; max_tasks_per_cycle=0 (unlimited) afterwards for a fast drain.
#include <kairo/realtime_thread_executor.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

using kairo::RealtimeThreadConfig;
using kairo::RealtimeThreadExecutor;

int main() {
    constexpr size_t kRequestedCapacity = 1000;
    constexpr size_t kPushCount = 1001;

    RealtimeThreadConfig config;
    config.thread_name = "cr053_rt";
    config.cycle_period_ns = 500'000'000;  // 500ms
    config.max_tasks_per_cycle = 0;        // unlimited drain per cycle
    config.thread_priority = 0;            // plain scheduling

    std::atomic<bool> gate_open{false};
    std::atomic<bool> callback_entered{false};
    config.cycle_callback = [&] {
        callback_entered.store(true);
        while (!gate_open.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };

    RealtimeThreadExecutor executor("cr053", config, /*enable_stats=*/true,
                                    kRequestedCapacity);
    if (!executor.start()) {
        std::printf("start FAILED\n");
        return 2;
    }
    // wait until the RT thread is parked inside the first cycle callback
    while (!callback_entered.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    size_t ok = 0, failed = 0;
    for (size_t i = 0; i < kPushCount; ++i) {
        if (executor.push_task([] {})) {
            ++ok;
        } else {
            ++failed;
        }
    }
    // consumer drain
    gate_open.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    const auto status = executor.get_status();
    executor.stop();

    std::printf("requested queue_capacity=%zu\n", kRequestedCapacity);
    std::printf("status.queue_capacity (LockFreeQueue, rounded)   = %zu\n",
                status.queue_capacity);
    std::printf("pushed ok=%zu failed=%zu of %zu\n", ok, failed, kPushCount);
    std::printf("pool_exhausted_count=%llu queue_full_count=%llu dropped_task_count=%llu\n",
                (unsigned long long)status.pool_exhausted_count,
                (unsigned long long)status.queue_full_count,
                (unsigned long long)status.dropped_task_count);
    std::printf("peak_queue_size=%llu failed_pushes=%llu\n",
                (unsigned long long)status.peak_queue_size,
                (unsigned long long)status.failed_pushes);
    std::printf("priority_applied=%d cpu_affinity_applied=%d (env note)\n",
                status.priority_applied ? 1 : 0,
                status.cpu_affinity_applied ? 1 : 0);

    const bool rounding_visible = status.queue_capacity == 1024;
    const bool pool_limited = failed == 1 && status.pool_exhausted_count == 1 &&
                              status.queue_full_count == 0;
    std::printf("verdict: %s\n",
                (rounding_visible && pool_limited)
                    ? "CONFIRMED - status reports 1024 but only 1000 slots usable; "
                      "pool exhausts before queue-full"
                    : (rounding_visible
                           ? "PARTIAL - rounding visible, pool attribution differs"
                           : "NOT REPRODUCED"));
    return (rounding_visible && pool_limited) ? 1 : 0;
}
