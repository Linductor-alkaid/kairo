#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

#include <kairo/executor.hpp>

using namespace std::chrono_literals;

int main() {
    kairo::Executor executor;
    kairo::ExecutorConfig config;
    config.min_threads = 1;
    config.max_threads = 1;
    if (!executor.initialize_ex(config)) {
        return 1;
    }

    auto retry = executor.submit_delayed(1, [] { return "retry complete"; });
    std::atomic<int> health_checks{0};
    const auto task_id = executor.submit_periodic(5, [&] { ++health_checks; });

    // 有界等待第一次健康检查完成：不依赖固定睡眠时长，调度慢的机器上也能等到。
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    auto status = executor.get_periodic_task_status(task_id);
    while ((!status || status->execution_count == 0) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
        status = executor.get_periodic_task_status(task_id);
    }
    const bool cancelled = executor.cancel_task(task_id);

    std::cout << retry.get() << '\n';
    std::cout << "health checks=" << health_checks.load() << '\n';
    std::cout << "periodic status=" << (status && status->execution_count > 0 ? "running" : "missing")
              << ", cancelled=" << (cancelled ? "yes" : "no") << '\n';

    executor.shutdown();
    return status && status->execution_count > 0 && cancelled ? 0 : 1;
}
