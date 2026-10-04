#include <atomic>
#include <chrono>
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

    std::atomic<int> runs{0};
    auto handle = ex.submit_periodic(std::chrono::milliseconds(20).count(), [&runs]() {
        auto current = runs.fetch_add(1) + 1;
        if (current % 3 == 0) {
            throw std::runtime_error("periodic demo failure");
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(140));

    auto status = ex.get_periodic_task_status(handle.id());
    if (status) {
        std::cout << "periodic executions=" << status->execution_count
                  << ", failures=" << status->failed_count
                  << ", consecutive_failures="
                  << status->consecutive_failure_count << "\n";
        if (!status->last_error_message.empty()) {
            std::cout << "last error: " << status->last_error_message << "\n";
        }
    }

    auto failures = ex.get_failure_status();
    std::cout << "facade task exceptions="
              << failures.task_exception_count << "\n";

    handle.cancel();
    ex.shutdown();
    return 0;
}
