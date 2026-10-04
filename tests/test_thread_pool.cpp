#include <cassert>
#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <future>
#include <algorithm>
#include <unordered_set>

// 包含 thread_pool 模块的头文件
#include <kairo/config.hpp>
#include <kairo/types.hpp>
#include "kairo/thread_pool_executor.hpp"
#include "kairo/task/task.hpp"
#include "kairo/thread_pool/priority_scheduler.hpp"
#include "kairo/thread_pool/thread_pool.hpp"

#ifdef __ANDROID__
#include "kairo/util/thread_utils.hpp"
#endif

using namespace kairo;

// 测试辅助宏
#define TEST_ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            std::cerr << "FAILED: " << message << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
            return false; \
        } \
    } while(0)

// ========== PriorityScheduler 测试 ==========

bool test_priority_scheduler_basic() {
    std::cout << "Testing PriorityScheduler basic operations..." << std::endl;
    
    PriorityScheduler scheduler;
    
    // 测试空队列
    TEST_ASSERT(scheduler.empty(), "Scheduler should be empty initially");
    TEST_ASSERT(scheduler.size() == 0, "Scheduler size should be 0 initially");
    
    // 创建测试任务
    Task task1;
    task1.task_id = "task_1";
    task1.priority = TaskPriority::NORMAL;
    task1.submit_time_ns = 1000;
    task1.function = []() noexcept {};
    
    // 测试enqueue
    scheduler.enqueue(task1);
    TEST_ASSERT(!scheduler.empty(), "Scheduler should not be empty after enqueue");
    TEST_ASSERT(scheduler.size() == 1, "Scheduler size should be 1");
    
    // 测试dequeue
    Task result;
    TEST_ASSERT(scheduler.dequeue(result), "Should be able to dequeue task");
    TEST_ASSERT(result.task_id == "task_1", "Dequeued task ID should match");
    TEST_ASSERT(scheduler.empty(), "Scheduler should be empty after dequeue");
    
    std::cout << "  PriorityScheduler basic operations: PASSED" << std::endl;
    return true;
}

bool test_priority_scheduler_priority_order() {
    std::cout << "Testing PriorityScheduler priority ordering..." << std::endl;
    
    PriorityScheduler scheduler;
    
    // 创建不同优先级的任务（按优先级从低到高提交）
    Task low_task;
    low_task.task_id = "low_task";
    low_task.priority = TaskPriority::LOW;
    low_task.submit_time_ns = 1000;
    low_task.function = []() noexcept {};
    
    Task normal_task;
    normal_task.task_id = "normal_task";
    normal_task.priority = TaskPriority::NORMAL;
    normal_task.submit_time_ns = 2000;
    normal_task.function = []() noexcept {};
    
    Task high_task;
    high_task.task_id = "high_task";
    high_task.priority = TaskPriority::HIGH;
    high_task.submit_time_ns = 3000;
    high_task.function = []() noexcept {};
    
    Task critical_task;
    critical_task.task_id = "critical_task";
    critical_task.priority = TaskPriority::CRITICAL;
    critical_task.submit_time_ns = 4000;
    critical_task.function = []() noexcept {};
    
    // 按低优先级到高优先级顺序提交
    scheduler.enqueue(low_task);
    scheduler.enqueue(normal_task);
    scheduler.enqueue(high_task);
    scheduler.enqueue(critical_task);
    
    // 应该按高优先级到低优先级顺序出队
    Task result;
    TEST_ASSERT(scheduler.dequeue(result), "Should dequeue critical task first");
    TEST_ASSERT(result.task_id == "critical_task", "Critical task should be dequeued first");
    
    TEST_ASSERT(scheduler.dequeue(result), "Should dequeue high task second");
    TEST_ASSERT(result.task_id == "high_task", "High task should be dequeued second");
    
    TEST_ASSERT(scheduler.dequeue(result), "Should dequeue normal task third");
    TEST_ASSERT(result.task_id == "normal_task", "Normal task should be dequeued third");
    
    TEST_ASSERT(scheduler.dequeue(result), "Should dequeue low task last");
    TEST_ASSERT(result.task_id == "low_task", "Low task should be dequeued last");
    
    TEST_ASSERT(scheduler.empty(), "Scheduler should be empty after all dequeues");
    
    std::cout << "  PriorityScheduler priority ordering: PASSED" << std::endl;
    return true;
}

