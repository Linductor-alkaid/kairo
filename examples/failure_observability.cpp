#include <chrono>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <thread>

#include <kairo/executor.hpp>

int main() {
    kairo::Executor ex;

    kairo::ExecutorConfig config;
    config.min_threads = 1;
    config.max_threads = 1;

    auto init = ex.initialize(config);
    if (!init) {
        std::cerr << "initialize failed: " << init.message << "\n";
        return 1;
    }

    ex.set_failure_callback([](const kairo::ExecutorFailureEvent& event) {
        std::cerr << "failure kind=" << static_cast<int>(event.kind)
                  << " executor=" << event.executor_name
                  << " task=" << event.task_id
                  << " message=" << event.message << "\n";
    });

    auto ok = ex.submit([]() { return 42; });
    auto failed = ex.submit([]() -> int {
        throw std::runtime_error("demo task failed");
    });

    std::cout << "ok result: " << ok.get() << "\n";
    try {
        (void)failed.get();
    } catch (const std::exception& e) {
        std::cout << "future observed exception: " << e.what() << "\n";
    }

    auto wait = ex.wait_for_completion(std::chrono::seconds(1));
    std::cout << "wait completed: " << (wait.completed ? "yes" : "no")
              << ", pending=" << wait.status.pending_tasks << "\n";

    auto status = ex.get_failure_status();
    std::cout << "task_exception_count=" << status.task_exception_count
              << ", submit_rejected_count=" << status.submit_rejected_count
              << ", total_count=" << status.total_count << "\n";

    for (const auto& event : ex.get_recent_failures()) {
        std::cout << "recent failure: " << event.message << "\n";
    }

    ex.shutdown();
    return 0;
}
