#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <kairo/config.hpp>
#include <kairo/executor.hpp>
#include <kairo/thread_pool/thread_pool.hpp>

using namespace kairo;

#define TEST_ASSERT(condition, message)                                      \
    do {                                                                     \
        if (!(condition)) {                                                   \
            std::cerr << "FAILED: " << message << " at " << __FILE__       \
                      << ":" << __LINE__ << std::endl;                       \
            return false;                                                     \
        }                                                                    \
    } while (0)

namespace {

bool is_ready(std::future<void>& future) {
    return future.wait_for(std::chrono::milliseconds(0)) ==
           std::future_status::ready;
}

bool future_throws_empty_task(std::future<void>& future) {
    try {
        future.get();
    } catch (const std::invalid_argument& ex) {
        return std::string(ex.what()).find("empty task") != std::string::npos;
    } catch (const std::exception& ex) {
        std::cerr << "future.get() threw unexpected exception: "
                  << ex.what() << std::endl;
        return false;
    }

    std::cerr << "future.get() did not throw" << std::endl;
    return false;
}

bool test_thread_pool_rejects_empty_task() {
    std::cout << "Testing ThreadPool rejects empty task..." << std::endl;

    ThreadPool pool;
    ThreadPoolConfig config;
    config.min_threads = 2;
    config.max_threads = 2;
    TEST_ASSERT(pool.initialize(config), "thread pool should initialize");

    const ThreadPoolStatus before = pool.get_status();
    std::exception_ptr rejection;
    const bool accepted =
        pool.try_submit(std::function<void()>{},
                        [&rejection](std::exception_ptr exception) {
                            rejection = exception;
                        });
    const ThreadPoolStatus after = pool.get_status();

    TEST_ASSERT(!accepted, "empty task should be rejected");
    TEST_ASSERT(after.total_tasks == before.total_tasks,
                "rejected empty task should not increment total_tasks");
    TEST_ASSERT(after.completed_tasks == before.completed_tasks,
                "rejected empty task should not increment completed_tasks");
    TEST_ASSERT(after.failed_tasks == before.failed_tasks,
                "rejected empty task should not increment failed_tasks");
    TEST_ASSERT(after.queue_size == before.queue_size,
                "rejected empty task should not enter the queue");
    TEST_ASSERT(rejection != nullptr,
                "empty task rejection should report an exception");
    try {
        std::rethrow_exception(rejection);
    } catch (const std::invalid_argument& ex) {
        TEST_ASSERT(std::string(ex.what()).find("empty task") != std::string::npos,
                    "empty task rejection should use std::invalid_argument");
    } catch (...) {
        TEST_ASSERT(false, "empty task rejection should use std::invalid_argument");
    }

    pool.shutdown();

    std::cout << "  ThreadPool rejects empty task: PASSED" << std::endl;
    return true;
}

bool test_thread_pool_rejects_batch_with_empty_task() {
    std::cout << "Testing ThreadPool rejects batch with empty task..."
              << std::endl;

    ThreadPool pool;
    ThreadPoolConfig config;
    config.min_threads = 2;
    config.max_threads = 2;
    TEST_ASSERT(pool.initialize(config), "thread pool should initialize");

    std::atomic<int> ran{0};
    std::vector<std::function<void()>> tasks;
    tasks.push_back([&ran]() { ran.fetch_add(1, std::memory_order_relaxed); });
    tasks.push_back(std::function<void()>{});
    tasks.push_back([&ran]() { ran.fetch_add(1, std::memory_order_relaxed); });

    const ThreadPoolStatus before = pool.get_status();
    TEST_ASSERT(!pool.try_submit_batch(tasks),
                "batch containing an empty task should be rejected");
    pool.wait_for_completion();
    const ThreadPoolStatus after = pool.get_status();

    TEST_ASSERT(ran.load(std::memory_order_relaxed) == 0,
                "rejected batch should not partially submit valid tasks");
    TEST_ASSERT(after.total_tasks == before.total_tasks,
                "rejected batch should not increment total_tasks");
    TEST_ASSERT(after.completed_tasks == before.completed_tasks,
                "rejected batch should not increment completed_tasks");
    TEST_ASSERT(after.failed_tasks == before.failed_tasks,
                "rejected batch should not increment failed_tasks");

    std::vector<std::function<void()>> valid_tasks;
    valid_tasks.push_back(
        [&ran]() { ran.fetch_add(1, std::memory_order_relaxed); });
    valid_tasks.push_back(
        [&ran]() { ran.fetch_add(1, std::memory_order_relaxed); });
    TEST_ASSERT(pool.try_submit_batch(std::move(valid_tasks)),
                "pool should accept a subsequent valid batch");
    pool.wait_for_completion();
    TEST_ASSERT(ran.load(std::memory_order_relaxed) == 2,
                "subsequent valid batch should run");

    pool.shutdown();

    std::cout << "  ThreadPool rejects batch with empty task: PASSED"
              << std::endl;
    return true;
}

bool test_executor_facade_empty_batch_future_is_exception() {
    std::cout << "Testing Executor facade empty task futures are exceptions..."
              << std::endl;

    Executor executor;
    ExecutorConfig config;
    config.min_threads = 2;
    config.max_threads = 2;
    TEST_ASSERT(executor.initialize(config), "executor should initialize");

    std::function<void()> empty;
    auto single = executor.submit(empty);
    TEST_ASSERT(is_ready(single), "empty submit future should be ready");
    TEST_ASSERT(future_throws_empty_task(single),
                "empty submit future should throw std::invalid_argument");

    std::atomic<int> ran{0};
    std::vector<std::function<void()>> tasks;
    tasks.push_back([&ran]() { ran.fetch_add(1, std::memory_order_relaxed); });
    tasks.push_back(std::function<void()>{});
    tasks.push_back([&ran]() { ran.fetch_add(1, std::memory_order_relaxed); });

    auto futures = executor.submit_batch(tasks);
    TEST_ASSERT(futures.size() == tasks.size(),
                "submit_batch should return one future per task");
    for (auto& future : futures) {
        TEST_ASSERT(is_ready(future),
                    "empty batch rejection futures should be ready");
        TEST_ASSERT(future_throws_empty_task(future),
                    "empty batch futures should throw std::invalid_argument");
    }

    (void)executor.wait_for_completion(std::chrono::seconds{300});
    TEST_ASSERT(ran.load(std::memory_order_relaxed) == 0,
                "empty batch should not partially submit valid tasks");

    const ExecutorFailureStatus status = executor.get_failure_status();
    TEST_ASSERT(status.submit_rejected_count >= 2,
                "facade should record empty submit and batch rejections");

    executor.shutdown();

    std::cout << "  Executor facade empty task futures are exceptions: PASSED"
              << std::endl;
    return true;
}

bool test_executor_facade_valid_task_still_works() {
    std::cout << "Testing Executor facade valid task still works..."
              << std::endl;

    Executor executor;
    ExecutorConfig config;
    config.min_threads = 2;
    config.max_threads = 2;
    TEST_ASSERT(executor.initialize(config), "executor should initialize");

    auto single = executor.submit([]() {});
    TEST_ASSERT(single.wait_for(std::chrono::seconds(1)) ==
                    std::future_status::ready,
                "valid submit future should become ready");
    single.get();

    std::atomic<int> ran{0};
    std::vector<std::function<void()>> tasks;
    tasks.push_back([&ran]() { ran.fetch_add(1, std::memory_order_relaxed); });
    tasks.push_back([&ran]() { ran.fetch_add(1, std::memory_order_relaxed); });

    auto futures = executor.submit_batch(tasks);
    for (auto& future : futures) {
        TEST_ASSERT(future.wait_for(std::chrono::seconds(1)) ==
                        std::future_status::ready,
                    "valid batch future should become ready");
        future.get();
    }
    TEST_ASSERT(ran.load(std::memory_order_relaxed) == 2,
                "valid batch tasks should run");

    executor.shutdown();

    std::cout << "  Executor facade valid task still works: PASSED"
              << std::endl;
    return true;
}

} // namespace

int main() {
    bool all_passed = true;
    all_passed &= test_thread_pool_rejects_empty_task();
    all_passed &= test_thread_pool_rejects_batch_with_empty_task();
    all_passed &= test_executor_facade_empty_batch_future_is_exception();
    all_passed &= test_executor_facade_valid_task_still_works();

    if (all_passed) {
        std::cout << "All ThreadPool empty task rejection tests passed."
                  << std::endl;
        return 0;
    }

    std::cerr << "Some ThreadPool empty task rejection tests failed."
              << std::endl;
    return 1;
}