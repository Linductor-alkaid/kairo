#include <chrono>
#include <exception>
#include <future>
#include <iostream>
#include <stdexcept>

#include <kairo/executor.hpp>

const char* lifecycle_name(kairo::ExecutorLifecycleState state) {
    using kairo::ExecutorLifecycleState;
    switch (state) {
    case ExecutorLifecycleState::Created: return "Created";
    case ExecutorLifecycleState::Initializing: return "Initializing";
    case ExecutorLifecycleState::Running: return "Running";
    case ExecutorLifecycleState::Draining: return "Draining";
    case ExecutorLifecycleState::Stopped: return "Stopped";
    case ExecutorLifecycleState::Failed: return "Failed";
    }
    return "Unknown";
}

int main() {
    kairo::Executor executor;

    kairo::ExecutorConfig config;
    config.min_threads = 1;
    config.max_threads = 1;
    config.enable_monitoring = true;

    const auto initialized = executor.initialize(config);
    if (!initialized) {
        std::cerr << "initialize failed: " << initialized.message << "\n";
        return 1;
    }

    std::promise<void> release_running_task;
    const auto release = release_running_task.get_future().share();
    std::promise<void> running_task_started;
    const auto started = running_task_started.get_future();

    auto running = executor.submit([release, &running_task_started] {
        running_task_started.set_value();
        release.wait();
    });
    started.wait();

    auto queued = executor.submit([] {});
    const auto busy_snapshot = executor.get_snapshot();
    std::cout << "busy lifecycle=" << lifecycle_name(busy_snapshot.lifecycle)
              << ", active=" << busy_snapshot.active_task_count
              << ", queued=" << busy_snapshot.queued_task_count
              << ", in_flight=" << busy_snapshot.in_flight_count << "\n";

    release_running_task.set_value();
    running.get();
    queued.get();

    auto failed = executor.submit([] {
        throw std::runtime_error("example task failure");
    });
    try {
        failed.get();
    } catch (const std::exception& error) {
        std::cout << "observed task exception: " << error.what() << "\n";
    }

    (void)executor.wait_for_completion(std::chrono::seconds{300});
    std::cout << executor.get_snapshot_text();

    executor.shutdown();
    const auto stopped_snapshot = executor.get_snapshot();
    std::cout << "after shutdown lifecycle=" << lifecycle_name(stopped_snapshot.lifecycle)
              << ", partial=" << (stopped_snapshot.partial ? "true" : "false") << "\n";
    return stopped_snapshot.lifecycle == kairo::ExecutorLifecycleState::Stopped ? 0 : 1;
}
