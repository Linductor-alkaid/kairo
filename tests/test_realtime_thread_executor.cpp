#include <cassert>
#include <cmath>
#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <future>
#include <algorithm>
#include <string>
#include <ctime>
#include <system_error>

// 包含 RealtimeThreadExecutor 的头文件
#include <kairo/config.hpp>
#include <kairo/types.hpp>
#include <kairo/interfaces.hpp>
#define private public
#include "kairo/realtime_thread_executor.hpp"
#undef private

using namespace kairo;

static_assert(std::atomic<int64_t>::is_always_lock_free,
              "RealtimeThreadExecutor statistics require lock-free int64_t atomics");

// 测试辅助宏
#define TEST_ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            std::cerr << "FAILED: " << message << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
            return false; \
        } \
    } while(0)

// ========== 基本功能测试 ==========

bool test_realtime_executor_basic() {
    std::cout << "Testing RealtimeThreadExecutor basic operations..." << std::endl;
    
    // 创建配置
    RealtimeThreadConfig config;
    config.thread_name = "test_realtime_thread";
    config.cycle_period_ns = 10000000;  // 10ms
    config.thread_priority = 0;  // 普通优先级
    config.cycle_callback = []() {
        // 空回调
    };
    
    // 创建执行器
    RealtimeThreadExecutor executor("test_executor", config);
    
    // 测试获取名称
    TEST_ASSERT(executor.get_name() == "test_executor", "Executor name should match");
    
    // 测试状态（启动前应该不是运行状态）
    auto status = executor.get_status();
    TEST_ASSERT(status.name == "test_executor", "Status name should match");
    TEST_ASSERT(status.is_running == false, "Executor should not be running before start");
    TEST_ASSERT(status.cycle_period_ns == 10000000, "Cycle period should match");
    TEST_ASSERT(status.cycle_count == 0, "Cycle count should be 0 initially");
    
    // 测试启动
    TEST_ASSERT(executor.start(), "Executor should start successfully");
    
    // 等待一小段时间让线程启动
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // 测试状态（启动后应该是运行状态）
    status = executor.get_status();
    TEST_ASSERT(status.is_running == true, "Executor should be running after start");
    
    // 测试停止
    executor.stop();
    
    // 等待线程结束
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // 测试状态（停止后应该不是运行状态）
    status = executor.get_status();
    TEST_ASSERT(status.is_running == false, "Executor should not be running after stop");
    
    std::cout << "  RealtimeThreadExecutor basic operations: PASSED" << std::endl;
    return true;
}

bool test_realtime_executor_double_start() {
    std::cout << "Testing RealtimeThreadExecutor double start..." << std::endl;
    
    RealtimeThreadConfig config;
    config.thread_name = "test_thread";
    config.cycle_period_ns = 10000000;  // 10ms
    config.cycle_callback = []() {};
    
    RealtimeThreadExecutor executor("test_executor", config);
    
    // 第一次启动应该成功
    TEST_ASSERT(executor.start(), "First start should succeed");
    
    // 等待线程启动
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // 第二次启动应该失败
    TEST_ASSERT(!executor.start(), "Second start should fail");
    
    executor.stop();
    
    std::cout << "  Double start: PASSED" << std::endl;
    return true;
}

bool test_realtime_start_thread_creation_failure_rolls_back() {
    std::cout << "Testing RealtimeThreadExecutor start thread creation failure rollback..." << std::endl;

    RealtimeThreadConfig config;
    config.thread_name = "test_thread_create_fail";
    config.cycle_period_ns = 10000000;  // 10ms
    config.cycle_callback = []() {};

    RealtimeThreadExecutor executor("test_executor", config);
    executor.thread_factory_ = [](std::function<void()>) -> std::thread {
        throw std::system_error(
            std::make_error_code(std::errc::resource_unavailable_try_again),
            "test thread creation failure");
    };

    TEST_ASSERT(!executor.start(), "start() should return false when thread creation fails");

    auto status = executor.get_status();
    TEST_ASSERT(!status.is_running, "Executor should roll running state back after failed start");

    executor.thread_factory_ = [](std::function<void()> entry) {
        return std::thread(std::move(entry));
    };

    TEST_ASSERT(executor.start(), "Subsequent start should be retryable and succeed");
    status = executor.get_status();
    TEST_ASSERT(status.is_running, "Executor should be running after retry succeeds");

    executor.stop();

    std::cout << "  Start thread creation failure rollback: PASSED" << std::endl;
    return true;
}

// ========== 周期循环测试 ==========

