#include <atomic>
#include <exception>
#include <iostream>
#include <stdexcept>

#include <kairo/executor.hpp>

int main() {
    auto& executor = kairo::Executor::instance();
    std::atomic<int> callbacks{0};
    executor.set_failure_callback([&](const kairo::ExecutorFailureEvent&) { ++callbacks; });

    auto failed = executor.submit([]() -> int {
        throw std::runtime_error("expected observability failure");
    });

    try {
        static_cast<void>(failed.get());
    } catch (const std::exception&) {
    }

    (void)executor.wait_for_completion(std::chrono::seconds{300});
    const auto status = executor.get_failure_status();
    const auto recent = executor.get_recent_failures();
    std::cout << "failures=" << status.task_exception_count
              << ", callback=" << callbacks.load()
              << ", recent=" << recent.size() << '\n';

    executor.clear_recent_failures();
    executor.shutdown();
    return status.task_exception_count == 1 && callbacks == 1 && recent.size() == 1 ? 0 : 1;
}