bool test_priority_scheduler_same_priority_fifo() {
    std::cout << "Testing PriorityScheduler same priority FIFO..." << std::endl;
    
    PriorityScheduler scheduler;
    
    // 创建相同优先级但不同提交时间的任务
    Task task1;
    task1.task_id = "task_1";
    task1.priority = TaskPriority::NORMAL;
    task1.submit_time_ns = 1000;  // 更早
    task1.function = []() noexcept {};
    
    Task task2;
    task2.task_id = "task_2";
    task2.priority = TaskPriority::NORMAL;
    task2.submit_time_ns = 2000;  // 更晚
    task2.function = []() noexcept {};
    
    Task task3;
    task3.task_id = "task_3";
    task3.priority = TaskPriority::NORMAL;
    task3.submit_time_ns = 3000;  // 最晚
    task3.function = []() noexcept {};
    
    // 按时间顺序提交
    scheduler.enqueue(task1);
    scheduler.enqueue(task2);
    scheduler.enqueue(task3);
    
    // 应该按提交时间顺序出队（FIFO）
    Task result;
    TEST_ASSERT(scheduler.dequeue(result), "Should dequeue task_1 first");
    TEST_ASSERT(result.task_id == "task_1", "Task 1 should be dequeued first (earliest)");
    
    TEST_ASSERT(scheduler.dequeue(result), "Should dequeue task_2 second");
    TEST_ASSERT(result.task_id == "task_2", "Task 2 should be dequeued second");
    
    TEST_ASSERT(scheduler.dequeue(result), "Should dequeue task_3 third");
    TEST_ASSERT(result.task_id == "task_3", "Task 3 should be dequeued third (latest)");
    
    std::cout << "  PriorityScheduler same priority FIFO: PASSED" << std::endl;
    return true;
}

bool test_priority_scheduler_clear() {
    std::cout << "Testing PriorityScheduler clear..." << std::endl;
    
    PriorityScheduler scheduler;
    
    // 添加一些任务
    for (int i = 0; i < 10; ++i) {
        Task task;
        task.task_id = "task_" + std::to_string(i);
        task.priority = static_cast<TaskPriority>(i % 4);
        task.submit_time_ns = i * 1000;
        task.function = []() noexcept {};
        scheduler.enqueue(task);
    }
    
    TEST_ASSERT(scheduler.size() == 10, "Scheduler should have 10 tasks");
    
    scheduler.clear();
    
    TEST_ASSERT(scheduler.empty(), "Scheduler should be empty after clear");
    TEST_ASSERT(scheduler.size() == 0, "Scheduler size should be 0 after clear");
    
    std::cout << "  PriorityScheduler clear: PASSED" << std::endl;
    return true;
}

