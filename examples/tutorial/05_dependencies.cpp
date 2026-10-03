#include <iostream>

#include <kairo/executor.hpp>

int main() {
    kairo::Executor executor;
    kairo::ExecutorConfig config;
    config.min_threads = 2;
    config.max_threads = 2;
    if (!executor.initialize(config)) {
        return 1;
    }

    auto load = executor.submit_with_handle([] { return 20; });
    auto sense = executor.submit_with_handle([] { return 22; });
    const auto prerequisites = executor.when_all({load.handle, sense.handle});
    auto plan = executor.submit_after(prerequisites, [&] {
        return load.future.get() + sense.future.get();
    });

    const int result = plan.get();
    std::cout << "plan score=" << result << '\n';
    executor.shutdown();
    return result == 42 ? 0 : 1;
}
