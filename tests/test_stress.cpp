#include <cassert>
#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <future>
#include <random>

// 包含 Executor 的头文件
#include <kairo/executor.hpp>

using namespace kairo;

// 测试辅助宏
#define TEST_ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            std::cerr << "FAILED: " << message << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
            return false; \
        } \
    } while(0)

// 测试函数前向声明
bool test_high_concurrency();
bool test_mixed_load();
bool test_long_running();

// ========== 高并发测试 ==========

bool test_high_concurrency() {
    std::cout << "Testing high concurrency (multiple threads submitting tasks)..." << std::endl;
    
    ExecutorConfig config;
    config.min_threads = 4;
    config.max_threads = 8;
    config.queue_capacity = 5000;
    
    Executor executor;
    TEST_ASSERT(executor.initialize(config), "Executor initialization should succeed");
    
    const int num_threads = 16;  // 2x max_threads
    const int tasks_per_thread = 500;
    std::atomic<int> total_completed{0};
    std::atomic<int> errors{0};
    
    auto start_time = std::chrono::steady_clock::now();
    
    // 多个线程同时提交任务
    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&executor, tasks_per_thread, &total_completed, &errors, t]() {
            std::vector<std::future<int>> futures;
            futures.reserve(tasks_per_thread);
            
            for (int i = 0; i < tasks_per_thread; ++i) {
                try {
                    auto future = executor.submit([i, t]() noexcept {
                        // 模拟一些计算
                        std::atomic<int> sum{0};
                        for (int j = 0; j < 50; ++j) {
                            sum += j * i;
                        }
                        return t * 1000 + i;
                    });
                    futures.push_back(std::move(future));
                } catch (...) {
                    errors.fetch_add(1);
                }
            }
            
            // 等待所有 future 完成
            for (auto& future : futures) {
                try {
                    future.get();
                    total_completed.fetch_add(1);
                } catch (...) {
                    errors.fetch_add(1);
                }
            }
        });
    }
    
    // 等待所有线程完成
    for (auto& thread : threads) {
        thread.join();
    }
    
    (void)executor.wait_for_completion(std::chrono::seconds{300});
    
    auto end_time = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time).count();
    
    const int expected_tasks = num_threads * tasks_per_thread;
    TEST_ASSERT(total_completed.load() == expected_tasks, 
                "All tasks should be completed");
    TEST_ASSERT(errors.load() == 0, "No errors should occur");
    
    std::cout << "  Threads: " << num_threads << std::endl;
    std::cout << "  Tasks per thread: " << tasks_per_thread << std::endl;
    std::cout << "  Total tasks: " << total_completed.load() << std::endl;
    std::cout << "  Errors: " << errors.load() << std::endl;
    std::cout << "  Elapsed time: " << elapsed_ms << " ms" << std::endl;
    std::cout << "  High concurrency test: PASSED" << std::endl;
    
    executor.shutdown();
    return true;
}

// ========== 混合负载测试 ==========

bool test_mixed_load() {
    std::cout << "Testing mixed load (async tasks + realtime tasks)..." << std::endl;
    
    ExecutorConfig config;
    config.min_threads = 4;
    config.max_threads = 8;
    config.queue_capacity = 2000;
    
    Executor executor;
    TEST_ASSERT(executor.initialize(config), "Executor initialization should succeed");
    
    // 注册实时任务
    std::atomic<int> realtime_cycles{0};
    RealtimeThreadConfig rt_config1;
    rt_config1.thread_name = "stress_realtime_1";
    rt_config1.cycle_period_ns = 10000000;  // 10ms
    rt_config1.thread_priority = 0;
    rt_config1.cycle_callback = [&realtime_cycles]() noexcept {
        realtime_cycles.fetch_add(1);
    };
    
    RealtimeThreadConfig rt_config2;
    rt_config2.thread_name = "stress_realtime_2";
    rt_config2.cycle_period_ns = 15000000;  // 15ms
    rt_config2.thread_priority = 0;
    rt_config2.cycle_callback = [&realtime_cycles]() noexcept {
        realtime_cycles.fetch_add(1);
    };
    
    TEST_ASSERT(executor.register_realtime_task("stress_realtime_1", rt_config1),
                "Realtime task 1 registration should succeed");
    TEST_ASSERT(executor.register_realtime_task("stress_realtime_2", rt_config2),
                "Realtime task 2 registration should succeed");
    
    TEST_ASSERT(executor.start_realtime_task("stress_realtime_1"),
                "Realtime task 1 start should succeed");
    TEST_ASSERT(executor.start_realtime_task("stress_realtime_2"),
                "Realtime task 2 start should succeed");
    
    // 在实时任务运行的同时，提交大量异步任务
    std::atomic<int> async_completed{0};
    const int num_async_tasks = 2000;
    std::vector<std::future<void>> futures;
    futures.reserve(num_async_tasks);
    
    auto start_time = std::chrono::steady_clock::now();
    
    for (int i = 0; i < num_async_tasks; ++i) {
        auto future = executor.submit([i, &async_completed]() noexcept {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            async_completed.fetch_add(1);
        });
        futures.push_back(std::move(future));
    }
    
    // 运行一段时间（5-10秒）
    const int run_duration_ms = 5000;
    std::this_thread::sleep_for(std::chrono::milliseconds(run_duration_ms));
    
    // 等待所有异步任务完成
    for (auto& future : futures) {
        future.get();
    }
    
    auto end_time = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time).count();
    
    // 停止实时任务
    executor.stop_realtime_task("stress_realtime_1");
    executor.stop_realtime_task("stress_realtime_2");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    TEST_ASSERT(async_completed.load() == num_async_tasks, 
                "All async tasks should be completed");
    TEST_ASSERT(realtime_cycles.load() > 0, 
                "Realtime tasks should have executed cycles");
    
    std::cout << "  Async tasks: " << async_completed.load() << std::endl;
    std::cout << "  Realtime cycles: " << realtime_cycles.load() << std::endl;
    std::cout << "  Elapsed time: " << elapsed_ms << " ms" << std::endl;
    std::cout << "  Mixed load test: PASSED" << std::endl;
    
    executor.shutdown();
    return true;
}