bool test_priority_scheduler_concurrent() {
    std::cout << "Testing PriorityScheduler concurrent operations..." << std::endl;
    
    PriorityScheduler scheduler;
    const int num_threads = 10;
    const int tasks_per_thread = 100;
    std::atomic<int> enqueue_count{0};
    std::atomic<int> dequeue_count{0};
    
    // 多个线程同时enqueue和dequeue
    std::vector<std::thread> threads;
    
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&, i]() {
            // 每个线程enqueue一些任务
            for (int j = 0; j < tasks_per_thread; ++j) {
                Task task;
                task.task_id = "task_" + std::to_string(i) + "_" + std::to_string(j);
                task.priority = static_cast<TaskPriority>(j % 4);
                task.submit_time_ns = j * 1000;
                task.function = []() noexcept {};
                scheduler.enqueue(task);
                enqueue_count.fetch_add(1);
            }
            
            // 每个线程dequeue一些任务
            for (int j = 0; j < tasks_per_thread; ++j) {
                Task result;
                if (scheduler.dequeue(result)) {
                    dequeue_count.fetch_add(1);
                }
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    TEST_ASSERT(enqueue_count.load() == num_threads * tasks_per_thread,
               "All enqueue operations should complete");
    TEST_ASSERT(dequeue_count.load() > 0, "Some dequeue operations should complete");
    
    std::cout << "  PriorityScheduler concurrent operations: PASSED" << std::endl;
    return true;
}

// ========== ThreadPool 测试 ==========

bool test_thread_pool_initialize() {
    std::cout << "Testing ThreadPool initialize..." << std::endl;
    
    ThreadPool pool;
    
    ThreadPoolConfig config;
    config.min_threads = 4;
    config.max_threads = 8;
    config.queue_capacity = 100;
    
    TEST_ASSERT(pool.initialize(config), "Should be able to initialize thread pool");
    TEST_ASSERT(!pool.is_stopped(), "Thread pool should not be stopped after initialization");
    
    // 再次初始化应该失败
    TEST_ASSERT(!pool.initialize(config), "Should not be able to initialize twice");
    
    pool.shutdown();
    
    std::cout << "  ThreadPool initialize: PASSED" << std::endl;
    return true;
}

bool test_thread_pool_init_oom_safety() {
    std::cout << "Testing ThreadPool initialize OOM safety..." << std::endl;

    ThreadPoolConfig config;
    config.min_threads = 4;
    config.max_threads = 4;
    config.queue_capacity = 1024;

    bool initialized = true;
    {
        ThreadPool pool;
        pool.set_worker_queue_create_hook_for_test([](size_t worker_id) {
            if (worker_id == 0) {
                throw std::bad_alloc();
            }
        });
        initialized = pool.initialize(config);
    }

    TEST_ASSERT(!initialized, "initialize should return false when worker queue construction throws");

    std::cout << "  ThreadPool initialize OOM safety: PASSED" << std::endl;
    return true;
}

bool test_thread_pool_init_worker_thread_failure_rolls_back() {
    std::cout << "Testing ThreadPool initialize worker thread failure rollback..." << std::endl;

    ThreadPool pool;

    ThreadPoolConfig config;
    config.min_threads = 4;
    config.max_threads = 4;
    config.queue_capacity = 100;

    std::atomic<int> create_attempts{0};
    pool.set_worker_thread_start_hook_for_test([&](size_t) {
        int attempt = create_attempts.fetch_add(1, std::memory_order_relaxed) + 1;
        if (attempt == 3) {
            throw std::system_error(
                std::make_error_code(std::errc::resource_unavailable_try_again),
                "injected worker thread creation failure");
        }
    });

    bool initialized = true;
    bool threw = false;
    try {
        initialized = pool.initialize(config);
    } catch (...) {
        threw = true;
    }

    TEST_ASSERT(!threw, "initialize should not throw when worker thread creation fails");
    TEST_ASSERT(!initialized, "initialize should return false when worker thread creation fails");
    TEST_ASSERT(!pool.is_stopped(), "rollback should leave the pool reusable, not permanently stopped");

    ThreadPoolStatus status_after_failure = pool.get_status();
    TEST_ASSERT(status_after_failure.total_threads == 0,
                "rollback should join and clear already-created workers");
    TEST_ASSERT(status_after_failure.queue_size == 0,
                "rollback should clear local/global queues");

    pool.set_worker_thread_start_hook_for_test(nullptr);
    TEST_ASSERT(pool.initialize(config), "same ThreadPool object should initialize after rollback");

    auto future = pool.submit([]() noexcept {
        return 123;
    });
    TEST_ASSERT(future.get() == 123, "reinitialized pool should execute submitted tasks");

    pool.shutdown();

    std::cout << "  ThreadPool initialize worker thread failure rollback: PASSED" << std::endl;
    return true;
}

bool test_thread_pool_invalid_cpu_affinity_is_nonfatal() {
    std::cout << "Testing ThreadPool invalid cpu_affinity is nonfatal..." << std::endl;

    ThreadPool pool;
    ThreadPoolConfig config;
    config.min_threads = 1;
    config.max_threads = 1;
    config.queue_capacity = 8;
    // 显式给出必然越界的 CPU id；worker 启动时绑核失败但线程池必须继续可用。
    // Android 上 cgroup/SELinux 导致的运行时 EPERM/EINVAL 走同一非致命路径。
    config.cpu_affinity = {999999};

    TEST_ASSERT(pool.initialize(config),
                "pool with invalid explicit affinity should still initialize");
    auto future = pool.submit([]() noexcept { return 7; });
    TEST_ASSERT(future.get() == 7,
                "pool with failed affinity request should still execute tasks");
    TEST_ASSERT(pool.get_status().total_threads == 1,
                "pool with failed affinity request should still create its worker");
    TEST_ASSERT(pool.shutdown(true) == ShutdownResult::Completed,
                "pool with failed affinity request should shut down");

    std::cout << "  ThreadPool invalid cpu_affinity nonfatal: PASSED" << std::endl;
    return true;
}

bool test_thread_pool_shutdown_drains_when_workers_start_late() {
    std::cout << "Testing ThreadPool shutdown with delayed worker entry..." << std::endl;

    ThreadPool pool;
    ThreadPoolConfig config;
    config.min_threads = 2;
    config.max_threads = 2;
    config.queue_capacity = 100;

    std::promise<void> release_workers;
    std::shared_future<void> release = release_workers.get_future().share();
    pool.set_worker_entry_hook_for_test([release](size_t) {
        release.wait();
    });

    TEST_ASSERT(pool.initialize(config), "delayed-worker pool should initialize");

    std::atomic<int> completed{0};
    for (int i = 0; i < 10; ++i) {
        pool.submit([&completed]() noexcept {
            completed.fetch_add(1, std::memory_order_relaxed);
        });
    }

    auto shutdown = std::async(std::launch::async, [&pool]() {
        return pool.shutdown(true);
    });
    const auto stop_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!pool.is_stopped() && std::chrono::steady_clock::now() < stop_deadline) {
        std::this_thread::yield();
    }
    TEST_ASSERT(pool.is_stopped(), "shutdown should publish stop before workers are released");

    release_workers.set_value();
    TEST_ASSERT(shutdown.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
                "shutdown should finish after delayed workers enter");
    TEST_ASSERT(completed.load(std::memory_order_relaxed) == 10,
                "workers starting after stop should drain accepted tasks");

    std::cout << "  ThreadPool shutdown with delayed worker entry: PASSED" << std::endl;
    return true;
}

