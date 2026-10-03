#include <kairo/executor.hpp>
#include <kairo/lockfree_task_executor.hpp>
#include <iostream>
#include <iomanip>

using namespace kairo;

void demo_task_monitor_sampling() {
    std::cout << "=== Task Monitor Sampling Demo ===\n\n";

    auto& ex = Executor::instance();
    ExecutorConfig config;
    config.min_threads = 2;
    config.max_threads = 4;
    config.enable_monitoring = true;
    ex.initialize(config);

    // 默认 100% 采样
    std::cout << "1. Full sampling (100%):\n";
    for (int i = 0; i < 1000; ++i) {
        ex.submit([]() { /* work */ });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto stats = ex.get_task_statistics("default");
    std::cout << "   Recorded tasks: " << stats.total_count << "/1000\n\n";

    // 设置 1% 采样率
    std::cout << "2. Sampled monitoring (1%):\n";
    ex.set_monitoring_sampling_rate(0.01);

    for (int i = 0; i < 10000; ++i) {
        ex.submit([]() { /* work */ });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    stats = ex.get_task_statistics("default");
    std::cout << "   Recorded tasks: ~" << stats.total_count << "/10000 (expected ~100)\n";
    std::cout << "   Overhead reduced by ~99%\n\n";

    ex.shutdown();
}

void demo_lockfree_queue_stats() {
    std::cout << "=== LockFree Queue Stats Demo ===\n\n";

    // 启用统计
    LockFreeTaskExecutor executor(1024, 2, true);
    executor.start();

    std::cout << "1. Single task submission:\n";
    for (int i = 0; i < 500; ++i) {
        executor.push_task([]() {
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto stats = executor.get_queue_stats();
    std::cout << "   Total pushes: " << stats.total_pushes << "\n";
    std::cout << "   Failed pushes: " << stats.failed_pushes << "\n";
    std::cout << "   Success rate: " << std::fixed << std::setprecision(2)
              << (stats.success_rate * 100) << "%\n";
    std::cout << "   Peak queue size: " << stats.peak_size << "\n\n";

    std::cout << "2. Batch submission:\n";
    std::function<void()> tasks[100];
    for (int i = 0; i < 100; ++i) {
        tasks[i] = []() { /* work */ };
    }

    size_t pushed;
    executor.push_tasks_batch(tasks, 100, pushed);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    stats = executor.get_queue_stats();
    std::cout << "   Batch operations: " << stats.batch_pushes << " pushes, "
              << stats.batch_pops << " pops\n";
    std::cout << "   Total operations: " << stats.total_pushes << " pushes, "
              << stats.total_pops << " pops\n\n";

    executor.stop();
}

int main() {
    demo_task_monitor_sampling();
    demo_lockfree_queue_stats();

    std::cout << "Done!\n";
    return 0;
}