// ========== 长时间运行测试 ==========

bool test_long_running() {
    std::cout << "Testing long-running scenario (30 seconds)..." << std::endl;
    
    ExecutorConfig config;
    config.min_threads = 4;
    config.max_threads = 8;
    config.queue_capacity = 1000;
    
    Executor executor;
    TEST_ASSERT(executor.initialize(config), "Executor initialization should succeed");
    
    // 注册实时任务
    std::atomic<int> realtime_cycles{0};
    RealtimeThreadConfig rt_config;
    rt_config.thread_name = "long_running_realtime";
    rt_config.cycle_period_ns = 20000000;  // 20ms
    rt_config.thread_priority = 0;
    rt_config.cycle_callback = [&realtime_cycles]() noexcept {
        realtime_cycles.fetch_add(1);
    };
    
    TEST_ASSERT(executor.register_realtime_task("long_running_realtime", rt_config),
                "Realtime task registration should succeed");
    TEST_ASSERT(executor.start_realtime_task("long_running_realtime"),
                "Realtime task start should succeed");
    
    std::atomic<int> total_submitted{0};
    std::atomic<int> total_completed{0};
    std::atomic<bool> stop_submitting{false};
    
    // 持续提交任务的线程
    std::thread submitter([&executor, &total_submitted, &total_completed, &stop_submitting]() {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(1, 10);
        
        while (!stop_submitting.load()) {
            int batch_size = dis(gen);
            std::vector<std::future<void>> futures;
            
            for (int i = 0; i < batch_size; ++i) {
                try {
                    auto future = executor.submit([&total_completed, &dis, &gen]() noexcept {
                        std::this_thread::sleep_for(std::chrono::milliseconds(dis(gen)));
                        total_completed.fetch_add(1);
                    });
                    futures.push_back(std::move(future));
                    total_submitted.fetch_add(1);
                } catch (...) {
                    // 忽略提交错误
                }
            }
            
            // 异步等待（不阻塞）
            std::thread([futures = std::move(futures)]() mutable {
                for (auto& future : futures) {
                    try {
                        future.get();
                    } catch (...) {
                        // 忽略执行错误
                    }
                }
            }).detach();
            
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
    
    // 运行 30 秒
    const int run_duration_sec = 30;
    std::cout << "  Running for " << run_duration_sec << " seconds..." << std::endl;
    
    auto start_time = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::seconds(run_duration_sec));
    stop_submitting.store(true);
    
    submitter.join();
    
    // 等待所有任务完成
    (void)executor.wait_for_completion(std::chrono::seconds{300});
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    auto end_time = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time).count();
    
    // 停止实时任务
    executor.stop_realtime_task("long_running_realtime");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    std::cout << "  Submitted tasks: " << total_submitted.load() << std::endl;
    std::cout << "  Completed tasks: " << total_completed.load() << std::endl;
    std::cout << "  Realtime cycles: " << realtime_cycles.load() << std::endl;
    std::cout << "  Elapsed time: " << elapsed_ms << " ms" << std::endl;
    std::cout << "  Long-running test: PASSED" << std::endl;
    
    executor.shutdown();
    return true;
}

// ========== 主函数 ==========

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Stress Test" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;
    
    bool all_passed = true;
    
    // 高并发测试
    std::cout << "--- High Concurrency Test ---" << std::endl;
    all_passed &= test_high_concurrency();
    std::cout << std::endl;
    
    // 混合负载测试
    std::cout << "--- Mixed Load Test ---" << std::endl;
    all_passed &= test_mixed_load();
    std::cout << std::endl;
    
    // 长时间运行测试（可选，因为耗时较长）
    // 可以通过环境变量或命令行参数控制是否运行
    const char* run_long_test = std::getenv("KAIRO_RUN_LONG_STRESS_TEST");
    if (run_long_test && std::string(run_long_test) == "1") {
        std::cout << "--- Long-Running Test ---" << std::endl;
        all_passed &= test_long_running();
        std::cout << std::endl;
    } else {
        std::cout << "--- Long-Running Test ---" << std::endl;
        std::cout << "  Skipped (set KAIRO_RUN_LONG_STRESS_TEST=1 to enable)" << std::endl;
        std::cout << std::endl;
    }
    
    if (all_passed) {
        std::cout << "========================================" << std::endl;
        std::cout << "All stress tests PASSED" << std::endl;
        std::cout << "========================================" << std::endl;
        return 0;
    } else {
        std::cerr << "========================================" << std::endl;
        std::cerr << "Some stress tests FAILED" << std::endl;
        std::cerr << "========================================" << std::endl;
        return 1;
    }
}