bool test_realtime_executor_cycle_loop() {
    std::cout << "Testing RealtimeThreadExecutor cycle loop..." << std::endl;
    
    std::atomic<int> cycle_count(0);
    const int64_t period_ns = 20000000;  // 20ms
    
    RealtimeThreadConfig config;
    config.thread_name = "test_thread";
    config.cycle_period_ns = period_ns;
    config.cycle_callback = [&cycle_count]() {
        cycle_count.fetch_add(1);
    };
    
    RealtimeThreadExecutor executor("test_executor", config);
    TEST_ASSERT(executor.start(), "Executor should start successfully");
    
    // 等待几个周期
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // 检查周期计数
    int count = cycle_count.load();
    TEST_ASSERT(count > 0, "Cycle callback should be called");
    TEST_ASSERT(count >= 4, "Should have at least 4 cycles in 100ms (20ms period)");
    
    // 检查状态中的周期计数
    // 注意：由于时序问题，状态中的周期计数可能与回调计数不完全一致
    // 在 Windows 上，由于线程调度精度问题，允许一定的差异
    auto status = executor.get_status();
    TEST_ASSERT(status.cycle_count > 0, "Status cycle count should be greater than 0");
    // 允许状态计数与回调计数有 ±1 的差异（由于时序问题）
    int64_t count_diff = (status.cycle_count > static_cast<int64_t>(count)) ? 
                         (status.cycle_count - static_cast<int64_t>(count)) : 
                         (static_cast<int64_t>(count) - status.cycle_count);
    TEST_ASSERT(count_diff <= 1, 
                "Status cycle count should be approximately equal to actual count (diff: " + 
                std::to_string(count_diff) + ")");
    
    executor.stop();
    
    std::cout << "  Cycle loop: PASSED (count: " << count << ")" << std::endl;
    return true;
}

bool test_realtime_executor_cycle_period_accuracy() {
    std::cout << "Testing RealtimeThreadExecutor cycle period accuracy..." << std::endl;
    
    std::vector<std::chrono::steady_clock::time_point> cycle_times;
    const int64_t period_ns = 10000000;  // 10ms
    const int num_cycles = 10;
    
    RealtimeThreadConfig config;
    config.thread_name = "test_thread";
    config.cycle_period_ns = period_ns;
    config.cycle_callback = [&cycle_times]() {
        cycle_times.push_back(std::chrono::steady_clock::now());
    };
    
    RealtimeThreadExecutor executor("test_executor", config);
    TEST_ASSERT(executor.start(), "Executor should start successfully");
    
    // 等待足够的周期
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    
    executor.stop();
    
    // 检查周期时间间隔
    // 注意：这是一个精度测试，用于验证周期控制的准确性
    // 在Windows上，即使使用了高精度定时器，系统调度仍可能影响精度
    if (cycle_times.size() >= num_cycles) {
        int64_t expected = period_ns;
        
        // 跳过前几个间隔（系统稳定期）
        const size_t skip_initial = 2;
        
        // 计算平均间隔和标准差
        int64_t total_interval = 0;
        size_t interval_count = 0;
        std::vector<int64_t> intervals;
        
        for (size_t i = skip_initial + 1; i < cycle_times.size(); ++i) {
            auto interval = std::chrono::duration_cast<std::chrono::nanoseconds>(
                cycle_times[i] - cycle_times[i-1]
            ).count();
            intervals.push_back(interval);
            total_interval += interval;
            interval_count++;
        }
        
        if (interval_count > 0) {
            // 计算平均间隔
            int64_t avg_interval = total_interval / interval_count;
            
            // 计算平均误差百分比
            double avg_error_percent = std::abs(static_cast<double>(avg_interval - expected)) / expected * 100.0;
            
#ifdef _WIN32
            // Windows: 使用高精度定时器后，平均误差应该在30%以内
            // 允许个别间隔有较大偏差（系统调度影响），但平均值应该接近预期
            TEST_ASSERT(avg_error_percent <= 30.0,
                       "Average cycle interval error too large: " + 
                       std::to_string(avg_error_percent) + "% (expected < 30%)");
            
            // 检查至少70%的间隔在合理范围内（±50%下限，+100%上限）
            int64_t tolerance_lower = expected / 2;
            int64_t tolerance_upper = expected * 2;
            size_t valid_count = 0;
            for (auto interval : intervals) {
                if (interval >= expected - tolerance_lower && interval <= expected + tolerance_upper) {
                    valid_count++;
                }
            }
            double valid_rate = static_cast<double>(valid_count) / interval_count;
            TEST_ASSERT(valid_rate >= 0.7,
                       "Too many intervals out of tolerance: " + 
                       std::to_string(valid_count) + "/" + std::to_string(interval_count) + 
                       " within tolerance (expected >= 70%)");
#else
            // Linux: 保持平均误差<10%，但允许少量间隔因 CI 调度抖动超出严格容差
            TEST_ASSERT(avg_error_percent <= 10.0,
                       "Average cycle interval error too large: " + 
                       std::to_string(avg_error_percent) + "% (expected < 10%)");
            
            int64_t tolerance_lower = expected / 5;
            int64_t tolerance_upper = expected / 5 * 3;
            size_t valid_count = 0;
            for (auto interval : intervals) {
                if (interval >= expected - tolerance_lower && interval <= expected + tolerance_upper) {
                    valid_count++;
                }
            }
            TEST_ASSERT(valid_count * 100 >= interval_count * 80,
                       "Too many intervals out of tolerance: " + 
                       std::to_string(valid_count) + "/" + std::to_string(interval_count) + 
                       " within tolerance (expected >= 80%)");
#endif
        }
    } else {
        // 如果没有足够的周期数据，至少检查是否有周期执行
        TEST_ASSERT(cycle_times.size() > 0, "At least some cycles should be executed");
    }
    
    std::cout << "  Cycle period accuracy: PASSED" << std::endl;
    return true;
}