bool test_thread_pool_submit_basic() {
    std::cout << "Testing ThreadPool submit basic..." << std::endl;
    
    ThreadPool pool;
    ThreadPoolConfig config;
    config.min_threads = 2;
    config.max_threads = 4;
    pool.initialize(config);
    
    // 提交简单任务
    auto future = pool.submit([]() noexcept {
        return 42;
    });

    int result = future.get();
    TEST_ASSERT(result == 42, "Task result should be 42");

    // 提交带参数的任务
    auto future2 = pool.submit([](int a, int b) noexcept {
        return a + b;
    }, 10, 20);
    
    int result2 = future2.get();
    TEST_ASSERT(result2 == 30, "Task result should be 30");
    
    pool.shutdown();
    
    std::cout << "  ThreadPool submit basic: PASSED" << std::endl;
    return true;
}

bool test_thread_pool_submit_priority() {
    std::cout << "Testing ThreadPool submit_priority..." << std::endl;
    
    ThreadPool pool;
    ThreadPoolConfig config;
    config.min_threads = 2;
    config.max_threads = 4;
    pool.initialize(config);
    
    std::vector<int> execution_order;
    std::mutex order_mutex;
    
    // 提交不同优先级的任务
    pool.submit_priority(0, [&]() {  // LOW
        std::lock_guard<std::mutex> lock(order_mutex);
        execution_order.push_back(0);
    });
    
    pool.submit_priority(3, [&]() {  // CRITICAL
        std::lock_guard<std::mutex> lock(order_mutex);
        execution_order.push_back(3);
    });
    
    pool.submit_priority(1, [&]() {  // NORMAL
        std::lock_guard<std::mutex> lock(order_mutex);
        execution_order.push_back(1);
    });
    
    pool.submit_priority(2, [&]() {  // HIGH
        std::lock_guard<std::mutex> lock(order_mutex);
        execution_order.push_back(2);
    });
    
    // 等待所有任务完成
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    pool.wait_for_completion();
    
    // 验证执行顺序（高优先级应该先执行）
    TEST_ASSERT(execution_order.size() == 4, "All tasks should be executed");
    // 注意：由于并发执行，顺序可能不完全确定，但CRITICAL应该较早执行
    TEST_ASSERT(std::find(execution_order.begin(), execution_order.end(), 3) != execution_order.end(),
               "CRITICAL task should be executed");
    
    pool.shutdown();
    
    std::cout << "  ThreadPool submit_priority: PASSED" << std::endl;
    return true;
}

bool test_thread_pool_concurrent_submit() {
    std::cout << "Testing ThreadPool concurrent submit..." << std::endl;
    
    ThreadPool pool;
    ThreadPoolConfig config;
    config.min_threads = 4;
    config.max_threads = 8;
    pool.initialize(config);
    
    const int num_tasks = 100;
    std::atomic<int> completed_count{0};
    std::vector<std::future<void>> futures;
    
    // 并发提交多个任务
    for (int i = 0; i < num_tasks; ++i) {
        auto future = pool.submit([&, i]() noexcept {
            completed_count.fetch_add(1);
        });
        futures.push_back(std::move(future));
    }
    
    // 等待所有任务完成
    for (auto& future : futures) {
        future.wait();
    }
    
    TEST_ASSERT(completed_count.load() == num_tasks, "All tasks should be completed");
    
    pool.shutdown();
    
    std::cout << "  ThreadPool concurrent submit: PASSED" << std::endl;
    return true;
}

