#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <atomic>
#include <kairo/executor.hpp>

using namespace kairo;

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Executor Basic Submit Example" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;

    // 初始化 Executor（使用单例模式）
    ExecutorConfig config;
    config.min_threads = 4;
    config.max_threads = 8;
    config.queue_capacity = 1000;

    auto& executor = Executor::instance();
    if (!executor.initialize(config)) {
        std::cerr << "Failed to initialize executor" << std::endl;
        return 1;
    }

    std::cout << "Executor initialized successfully" << std::endl;
    std::cout << std::endl;

    // ========== 示例 1: 基本任务提交 ==========
    std::cout << "Example 1: Basic task submission" << std::endl;
    {
        auto future = executor.submit([]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            return 42;
        });

        int result = future.get();
        std::cout << "  Task result: " << result << std::endl;
    }
    std::cout << std::endl;

    // ========== 示例 2: 带参数的任务提交 ==========
    std::cout << "Example 2: Task submission with parameters" << std::endl;
    {
        auto future = executor.submit([](int a, int b) {
            return a + b;
        }, 10, 20);

        int result = future.get();
        std::cout << "  10 + 20 = " << result << std::endl;
    }
    std::cout << std::endl;

    // ========== 示例 3: 优先级任务提交 ==========
    std::cout << "Example 3: Priority task submission" << std::endl;
    {
        std::vector<int> execution_order;
        std::mutex order_mutex;

        // 提交低优先级任务
        executor.submit_priority(0, [&execution_order, &order_mutex]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            std::lock_guard<std::mutex> lock(order_mutex);
            execution_order.push_back(0);
            std::cout << "  Low priority task executed" << std::endl;
        });

        // 提交高优先级任务
        executor.submit_priority(2, [&execution_order, &order_mutex]() {
            std::lock_guard<std::mutex> lock(order_mutex);
            execution_order.push_back(2);
            std::cout << "  High priority task executed" << std::endl;
        });

        // 等待任务完成
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::cout << std::endl;

    // ========== 示例 4: 延迟任务提交 ==========
    std::cout << "Example 4: Delayed task submission" << std::endl;
    {
        auto start_time = std::chrono::steady_clock::now();

        auto submission = executor.submit_delayed(200, []() {
            return std::string("Delayed task executed");
        });

        std::string result = submission.future.get();
        auto end_time = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            end_time - start_time).count();

        std::cout << "  " << result << std::endl;
        std::cout << "  Elapsed time: " << elapsed << " ms" << std::endl;
    }
    std::cout << std::endl;

    // ========== 示例 5: 周期性任务提交 ==========
    std::cout << "Example 5: Periodic task submission" << std::endl;
    {
        std::atomic<int> counter(0);

        auto periodic = executor.submit_periodic(100, [&counter]() {
            int count = counter.fetch_add(1) + 1;
            std::cout << "  Periodic task execution #" << count << std::endl;
        });

        std::cout << "  Periodic task started (ID: " << periodic.id() << ")" << std::endl;

        // 运行几个周期
        std::this_thread::sleep_for(std::chrono::milliseconds(350));

        // 取消周期性任务
        periodic.cancel();
        std::cout << "  Periodic task cancelled" << std::endl;
    }
    std::cout << std::endl;

    // ========== 示例 6: 并发任务提交 ==========
    std::cout << "Example 6: Concurrent task submission" << std::endl;
    {
        const int num_tasks = 10;
        std::vector<std::future<int>> futures;

        for (int i = 0; i < num_tasks; ++i) {
            auto future = executor.submit([i]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                return i * i;
            });
            futures.push_back(std::move(future));
        }

        // 等待所有任务完成并收集结果
        int sum = 0;
        for (auto& future : futures) {
            sum += future.get();
        }

        std::cout << "  Submitted " << num_tasks << " tasks" << std::endl;
        std::cout << "  Sum of results: " << sum << std::endl;
    }
    std::cout << std::endl;

    // ========== 示例 7: 监控查询 ==========
    std::cout << "Example 7: Monitor queries" << std::endl;
    {
        // 提交一些任务
        for (int i = 0; i < 20; ++i) {
            executor.submit([i]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                return i;
            });
        }

        // 等待任务完成
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // 查询执行器状态
        auto status = executor.get_async_executor_status();
        std::cout << "  Executor name: " << status.name << std::endl;
        std::cout << "  Is running: " << (status.is_running ? "true" : "false") << std::endl;
        std::cout << "  Completed tasks: " << status.completed_tasks << std::endl;
        std::cout << "  Failed tasks: " << status.failed_tasks << std::endl;
        std::cout << "  Queue size: " << status.queue_size << std::endl;
        std::cout << "  Average task time: " << status.avg_task_time_ms << " ms" << std::endl;
    }
    std::cout << std::endl;

    // ========== 示例 8: 实时任务管理 ==========
    std::cout << "Example 8: Realtime task management" << std::endl;
    {
        // 注册实时任务
        RealtimeThreadConfig rt_config;
        rt_config.thread_name = "example_realtime";
        rt_config.cycle_period_ns = 50000000;  // 50ms
        rt_config.thread_priority = 0;  // 普通优先级（示例环境）
        rt_config.cycle_callback = []() {
            static int cycle_count = 0;
            if (++cycle_count <= 3) {
                std::cout << "  Realtime cycle #" << cycle_count << std::endl;
            }
        };

        if (executor.register_realtime_task("example_realtime", rt_config)) {
            std::cout << "  Realtime task registered" << std::endl;

            // 启动实时任务
            if (executor.start_realtime_task("example_realtime")) {
                std::cout << "  Realtime task started" << std::endl;

                // 运行一段时间
                std::this_thread::sleep_for(std::chrono::milliseconds(200));

                // 查询状态
                auto status = executor.get_realtime_executor_status("example_realtime");
                std::cout << "  Realtime task status:" << std::endl;
                std::cout << "    Name: " << status.name << std::endl;
                std::cout << "    Is running: " << (status.is_running ? "true" : "false") << std::endl;
                std::cout << "    Cycle count: " << status.cycle_count << std::endl;

                // 停止实时任务
                executor.stop_realtime_task("example_realtime");
                std::cout << "  Realtime task stopped" << std::endl;
            }
        }
    }
    std::cout << std::endl;

    // 关闭 Executor
    executor.shutdown();
    std::cout << "Executor shut down" << std::endl;

    std::cout << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "Example completed successfully!" << std::endl;
    std::cout << "========================================" << std::endl;

    return 0;
}