// ========== 任务处理测试 ==========

bool test_realtime_executor_push_task() {
    std::cout << "Testing RealtimeThreadExecutor push_task..." << std::endl;
    
    std::atomic<int> task_count(0);
    const int64_t period_ns = 10000000;  // 10ms
    
    RealtimeThreadConfig config;
    config.thread_name = "test_thread";
    config.cycle_period_ns = period_ns;
    config.cycle_callback = []() {
        // 空回调
    };
    
    RealtimeThreadExecutor executor("test_executor", config);
    TEST_ASSERT(executor.start(), "Executor should start successfully");
    
    // 推送多个任务
    const int num_tasks = 10;
    for (int i = 0; i < num_tasks; ++i) {
        executor.push_task([&task_count, i]() {
            task_count.fetch_add(1);
        });
    }
    
    // 等待任务执行（需要等待几个周期）
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    
    // 检查任务是否都执行了
    int count = task_count.load();
    TEST_ASSERT(count == num_tasks, "All tasks should be executed");
    
    executor.stop();
    
    std::cout << "  Push task: PASSED (executed " << count << " tasks)" << std::endl;
    return true;
}

bool test_realtime_executor_task_order() {
    std::cout << "Testing RealtimeThreadExecutor task order..." << std::endl;
    
    std::vector<int> execution_order;
    std::mutex order_mutex;
    const int64_t period_ns = 10000000;  // 10ms
    
    RealtimeThreadConfig config;
    config.thread_name = "test_thread";
    config.cycle_period_ns = period_ns;
    config.cycle_callback = []() {};
    
    RealtimeThreadExecutor executor("test_executor", config);
    TEST_ASSERT(executor.start(), "Executor should start successfully");
    
    // 推送多个任务，每个任务记录执行顺序
    const int num_tasks = 5;
    for (int i = 0; i < num_tasks; ++i) {
        executor.push_task([&execution_order, &order_mutex, i]() {
            std::lock_guard<std::mutex> lock(order_mutex);
            execution_order.push_back(i);
        });
    }
    
    // 等待任务执行
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    
    executor.stop();
    
    // 检查任务执行顺序（应该是 FIFO）
    TEST_ASSERT(execution_order.size() == num_tasks, "All tasks should be executed");
    for (int i = 0; i < num_tasks; ++i) {
        TEST_ASSERT(execution_order[i] == i, "Tasks should execute in order");
    }
    
    std::cout << "  Task order: PASSED" << std::endl;
    return true;
}