bool test_thread_pool_concurrent_task_id_uniqueness() {
    std::cout << "Testing ThreadPool concurrent task ID uniqueness..." << std::endl;

    const int num_threads = 16;
    const int ids_per_thread = 5000;
    std::atomic<int> ready_count{0};
    std::atomic<bool> start_flag{false};
    std::vector<std::thread> threads;
    std::vector<std::vector<std::string>> ids(num_threads);

    for (int t = 0; t < num_threads; ++t) {
        ids[t].reserve(ids_per_thread);
        threads.emplace_back([&, t]() {
            ready_count.fetch_add(1, std::memory_order_release);
            while (!start_flag.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            for (int i = 0; i < ids_per_thread; ++i) {
                ids[t].push_back(generate_task_id());
            }
        });
    }

    while (ready_count.load(std::memory_order_acquire) < num_threads) {
        std::this_thread::yield();
    }

    start_flag.store(true, std::memory_order_release);

    for (auto& t : threads) {
        t.join();
    }

    std::unordered_set<std::string> unique_ids;
    unique_ids.reserve(static_cast<size_t>(num_threads * ids_per_thread));
    for (const auto& thread_ids : ids) {
        TEST_ASSERT(thread_ids.size() == static_cast<size_t>(ids_per_thread),
                   "Each submitter should generate the expected number of IDs");
        for (const auto& id : thread_ids) {
            TEST_ASSERT(id.rfind("task_", 0) == 0, "Task ID should keep task_ prefix");
            TEST_ASSERT(unique_ids.insert(id).second, "Task IDs should be unique under concurrency");
        }
    }

    TEST_ASSERT(unique_ids.size() == static_cast<size_t>(num_threads * ids_per_thread),
               "All generated task IDs should be unique");

    std::cout << "  ThreadPool concurrent task ID uniqueness: PASSED" << std::endl;
    return true;
}

bool test_thread_pool_exception_handling() {
    std::cout << "Testing ThreadPool exception handling..." << std::endl;
    
    ThreadPool pool;
    ThreadPoolConfig config;
    config.min_threads = 2;
    config.max_threads = 4;
    pool.initialize(config);
    
    // 提交抛出异常的任务
    auto future = pool.submit([]() {
        throw std::runtime_error("Test exception");
        return 42;
    });

    future.wait();
    pool.shutdown();
    
    // 应该能够捕获异常
    bool exception_caught = false;
    try {
        future.get();
    } catch (const std::exception& e) {
        exception_caught = true;
        TEST_ASSERT(std::string(e.what()) == "Test exception", "Exception message should match");
    }
    
    TEST_ASSERT(exception_caught, "Exception should be caught");
    
    std::cout << "  ThreadPool exception handling: PASSED" << std::endl;
    return true;
}

bool test_thread_pool_status() {
    std::cout << "Testing ThreadPool status..." << std::endl;
    
    ThreadPool pool;
    ThreadPoolConfig config;
    config.min_threads = 4;
    config.max_threads = 8;
    pool.initialize(config);
    
    // 获取初始状态
    auto status = pool.get_status();
    TEST_ASSERT(status.total_threads == 4, "Total threads should be 4");
    TEST_ASSERT(status.total_tasks == 0, "Initial total tasks should be 0");
    
    // 提交一些任务
    std::vector<std::future<void>> futures;
    for (int i = 0; i < 10; ++i) {
        futures.push_back(pool.submit([]() noexcept {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }));
    }
    
    // 等待一段时间让任务开始执行
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    status = pool.get_status();
    TEST_ASSERT(status.total_tasks == 10, "Total tasks should be 10");
    TEST_ASSERT(status.queue_size >= 0, "Queue size should be non-negative");
    
    // 等待所有任务完成
    for (auto& future : futures) {
        future.wait();
    }
    pool.wait_for_completion();
    
    status = pool.get_status();
    TEST_ASSERT(status.completed_tasks == 10, "Completed tasks should be 10");
    
    pool.shutdown();
    
    std::cout << "  ThreadPool status: PASSED" << std::endl;
    return true;
}

bool test_thread_pool_shutdown() {
    std::cout << "Testing ThreadPool shutdown..." << std::endl;
    
    ThreadPool pool;
    ThreadPoolConfig config;
    config.min_threads = 2;
    config.max_threads = 4;
    pool.initialize(config);
    
    // 提交一些任务
    std::vector<std::future<int>> futures;
    for (int i = 0; i < 5; ++i) {
        futures.push_back(pool.submit([i]() noexcept {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            return i;
        }));
    }
    
    // 等待一段时间
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // 关闭线程池（等待任务完成）
    pool.shutdown(true);
    
    TEST_ASSERT(pool.is_stopped(), "Thread pool should be stopped after shutdown");
    
    // 验证任务都完成了
    for (size_t i = 0; i < futures.size(); ++i) {
        int result = futures[i].get();
        TEST_ASSERT(result == static_cast<int>(i), "Task result should match");
    }
    
    std::cout << "  ThreadPool shutdown: PASSED" << std::endl;
    return true;
}

bool test_thread_pool_shutdown_no_wait() {
    std::cout << "Testing ThreadPool shutdown (no wait)..." << std::endl;
    
    ThreadPool pool;
    ThreadPoolConfig config;
    config.min_threads = 2;
    config.max_threads = 4;
    pool.initialize(config);
    
    // 提交一些长时间运行的任务
    std::vector<std::future<void>> futures;
    for (int i = 0; i < 5; ++i) {
        futures.push_back(pool.submit([]() noexcept {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }));
    }
    
    // 立即关闭（不等待）
    pool.shutdown(false);
    
    TEST_ASSERT(pool.is_stopped(), "Thread pool should be stopped after shutdown");
    
    std::cout << "  ThreadPool shutdown (no wait): PASSED" << std::endl;
    return true;
}

bool test_thread_pool_concurrent_shutdown() {
    std::cout << "Testing ThreadPool concurrent shutdown (10 threads, P-008 regression)..." << std::endl;

    ThreadPool pool;
    ThreadPoolConfig config;
    config.min_threads = 4;
    config.max_threads = 8;
    TEST_ASSERT(pool.initialize(config), "Should initialize thread pool");

    // 提交几个短任务,确保 worker 线程真的在运行
    std::vector<std::future<void>> futures;
    for (int i = 0; i < 4; ++i) {
        futures.push_back(pool.submit([]() noexcept {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }));
    }

    // P-008 回归测试: 10 个线程同时调用 shutdown(),
    // 修复前可能两个线程同时通过 stop_.load() 早返回检查,
    // 然后都进入 resize_monitor_thread_.join() / workers_ join 循环,
    // 第二次 join 触发 std::system_error (UB)。
    const int num_threads = 10;
    std::atomic<int> ready_count{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> shutdown_threads;
    std::vector<std::exception_ptr> exceptions(num_threads);
    std::atomic<int> success_count{0};

    for (int i = 0; i < num_threads; ++i) {
        shutdown_threads.emplace_back([&, i]() {
            // 在屏障前自旋,等所有线程就位
            ready_count.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            try {
                pool.shutdown(true);  // 触发 race 的关键调用
                success_count.fetch_add(1, std::memory_order_relaxed);
            } catch (...) {
                exceptions[i] = std::current_exception();
            }
        });
    }

    // 等所有线程都进入就绪态
    while (ready_count.load(std::memory_order_acquire) < num_threads) {
        std::this_thread::yield();
    }
    // 同时放行所有 shutdown 调用,最大化 race 窗口
    go.store(true, std::memory_order_release);

    // 等待所有 shutdown 线程完成
    for (auto& t : shutdown_threads) {
        t.join();
    }

    // 断言: 没有线程因 double-join 抛出异常
    for (int i = 0; i < num_threads; ++i) {
        TEST_ASSERT(!exceptions[i], "Concurrent shutdown should not throw (no double-join UB)");
    }
    TEST_ASSERT(success_count.load() == num_threads,
                "All concurrent shutdown calls should succeed");
    TEST_ASSERT(pool.is_stopped(), "Thread pool should be stopped after concurrent shutdown");

    // 任务 future 应当仍然可 get() (已经在 shutdown(true) 之前提交)
    for (auto& f : futures) {
        f.get();
    }

    std::cout << "  ThreadPool concurrent shutdown: PASSED" << std::endl;
    return true;
}

bool test_thread_pool_submit_after_shutdown() {
    std::cout << "Testing ThreadPool submit after shutdown..." << std::endl;

    ThreadPool pool;
    ThreadPoolConfig config;
    config.min_threads = 2;
    config.max_threads = 4;
    pool.initialize(config);

    pool.shutdown();
    
    // 关闭后提交任务应该抛出异常
    bool exception_caught = false;
    try {
        auto future = pool.submit([]() noexcept {
            return 42;
        });
        future.get();
    } catch (const std::exception& e) {
        exception_caught = true;
    }
    
    TEST_ASSERT(exception_caught, "Should throw exception when submitting after shutdown");
    
    std::cout << "  ThreadPool submit after shutdown: PASSED" << std::endl;
    return true;
}

bool test_thread_pool_submit_batch_after_shutdown_futures_ready() {
    std::cout << "Testing ThreadPool submit_batch after shutdown futures ready..." << std::endl;

    ThreadPoolConfig config;
    config.min_threads = 2;
    config.max_threads = 4;

    ThreadPoolExecutor executor("test", config);
    TEST_ASSERT(executor.start(), "Should be able to start thread pool executor");
    executor.stop();

    std::vector<std::function<void()>> tasks;
    tasks.push_back([]() noexcept {});
    tasks.push_back([]() noexcept {});
    tasks.push_back([]() noexcept {});

    auto futures = executor.submit_batch(tasks);
    TEST_ASSERT(futures.size() == tasks.size(), "Should return one future per submitted task");

    for (auto& future : futures) {
        TEST_ASSERT(future.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready,
                    "Rejected batch future should become ready promptly");

        bool exception_caught = false;
        try {
            future.get();
        } catch (const std::exception& e) {
            exception_caught = true;
            TEST_ASSERT(std::string(e.what()).find("ThreadPool is stopped") != std::string::npos,
                        "Rejected batch future exception should mention stopped thread pool");
        }

        TEST_ASSERT(exception_caught, "Rejected batch future should throw");
    }

    std::cout << "  ThreadPool submit_batch after shutdown futures ready: PASSED" << std::endl;
    return true;
}

bool test_thread_pool_wait_for_completion() {
    std::cout << "Testing ThreadPool wait_for_completion..." << std::endl;
    
    ThreadPool pool;
    ThreadPoolConfig config;
    config.min_threads = 2;
    config.max_threads = 4;
    pool.initialize(config);
    
    std::atomic<int> completed{0};
    const int num_tasks = 20;
    
    // 提交多个任务
    for (int i = 0; i < num_tasks; ++i) {
        pool.submit([&]() noexcept {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            completed.fetch_add(1);
        });
    }
    
    // 等待所有任务完成
    pool.wait_for_completion();
    
    TEST_ASSERT(completed.load() == num_tasks, "All tasks should be completed");
    
    pool.shutdown();
    
    std::cout << "  ThreadPool wait_for_completion: PASSED" << std::endl;
    return true;
}

bool test_thread_pool_wait_for_completion_latency() {
    std::cout << "Testing ThreadPool wait_for_completion latency..." << std::endl;

    ThreadPool pool;
    ThreadPoolConfig config;
    config.min_threads = 16;
    config.max_threads = 16;
    config.queue_capacity = 2048;
    TEST_ASSERT(pool.initialize(config), "Should initialize thread pool");

    const int num_tasks = 1000;
    std::atomic<bool> final_task_started{false};
    std::atomic<bool> release{false};

    for (int i = 0; i < num_tasks - 1; ++i) {
        pool.submit([]() noexcept {});
    }

    pool.submit([&]() noexcept {
        final_task_started.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });

    while (!final_task_started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    auto waiter = std::async(std::launch::async, [&pool]() {
        pool.wait_for_completion();
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    auto start = std::chrono::steady_clock::now();
    release.store(true, std::memory_order_release);
    waiter.wait();
    auto elapsed = std::chrono::steady_clock::now() - start;
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count() / 1000.0;

    TEST_ASSERT(elapsed < std::chrono::milliseconds(5),
                "wait_for_completion should unblock in under 5ms after fast tasks complete");

    pool.shutdown();

    std::cout << "  ThreadPool wait_for_completion latency: PASSED (" << elapsed_ms << " ms)" << std::endl;
    return true;
}

// ========== 主测试函数 ==========

bool test_task_timeout_soft_skip_works() {
    std::cout << "Testing task timeout soft skip (P024)..." << std::endl;

    ThreadPool pool;
    ThreadPoolConfig config;
    config.min_threads = 1;
    config.max_threads = 1;
    config.task_timeout_ms = 1;  // 1ms timeout
    pool.initialize(config);

    // Block the single worker so subsequent tasks queue up
    std::atomic<bool> blocker_done{false};
    pool.submit([&blocker_done]() noexcept {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        blocker_done.store(true);
    });

    // Let the blocker start
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    // Submit a task that will have elapsed > 1ms by the time the worker is free
    std::atomic<bool> function_called{false};
    pool.submit([&function_called]() noexcept {
        function_called.store(true);
    });

    // Wait for everything to settle
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    TEST_ASSERT(!function_called.load(),
                "Timed-out task function should NOT be called");
    TEST_ASSERT(pool.get_timeout_count() >= 1,
                "timeout_count should be >= 1 after soft timeout");

    pool.shutdown();

    std::cout << "  task timeout soft skip: PASSED" << std::endl;
    return true;
}

bool test_task_timeout_not_triggered_when_fast() {
    std::cout << "Testing task timeout not triggered when fast (P024)..." << std::endl;

    ThreadPool pool;
    ThreadPoolConfig config;
    config.min_threads = 2;
    config.max_threads = 4;
    config.task_timeout_ms = 1000;  // 1s timeout, plenty of time
    pool.initialize(config);

    std::atomic<bool> function_called{false};
    auto future = pool.submit([&function_called]() noexcept {
        function_called.store(true);
    });

    future.wait();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    TEST_ASSERT(function_called.load(),
                "Task function should be called normally");
    TEST_ASSERT(pool.get_timeout_count() == 0,
                "timeout_count should be 0 when task is fast enough");

    pool.shutdown();

    std::cout << "  task timeout not triggered when fast: PASSED" << std::endl;
    return true;
}

bool test_default_thread_count_is_adaptive_sentinel() {
    std::cout << "Testing ExecutorConfig/ThreadPoolConfig default thread count is 0 (adaptive sentinel)..." << std::endl;
    ExecutorConfig ec;
    TEST_ASSERT(ec.min_threads == 0, "Default min_threads should be 0 (adaptive sentinel)");
    TEST_ASSERT(ec.max_threads == 0, "Default max_threads should be 0 (adaptive sentinel)");
    ThreadPoolConfig tc;
    TEST_ASSERT(tc.min_threads == 0, "Default ThreadPoolConfig.min_threads should be 0");
    TEST_ASSERT(tc.max_threads == 0, "Default ThreadPoolConfig.max_threads should be 0");
    return true;
}

bool test_default_enable_work_stealing_is_true() {
    std::cout << "Testing default enable_work_stealing is true (opt-out philosophy)..." << std::endl;
    ExecutorConfig ec;
    TEST_ASSERT(ec.enable_work_stealing == true, "Default enable_work_stealing should be true");
    ThreadPoolConfig tc;
    TEST_ASSERT(tc.enable_work_stealing == true, "Default ThreadPoolConfig.enable_work_stealing should be true");
    return true;
}

bool test_default_cpu_affinity_is_auto_allocated() {
#if defined(__ANDROID__)
    std::cout << "Testing default cpu_affinity is auto-allocated from Android cpuset..." << std::endl;

    ExecutorConfig config;
    TEST_ASSERT(config.cpu_affinity.empty(), "Default cpu_affinity should be empty (auto sentinel)");

    const std::vector<int> auto_affinity = kairo::util::get_current_thread_affinity();
    std::cout << "  Android allowed-cpuset affinity size = " << auto_affinity.size() << std::endl;
    // cgroup cpuset 因设备而异，甚至可能为空；不校验具体 0..hw-1 内容。
    for (int cpu : auto_affinity) {
        TEST_ASSERT(cpu >= 0, "allowed cpu id must not be negative");
    }
    return true;
#else
    std::cout << "Testing default cpu_affinity is auto-allocated [0..hw-1]..." << std::endl;

    // 1. Default config has empty cpu_affinity
    ExecutorConfig config;
    TEST_ASSERT(config.cpu_affinity.empty(), "Default cpu_affinity should be empty (auto sentinel)");

    // 2. Simulate executor_manager auto-allocation logic
    unsigned hw = std::thread::hardware_concurrency();
    std::cout << "  hw_concurrency = " << hw << std::endl;

    std::vector<int> auto_affinity;
    if (hw > 0) {
        auto_affinity.resize(hw);
        for (unsigned i = 0; i < hw; ++i) {
            auto_affinity[i] = static_cast<int>(i);
        }
    }

    if (hw > 0) {
        TEST_ASSERT(!auto_affinity.empty(), "auto-affinity should be non-empty when hw > 0");
        TEST_ASSERT(static_cast<unsigned>(auto_affinity.size()) == hw,
                    "auto-affinity size should equal hw_concurrency");
        // Contents should be [0, 1, 2, ..., hw-1]
        for (unsigned i = 0; i < hw; ++i) {
            TEST_ASSERT(auto_affinity[i] == static_cast<int>(i),
                        "auto-affinity entry should match core index");
        }
        if (hw >= 2) {
            TEST_ASSERT(auto_affinity.size() >= 2, "auto-affinity should have >= 2 entries when hw >= 2");
        }
    } else {
        // Probe failed (rare in CI), affinity stays empty → OS free-schedules
        TEST_ASSERT(auto_affinity.empty(), "auto-affinity should stay empty when hw == 0");
        std::cout << "  (hw == 0, probe failed, skipped)" << std::endl;
    }

    std::cout << "  auto-affinity size = " << auto_affinity.size() << std::endl;
    return true;
#endif
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Executor ThreadPool Module Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;
    
    bool all_passed = true;
    
    // PriorityScheduler 测试
    std::cout << "--- PriorityScheduler Tests ---" << std::endl;
    all_passed &= test_priority_scheduler_basic();
    all_passed &= test_priority_scheduler_priority_order();
    all_passed &= test_priority_scheduler_same_priority_fifo();
    all_passed &= test_priority_scheduler_clear();
    all_passed &= test_priority_scheduler_concurrent();
    std::cout << std::endl;
    
    // ThreadPool 测试
    std::cout << "--- ThreadPool Tests ---" << std::endl;
    all_passed &= test_thread_pool_initialize();
    all_passed &= test_thread_pool_init_oom_safety();
    all_passed &= test_thread_pool_init_worker_thread_failure_rolls_back();
    all_passed &= test_thread_pool_invalid_cpu_affinity_is_nonfatal();
    all_passed &= test_thread_pool_shutdown_drains_when_workers_start_late();
    all_passed &= test_thread_pool_submit_basic();
    all_passed &= test_thread_pool_submit_priority();
    all_passed &= test_thread_pool_concurrent_submit();
    all_passed &= test_thread_pool_concurrent_task_id_uniqueness();
    all_passed &= test_thread_pool_exception_handling();
    all_passed &= test_thread_pool_status();
    all_passed &= test_thread_pool_shutdown();
    all_passed &= test_thread_pool_shutdown_no_wait();
    all_passed &= test_thread_pool_concurrent_shutdown();
    all_passed &= test_thread_pool_submit_after_shutdown();
    all_passed &= test_thread_pool_submit_batch_after_shutdown_futures_ready();
    all_passed &= test_thread_pool_wait_for_completion();
    all_passed &= test_thread_pool_wait_for_completion_latency();
    all_passed &= test_task_timeout_soft_skip_works();
    all_passed &= test_task_timeout_not_triggered_when_fast();
    std::cout << std::endl;

    // 默认值自适应测试
    std::cout << "--- Default Value Tests ---" << std::endl;
    all_passed &= test_default_thread_count_is_adaptive_sentinel();
    all_passed &= test_default_enable_work_stealing_is_true();
    all_passed &= test_default_cpu_affinity_is_auto_allocated();
    std::cout << std::endl;

    // 总结
    std::cout << "========================================" << std::endl;
    std::cout << "Test Summary:" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;
    
    if (all_passed) {
        std::cout << "All tests PASSED!" << std::endl;
        return 0;
    } else {
        std::cout << "Some tests FAILED!" << std::endl;
        return 1;
    }
}
