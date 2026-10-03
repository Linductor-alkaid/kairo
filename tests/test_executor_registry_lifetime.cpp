#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <thread>

#include <kairo/executor.hpp>

using namespace kairo;
using namespace std::chrono_literals;

#define TEST_ASSERT(condition, message)                                      \
    do {                                                                     \
        if (!(condition)) {                                                  \
            std::cerr << "FAILED: " << message << " at " << __FILE__       \
                      << ':' << __LINE__ << std::endl;                      \
            return false;                                                    \
        }                                                                    \
    } while (0)

namespace {

RealtimeThreadConfig realtime_config() {
    RealtimeThreadConfig config;
    config.thread_name = "registry-lifetime";
    config.cycle_period_ns = 1'000'000;
    config.enable_process_memory_lock = false;
    config.timer_slack_ns = 0;
    config.cycle_callback = [] {};
    return config;
}

bool test_snapshot_survives_registry_removal() {
    ExecutorManager manager;
    auto backend = manager.create_realtime_executor("snapshot", realtime_config());
    TEST_ASSERT(backend, "realtime backend should be created");
    TEST_ASSERT(manager.register_realtime_executor("snapshot", std::move(backend)),
                "realtime backend should register");

    auto snapshot = manager.get_realtime_executor_snapshot("snapshot");
    TEST_ASSERT(snapshot, "snapshot should retain the registered backend");

    manager.shutdown(false);
    TEST_ASSERT(!manager.get_realtime_executor_snapshot("snapshot"),
                "shutdown should remove the registry entry");
    TEST_ASSERT(!snapshot->get_status().is_running,
                "snapshot must remain safe to inspect after registry removal");

    auto late_backend = manager.create_realtime_executor("late", realtime_config());
    TEST_ASSERT(late_backend, "late realtime backend should be created");
    TEST_ASSERT(!manager.register_realtime_executor("late", std::move(late_backend)),
                "shutdown must seal named registries against new registrations");
    return true;
}

bool test_facade_push_races_with_shutdown() {
    Executor executor;
    TEST_ASSERT(executor.register_realtime_task("control", realtime_config()),
                "realtime backend should register");
    TEST_ASSERT(executor.start_realtime_task("control"),
                "realtime backend should start");

    std::atomic<bool> keep_pushing{true};
    std::thread producer([&] {
        while (keep_pushing.load(std::memory_order_acquire)) {
            (void)executor.try_push_realtime_task("control", [] {});
        }
    });

    std::this_thread::sleep_for(10ms);
    executor.shutdown(false);
    keep_pushing.store(false, std::memory_order_release);
    producer.join();
    return true;
}

}  // namespace

int main() {
    return test_snapshot_survives_registry_removal() &&
                   test_facade_push_races_with_shutdown()
               ? 0
               : 1;
}