bool test_realtime_stop_drains_queue() {
    std::cout << "Testing RealtimeThreadExecutor stop drains queued tasks..." << std::endl;

    constexpr size_t queue_capacity = 50;
    constexpr int num_tasks = 50;
    std::atomic<int> cycle_count(0);
    std::atomic<int> task_count(0);

    RealtimeThreadConfig config;
    config.thread_name = "test_stop_drains";
    config.cycle_period_ns = 500000000;  // 500ms
    config.thread_priority = 0;
    config.cycle_callback = [&cycle_count]() {
        cycle_count.fetch_add(1, std::memory_order_relaxed);
    };

    RealtimeThreadExecutor executor("test_stop_drains", config,
                                    /*enable_stats=*/true,
                                    queue_capacity);
    TEST_ASSERT(executor.start(), "Executor should start successfully");

    for (int i = 0; i < 200; ++i) {
        if (executor.get_status().cycle_count > 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    TEST_ASSERT(executor.get_status().cycle_count > 0,
                "Executor should complete its first cycle before queuing tasks");

    // Queue tasks while the RT thread is sleeping until the next period, then stop
    // before another process_tasks() pass can execute them.
    for (int i = 0; i < num_tasks; ++i) {
        TEST_ASSERT(executor.push_task_ex([&task_count]() {
                        task_count.fetch_add(1, std::memory_order_relaxed);
                    }),
                    "Initial task push should succeed");
    }

    executor.stop();

    auto status = executor.get_status();
    TEST_ASSERT(task_count.load(std::memory_order_acquire) == 0,
                "Queued tasks should be dropped, not executed, during stop");
    TEST_ASSERT(status.dropped_task_count == static_cast<uint64_t>(num_tasks),
                "stop() should count drained queued tasks as dropped");

    // stop() 后不再接受新任务; 失败同样计入 dropped_task_count.
    for (int i = 0; i < num_tasks; ++i) {
        TEST_ASSERT(!executor.push_task_ex([]() {}),
                    "push_task_ex should reject tasks after stop");
    }

    status = executor.get_status();
    TEST_ASSERT(status.dropped_task_count == static_cast<uint64_t>(num_tasks * 2),
                "Rejected post-stop tasks should be counted as dropped");

    std::cout << "  Stop drains queue: PASSED (dropped "
              << status.dropped_task_count << " tasks)" << std::endl;
    return true;
}

bool test_realtime_concurrent_stop_does_not_double_release() {
    std::cout << "Testing RealtimeThreadExecutor concurrent stop does not double-release..." << std::endl;

    constexpr size_t queue_capacity = 64;
    constexpr int num_tasks = static_cast<int>(queue_capacity - 1);
    std::atomic<int> cycle_count(0);
    std::atomic<int> task_count(0);
    std::atomic<bool> release_stop_callers(false);

    RealtimeThreadConfig config;
    config.thread_name = "test_stop_race";
    config.cycle_period_ns = 500000000;  // 500ms
    config.thread_priority = 0;
    config.cycle_callback = [&cycle_count]() {
        cycle_count.fetch_add(1, std::memory_order_relaxed);
    };

    RealtimeThreadExecutor executor("test_stop_race", config,
                                    /*enable_stats=*/true,
                                    queue_capacity);
    TEST_ASSERT(executor.start(), "Executor should start successfully");

    for (int i = 0; i < 200; ++i) {
        if (executor.get_status().cycle_count > 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    TEST_ASSERT(executor.get_status().cycle_count > 0,
                "Executor should complete its first cycle before queuing tasks");

    for (int i = 0; i < num_tasks; ++i) {
        TEST_ASSERT(executor.push_task_ex([&task_count]() {
                        task_count.fetch_add(1, std::memory_order_relaxed);
                    }),
                    "Initial task push should succeed");
    }

    std::thread stopper1([&]() {
        while (!release_stop_callers.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        executor.stop();
    });
    std::thread stopper2([&]() {
        while (!release_stop_callers.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        executor.stop();
    });

    release_stop_callers.store(true, std::memory_order_release);
    stopper1.join();
    stopper2.join();

    auto status = executor.get_status();
    TEST_ASSERT(task_count.load(std::memory_order_acquire) == 0,
                "Queued tasks should be dropped, not executed, during concurrent stop");
    TEST_ASSERT(status.dropped_task_count == static_cast<uint64_t>(num_tasks),
                "Concurrent stop should drain each queued task exactly once");

    std::cout << "  Concurrent stop does not double-release: PASSED (dropped "
              << status.dropped_task_count << " tasks)" << std::endl;
    return true;
}

bool test_realtime_push_after_stop_returns_false_and_counts_drop() {
    std::cout << "Testing RealtimeThreadExecutor PushAfterStopReturnsFalseAndCountsDrop..." << std::endl;

    std::atomic<int> task_count(0);

    RealtimeThreadConfig config;
    config.thread_name = "test_push_after_stop";
    config.cycle_period_ns = 5000000;  // 5ms
    config.thread_priority = 0;
    config.cycle_callback = []() {};

    RealtimeThreadExecutor executor("test_push_after_stop", config,
                                    /*enable_stats=*/true,
                                    /*queue_capacity=*/64);
    TEST_ASSERT(executor.start(), "Executor should start successfully");

    for (int i = 0; i < 8; ++i) {
        TEST_ASSERT(executor.push_task_ex([&task_count]() {
                        task_count.fetch_add(1, std::memory_order_relaxed);
                    }),
                    "Push while running should succeed");
    }

    for (int i = 0; i < 100; ++i) {
        if (task_count.load(std::memory_order_acquire) == 8) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    executor.stop();

    const auto before = executor.get_status();
    TEST_ASSERT(!before.is_running, "Executor should not be running after stop");

    TEST_ASSERT(!executor.push_task_ex([]() {}),
                "push_task_ex should return false after stop");
    executor.push_task([]() {});

    const auto after = executor.get_status();
    TEST_ASSERT(after.dropped_task_count == before.dropped_task_count + 2,
                "push_task_ex and push_task should count post-stop rejects as drops");

    std::cout << "  PushAfterStopReturnsFalseAndCountsDrop: PASSED (dropped "
              << after.dropped_task_count << " tasks)" << std::endl;
    return true;
}

bool test_realtime_concurrent_stop_push_no_accepted_orphans() {
    std::cout << "Testing RealtimeThreadExecutor ConcurrentStopPushNoAcceptedOrphans..." << std::endl;

    constexpr int num_producers = 6;
    constexpr int attempts_after_stop = 64;
    std::atomic<int> executed_count(0);
    std::atomic<int> accepted_before_stop(0);
    std::atomic<int> rejected_before_stop(0);
    std::atomic<int> accepted_after_stop(0);
    std::atomic<int> rejected_after_stop(0);
    std::atomic<bool> release_producers(false);
    std::atomic<bool> stop_returned(false);

    RealtimeThreadConfig config;
    config.thread_name = "test_stop_push_race";
    config.cycle_period_ns = 1000000;  // 1ms
    config.thread_priority = 0;
    config.cycle_callback = []() {};

    RealtimeThreadExecutor executor("test_stop_push_race", config,
                                    /*enable_stats=*/true,
                                    /*queue_capacity=*/1024);
    TEST_ASSERT(executor.start(), "Executor should start successfully");

    std::vector<std::thread> producers;
    producers.reserve(num_producers);
    for (int i = 0; i < num_producers; ++i) {
        producers.emplace_back([&]() {
            while (!release_producers.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            while (!stop_returned.load(std::memory_order_acquire)) {
                const bool accepted = executor.push_task_ex([&executed_count]() {
                    executed_count.fetch_add(1, std::memory_order_relaxed);
                });
                if (accepted) {
                    accepted_before_stop.fetch_add(1, std::memory_order_relaxed);
                } else {
                    rejected_before_stop.fetch_add(1, std::memory_order_relaxed);
                }
                std::this_thread::yield();
            }

            for (int j = 0; j < attempts_after_stop; ++j) {
                const bool accepted = executor.push_task_ex([&executed_count]() {
                    executed_count.fetch_add(1, std::memory_order_relaxed);
                });
                if (accepted) {
                    accepted_after_stop.fetch_add(1, std::memory_order_relaxed);
                } else {
                    rejected_after_stop.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    release_producers.store(true, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    executor.stop();
    stop_returned.store(true, std::memory_order_release);

    for (auto& producer : producers) {
        producer.join();
    }

    const auto status = executor.get_status();
    TEST_ASSERT(!status.is_running, "Executor should remain stopped");
    TEST_ASSERT(accepted_after_stop.load(std::memory_order_acquire) == 0,
                "No push after stop() returned should be accepted");
    TEST_ASSERT(rejected_after_stop.load(std::memory_order_acquire) ==
                    num_producers * attempts_after_stop,
                "All pushes after stop() returned should be rejected");

    const int accepted_before = accepted_before_stop.load(std::memory_order_acquire);
    const int executed = executed_count.load(std::memory_order_acquire);
    TEST_ASSERT(executed <= accepted_before,
                "Executed tasks must be a subset of accepted tasks");
    TEST_ASSERT(status.dropped_task_count >=
                    static_cast<uint64_t>(rejected_after_stop.load(std::memory_order_acquire)),
                "Post-stop rejected pushes must be reflected in dropped_task_count");

    std::cout << "  ConcurrentStopPushNoAcceptedOrphans: PASSED"
              << " (accepted_before=" << accepted_before
              << ", rejected_before=" << rejected_before_stop.load(std::memory_order_acquire)
              << ", rejected_after=" << rejected_after_stop.load(std::memory_order_acquire)
              << ", executed=" << executed
              << ", dropped=" << status.dropped_task_count << ")" << std::endl;
    return true;
}

// P-002 regression: a producer that registered at the admission gate must
// hold stop_and_join() until it leaves, and producers arriving after the gate
// closed are rejected as drops without touching the pool or queue. The old
// unconditional fetch_add let stop observe in_flight_pushes_ == 0 while a
// producer sat before the increment, so stop could drain, return, and the
// destructor could free the object under the producer.
bool test_realtime_stop_admission_gate_waits_for_registered_producer() {
    std::cout << "Testing RealtimeThreadExecutor StopAdmissionGate..." << std::endl;

    RealtimeThreadConfig config;
    config.thread_name = "test_admission_gate";
    config.cycle_period_ns = 20'000'000;  // 20ms
    config.thread_priority = 0;
    config.cycle_callback = [] {};

    RealtimeThreadExecutor executor("test_admission_gate", config);
    TEST_ASSERT(executor.start(), "admission gate executor should start");

    std::atomic<bool> registered{false};
    std::atomic<bool> release{false};
    struct AdmissionHooks {
        std::atomic<bool>* registered;
        std::atomic<bool>* release;
    } hooks{&registered, &release};
    executor.set_admission_registered_hook_for_test(
        +[](void* context) {
            auto* h = static_cast<AdmissionHooks*>(context);
            h->registered->store(true, std::memory_order_release);
            while (!h->release->load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        },
        &hooks);

    std::atomic<bool> push_accepted{true};
    std::thread producer([&] {
        push_accepted.store(executor.push_task_ex([] {}),
                            std::memory_order_release);
    });
    while (!registered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    std::atomic<bool> stop_returned{false};
    std::thread stopper([&] {
        (void)executor.stop_and_join();
        stop_returned.store(true, std::memory_order_release);
    });

    // The producer is registered inside the admission gate; stop must not
    // return before it leaves.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    TEST_ASSERT(!stop_returned.load(std::memory_order_acquire),
                "stop_and_join returned while a registered producer was "
                "still inside push_task_ex");

    release.store(true, std::memory_order_release);
    producer.join();
    stopper.join();

    TEST_ASSERT(stop_returned.load(std::memory_order_acquire),
                "stop_and_join should return after the producer left");
    // running_ is flipped before stop waits out registered producers, so this
    // submission completes as a post-stop reject.
    TEST_ASSERT(!push_accepted.load(std::memory_order_acquire),
                "submission after stop began should be rejected");

    const auto before = executor.get_status();
    TEST_ASSERT(!executor.push_task_ex([] {}),
                "late producer after stop_and_join should be rejected");
    const auto after = executor.get_status();
    TEST_ASSERT(after.dropped_task_count == before.dropped_task_count + 1,
                "late producer rejection should count as a drop");

    std::cout << "  RealtimeStopAdmissionGateWaitsForRegisteredProducer: PASSED"
              << std::endl;
    return true;
}

// ========== 统计信息测试 ==========

bool test_realtime_executor_statistics() {
    std::cout << "Testing RealtimeThreadExecutor statistics..." << std::endl;
    
    const int64_t period_ns = 10000000;  // 10ms
    
    RealtimeThreadConfig config;
    config.thread_name = "test_thread";
    config.cycle_period_ns = period_ns;
    config.cycle_callback = []() {
        // 模拟一些工作
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    };
    
    RealtimeThreadExecutor executor("test_executor", config);
    TEST_ASSERT(executor.start(), "Executor should start successfully");
    
    // 等待几个周期
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    
    auto status = executor.get_status();
    
    // 检查统计信息
    TEST_ASSERT(status.cycle_count > 0, "Cycle count should be greater than 0");
    TEST_ASSERT(status.avg_cycle_time_ns > 0, "Average cycle time should be greater than 0");
    TEST_ASSERT(status.max_cycle_time_ns > 0, "Max cycle time should be greater than 0");
    TEST_ASSERT(status.max_cycle_time_ns >= status.avg_cycle_time_ns, 
                "Max cycle time should be >= average cycle time");
    
    executor.stop();
    
    std::cout << "  Statistics: PASSED (cycles: " << status.cycle_count 
              << ", avg: " << status.avg_cycle_time_ns / 1000000.0 << "ms"
              << ", max: " << status.max_cycle_time_ns / 1000000.0 << "ms)" << std::endl;
    return true;
}

bool test_realtime_executor_timeout_count() {
    std::cout << "Testing RealtimeThreadExecutor timeout count..." << std::endl;
    
    const int64_t period_ns = 10000000;  // 10ms
    
    RealtimeThreadConfig config;
    config.thread_name = "test_thread";
    config.cycle_period_ns = period_ns;
    config.cycle_callback = []() {
        // 模拟长时间工作，导致周期超时
        std::this_thread::sleep_for(std::chrono::milliseconds(15));  // 超过周期时间
    };
    
    RealtimeThreadExecutor executor("test_executor", config);
    TEST_ASSERT(executor.start(), "Executor should start successfully");
    
    // 等待几个周期
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    auto status = executor.get_status();
    
    // 检查超时计数
    TEST_ASSERT(status.cycle_timeout_count > 0, "Timeout count should be greater than 0");
    
    executor.stop();
    
    std::cout << "  Timeout count: PASSED (timeouts: " << status.cycle_timeout_count << ")" << std::endl;
    return true;
}

bool test_realtime_atomic_stats_lock_free() {
    std::cout << "Testing RealtimeThreadExecutor atomic stats lock-free path..." << std::endl;

    RealtimeThreadConfig config;
    config.thread_name = "test_thread";
    config.cycle_period_ns = 10000000;  // 10ms
    config.cycle_callback = []() {};

    RealtimeThreadExecutor executor("test_executor", config);
    constexpr int kIterations = 10000;
    constexpr int64_t kUpdateBudgetNs = 500;
    std::atomic<bool> writer_done(false);
    std::atomic<int64_t> elapsed_ns(0);

    std::thread rt_writer([&]() {
        timespec start{};
        timespec end{};
#ifdef CLOCK_MONOTONIC_RAW
        constexpr clockid_t kClockId = CLOCK_MONOTONIC_RAW;
#else
        constexpr clockid_t kClockId = CLOCK_MONOTONIC;
#endif
        clock_gettime(kClockId, &start);
        for (int i = 0; i < kIterations; ++i) {
            executor.update_statistics(1000 + i);
        }
        clock_gettime(kClockId, &end);

        const int64_t seconds = static_cast<int64_t>(end.tv_sec - start.tv_sec);
        const int64_t nanoseconds = static_cast<int64_t>(end.tv_nsec - start.tv_nsec);
        elapsed_ns.store(seconds * 1000000000LL + nanoseconds, std::memory_order_release);
        writer_done.store(true, std::memory_order_release);
    });

    while (!writer_done.load(std::memory_order_acquire)) {
        auto status = executor.get_status();
        TEST_ASSERT(status.cycle_count <= kIterations,
                    "Concurrent status read should observe a bounded cycle count");
        std::this_thread::yield();
    }
    rt_writer.join();

    const auto status = executor.get_status();
    const int64_t per_update_ns = elapsed_ns.load(std::memory_order_acquire) / kIterations;
    TEST_ASSERT(status.cycle_count == kIterations,
                "Stats writer should record all update_statistics calls");
    TEST_ASSERT(status.avg_cycle_time_ns > 0.0,
                "Average cycle time should be recorded");
    TEST_ASSERT(status.max_cycle_time_ns == static_cast<double>(1000 + kIterations - 1),
                "Max cycle time should track the largest integer sample");
    TEST_ASSERT(per_update_ns < kUpdateBudgetNs,
                "Average update_statistics time should stay below 500ns");

    std::cout << "  Atomic stats lock-free path: PASSED (avg update "
              << per_update_ns << "ns)" << std::endl;
    return true;
}

// ========== 异常处理测试 ==========

bool test_realtime_executor_cycle_callback_exception() {
    std::cout << "Testing RealtimeThreadExecutor cycle callback exception..." << std::endl;
    
    std::atomic<int> cycle_count(0);
    const int64_t period_ns = 10000000;  // 10ms
    
    RealtimeThreadConfig config;
    config.thread_name = "test_thread";
    config.cycle_period_ns = period_ns;
    config.cycle_callback = [&cycle_count]() {
        cycle_count.fetch_add(1);
        if (cycle_count.load() == 1) {
            // 第一次周期抛出异常
            throw std::runtime_error("Test exception");
        }
    };
    
    RealtimeThreadExecutor executor("test_executor", config);
    TEST_ASSERT(executor.start(), "Executor should start successfully");
    
    // 等待几个周期
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // 检查周期是否继续执行（异常不应该影响周期执行）
    int count = cycle_count.load();
    TEST_ASSERT(count > 1, "Cycle should continue after exception");
    
    // 检查执行器是否仍在运行
    auto status = executor.get_status();
    TEST_ASSERT(status.is_running == true, "Executor should still be running after exception");
    
    executor.stop();
    
    std::cout << "  Cycle callback exception: PASSED (cycles: " << count << ")" << std::endl;
    return true;
}

bool test_realtime_executor_task_exception() {
    std::cout << "Testing RealtimeThreadExecutor task exception..." << std::endl;
    
    std::atomic<int> task_count(0);
    const int64_t period_ns = 10000000;  // 10ms
    
    RealtimeThreadConfig config;
    config.thread_name = "test_thread";
    config.cycle_period_ns = period_ns;
    config.cycle_callback = []() {};
    
    RealtimeThreadExecutor executor("test_executor", config);
    TEST_ASSERT(executor.start(), "Executor should start successfully");
    
    // 推送一个会抛出异常的任务
    executor.push_task([&task_count]() {
        task_count.fetch_add(1);
        throw std::runtime_error("Task exception");
    });
    
    // 推送一个正常任务
    executor.push_task([&task_count]() {
        task_count.fetch_add(1);
    });
    
    // 等待任务执行
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // 检查两个任务是否都执行了（异常不应该影响其他任务）
    int count = task_count.load();
    TEST_ASSERT(count == 2, "Both tasks should be executed");
    
    executor.stop();
    
    std::cout << "  Task exception: PASSED" << std::endl;
    return true;
}

// ========== 并发测试 ==========

bool test_realtime_executor_concurrent_push_task() {
    std::cout << "Testing RealtimeThreadExecutor concurrent push_task..." << std::endl;
    
    std::atomic<int> task_count(0);
    const int64_t period_ns = 10000000;  // 10ms
    
    RealtimeThreadConfig config;
    config.thread_name = "test_thread";
    config.cycle_period_ns = period_ns;
    config.cycle_callback = []() {};
    
    RealtimeThreadExecutor executor("test_executor", config);
    TEST_ASSERT(executor.start(), "Executor should start successfully");
    
    // 多个线程同时推送任务
    const int num_threads = 5;
    const int tasks_per_thread = 10;
    std::vector<std::thread> threads;
    
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&executor, &task_count, tasks_per_thread]() {
            for (int j = 0; j < tasks_per_thread; ++j) {
                executor.push_task([&task_count]() {
                    task_count.fetch_add(1);
                });
            }
        });
    }
    
    // 等待所有线程完成
    for (auto& t : threads) {
        t.join();
    }

    // 等待任务执行，使用轮询而不是固定等待
    const int expected_count = num_threads * tasks_per_thread;
    int count = 0;
    for (int i = 0; i < 50; ++i) {  // 最多等待500ms
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        count = task_count.load();
        if (count == expected_count) {
            break;
        }
    }

    // 检查所有任务是否都执行了
    TEST_ASSERT(count == expected_count,
                "All tasks should be executed");
    
    executor.stop();
    
    std::cout << "  Concurrent push_task: PASSED (executed " << count << " tasks)" << std::endl;
    return true;
}

bool test_realtime_executor_concurrent_get_status() {
    std::cout << "Testing RealtimeThreadExecutor concurrent get_status..." << std::endl;
    
    const int64_t period_ns = 10000000;  // 10ms
    
    RealtimeThreadConfig config;
    config.thread_name = "test_thread";
    config.cycle_period_ns = period_ns;
    config.cycle_callback = []() {};
    
    RealtimeThreadExecutor executor("test_executor", config);
    TEST_ASSERT(executor.start(), "Executor should start successfully");
    
    // 多个线程同时获取状态
    const int num_threads = 10;
    std::vector<std::thread> threads;
    std::atomic<int> success_count(0);
    
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&executor, &success_count]() {
            for (int j = 0; j < 100; ++j) {
                auto status = executor.get_status();
                if (status.is_running && status.cycle_period_ns == 10000000) {
                    success_count.fetch_add(1);
                }
            }
        });
    }
    
    // 等待所有线程完成
    for (auto& t : threads) {
        t.join();
    }
    
    executor.stop();
    
    // 检查所有调用是否成功
    int count = success_count.load();
    TEST_ASSERT(count == num_threads * 100, "All status calls should succeed");
    
    std::cout << "  Concurrent get_status: PASSED" << std::endl;
    return true;
}

// ========== 主函数 ==========

int main() {
    std::cout << "========== RealtimeThreadExecutor 集成测试 ==========" << std::endl;
    std::cout << std::endl;
    
    bool all_passed = true;
    
    // 基本功能测试
    all_passed &= test_realtime_executor_basic();
    all_passed &= test_realtime_executor_double_start();
    all_passed &= test_realtime_start_thread_creation_failure_rolls_back();
    
    // 周期循环测试
    all_passed &= test_realtime_executor_cycle_loop();
    all_passed &= test_realtime_executor_cycle_period_accuracy();
    
    // 任务处理测试
    all_passed &= test_realtime_executor_push_task();
    all_passed &= test_realtime_executor_task_order();
    all_passed &= test_realtime_stop_drains_queue();
    all_passed &= test_realtime_concurrent_stop_does_not_double_release();
    all_passed &= test_realtime_push_after_stop_returns_false_and_counts_drop();
    all_passed &= test_realtime_concurrent_stop_push_no_accepted_orphans();
    all_passed &= test_realtime_stop_admission_gate_waits_for_registered_producer();
    
    // 统计信息测试
    all_passed &= test_realtime_executor_statistics();
    all_passed &= test_realtime_executor_timeout_count();
    all_passed &= test_realtime_atomic_stats_lock_free();
    
    // 异常处理测试
    all_passed &= test_realtime_executor_cycle_callback_exception();
    all_passed &= test_realtime_executor_task_exception();
    
    // 并发测试
    all_passed &= test_realtime_executor_concurrent_push_task();
    all_passed &= test_realtime_executor_concurrent_get_status();
    
    std::cout << std::endl;
    if (all_passed) {
        std::cout << "========== 所有测试通过 ==========" << std::endl;
        return 0;
    } else {
        std::cout << "========== 部分测试失败 ==========" << std::endl;
        return 1;
    }
}
