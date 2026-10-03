#include <atomic>
#include <functional>
#include <iostream>
#include <vector>

#include <kairo/executor.hpp>

int main() {
    kairo::Executor executor;
    kairo::ExecutorConfig config;
    config.min_threads = 2;
    config.max_threads = 2;
    if (!executor.initialize(config)) {
        return 1;
    }

    std::atomic<int> processed{0};
    std::vector<std::function<void()>> tasks;
    for (int index = 0; index < 3; ++index) {
        tasks.push_back([&] { ++processed; });
    }

    auto futures = executor.submit_batch(tasks);
    for (auto& future : futures) {
        future.get();
    }

    executor.submit_batch_no_future(tasks);
    const bool completed = executor.wait_for_completion_for(std::chrono::seconds(1));
    std::cout << "batch processed=" << processed.load()
              << ", completed=" << (completed ? "yes" : "no") << '\n';

    executor.shutdown();
    return completed && processed == 6 ? 0 : 1;
}
