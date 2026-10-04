#include <exception>
#include <iostream>
#include <stdexcept>

#include <kairo/executor.hpp>

int main() {
    auto& executor = kairo::Executor::instance();

    auto answer = executor.submit_auto([] { return 42; });
    std::cout << "answer=" << answer.get() << '\n';

    const auto decision = executor.get_last_routing_decision();
    if (!decision || decision->selected_backend != kairo::ExecutionBackend::DefaultAsync) {
        std::cerr << "unexpected routing decision\n";
        executor.shutdown();
        return 1;
    }

    auto failing_task = executor.submit_auto([]() -> int {
        throw std::runtime_error("expected tutorial failure");
    });

    try {
        static_cast<void>(failing_task.get());
    } catch (const std::exception& error) {
        std::cout << "task failed: " << error.what() << '\n';
    }

    executor.shutdown();
    return 0;
}
