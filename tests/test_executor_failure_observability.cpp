#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <kairo/executor.hpp>

using namespace kairo;

#define TEST_ASSERT(condition, message)                                      \
    do {                                                                    \
        if (!(condition)) {                                                  \
            std::cerr << "FAILED: " << message << " at " << __FILE__      \
                      << ":" << __LINE__ << std::endl;                     \
            return false;                                                    \
        }                                                                   \
    } while (0)

bool test_failure_status_recent_events_and_callback() {
    std::cout << "Testing Executor failure status and recent events..."
              << std::endl;

    Executor executor;
    executor.set_recent_failure_capacity(2);

    std::atomic<int> callback_count{0};
    std::atomic<bool> callback_saw_expected_kind{true};
    executor.set_failure_callback([&callback_count, &callback_saw_expected_kind](
                                      const ExecutorFailureEvent& event) {
        if (event.kind != FailureKind::SubmitRejected) {
            callback_saw_expected_kind.store(false, std::memory_order_relaxed);
        }
        callback_count.fetch_add(1, std::memory_order_relaxed);
        throw std::runtime_error("observer failure should be isolated");
    });

    TEST_ASSERT(!executor.start_realtime_task("missing_rt_a"),
                "starting missing realtime task should fail visibly");
    TEST_ASSERT(!executor.start_realtime_task("missing_rt_b"),
                "starting second missing realtime task should fail visibly");
    TEST_ASSERT(!executor.start_realtime_task("missing_rt_c"),
                "starting third missing realtime task should fail visibly");

    auto status = executor.get_failure_status();
    TEST_ASSERT(status.total_count == 3, "total failure count should be 3");
    TEST_ASSERT(status.submit_rejected_count == 3,
                "submit rejection count should be 3");
    TEST_ASSERT(status.task_exception_count == 0,
                "task exception count should remain 0 in phase 1 plumbing test");
    TEST_ASSERT(callback_count.load(std::memory_order_relaxed) == 3,
                "callback should be invoked for every event");
    TEST_ASSERT(callback_saw_expected_kind.load(std::memory_order_relaxed),
                "callback should receive submit rejection events");

    auto recent = executor.get_recent_failures();
    TEST_ASSERT(recent.size() == 2, "recent failure buffer should keep capacity");
    TEST_ASSERT(recent[0].executor_name == "missing_rt_b",
                "oldest retained event should be missing_rt_b");
    TEST_ASSERT(recent[1].executor_name == "missing_rt_c",
                "newest retained event should be missing_rt_c");
    TEST_ASSERT(recent[0].timestamp <= recent[1].timestamp,
                "recent events should be ordered from old to new");

    auto latest_one = executor.get_recent_failures(1);
    TEST_ASSERT(latest_one.size() == 1, "max_count should limit result size");
    TEST_ASSERT(latest_one[0].executor_name == "missing_rt_c",
                "limited recent query should return newest event");

    executor.clear_recent_failures();
    TEST_ASSERT(executor.get_recent_failures().empty(),
                "clear_recent_failures should clear only recent events");
    status = executor.get_failure_status();
    TEST_ASSERT(status.total_count == 3,
                "clear_recent_failures should not reset counters");

    std::cout << "  Failure status and recent events: PASSED" << std::endl;
    return true;
}

bool test_recent_failure_capacity_zero_keeps_counters() {
    std::cout << "Testing Executor recent failure capacity zero..." << std::endl;

    Executor executor;
    executor.set_recent_failure_capacity(0);

    TEST_ASSERT(!executor.start_realtime_task("missing_rt"),
                "starting missing realtime task should fail visibly");

    auto status = executor.get_failure_status();
    TEST_ASSERT(status.total_count == 1, "counter should update at capacity 0");
    TEST_ASSERT(status.submit_rejected_count == 1,
                "submit rejection counter should update at capacity 0");
    TEST_ASSERT(executor.get_recent_failures().empty(),
                "capacity 0 should disable recent event retention");

    std::cout << "  Recent failure capacity zero: PASSED" << std::endl;
    return true;
}

bool test_submit_exception_observable_without_future_get() {
    std::cout << "Testing submit exception observable without future.get()..."
              << std::endl;

    Executor executor;
    ExecutorConfig config;
    config.min_threads = 1;
    config.max_threads = 1;
    TEST_ASSERT(executor.initialize(config), "executor should initialize");

    std::atomic<int> callback_count{0};
    executor.set_failure_callback([&callback_count](const ExecutorFailureEvent& event) {
        if (event.kind == FailureKind::TaskException) {
            callback_count.fetch_add(1, std::memory_order_relaxed);
        }
    });

    auto future = executor.submit([]() -> int {
        throw std::runtime_error("submit boom");
    });
    (void)future;

    (void)executor.wait_for_completion(std::chrono::seconds{300});

    auto failure_status = executor.get_failure_status();
    TEST_ASSERT(failure_status.task_exception_count == 1,
                "submit exception should increment facade task exception count");
    TEST_ASSERT(callback_count.load(std::memory_order_relaxed) == 1,
                "submit exception should invoke failure callback");

    auto async_status = executor.get_async_executor_status();
    TEST_ASSERT(async_status.failed_tasks == 1,
                "submit exception should increment async failed_tasks");

    auto recent = executor.get_recent_failures(1);
    TEST_ASSERT(recent.size() == 1, "recent failure should contain submit exception");
    TEST_ASSERT(recent[0].kind == FailureKind::TaskException,
                "recent failure kind should be TaskException");
    TEST_ASSERT(recent[0].exception != nullptr,
                "recent failure should retain exception_ptr");

    executor.shutdown();

    std::cout << "  Submit exception observable: PASSED" << std::endl;
    return true;
}

bool test_submit_priority_exception_observable() {
    std::cout << "Testing submit_priority exception observable..." << std::endl;

    Executor executor;
    ExecutorConfig config;
    config.min_threads = 1;
    config.max_threads = 1;
    TEST_ASSERT(executor.initialize(config), "executor should initialize");

    auto future = executor.submit_priority(3, []() {
        throw std::runtime_error("priority boom");
    });
    (void)future;

    (void)executor.wait_for_completion(std::chrono::seconds{300});

    auto failure_status = executor.get_failure_status();
    TEST_ASSERT(failure_status.task_exception_count == 1,
                "priority exception should increment task exception count");

    auto async_status = executor.get_async_executor_status();
    TEST_ASSERT(async_status.failed_tasks == 1,
                "priority exception should increment async failed_tasks");

    executor.shutdown();

    std::cout << "  Submit priority exception observable: PASSED" << std::endl;
    return true;
}

bool test_submit_batch_partial_exception_observable() {
    std::cout << "Testing submit_batch partial exception observable..." << std::endl;

    Executor executor;
    ExecutorConfig config;
    config.min_threads = 2;
    config.max_threads = 2;
    TEST_ASSERT(executor.initialize(config), "executor should initialize");

    std::vector<std::function<void()>> tasks;
    tasks.push_back([]() {});
    tasks.push_back([]() { throw std::runtime_error("batch boom"); });
    tasks.push_back([]() {});

    auto futures = executor.submit_batch(tasks);
    (void)executor.wait_for_completion(std::chrono::seconds{300});

    size_t throwing_futures = 0;
    for (auto& future : futures) {
        try {
            future.get();
        } catch (const std::exception&) {
            ++throwing_futures;
        }
    }

    TEST_ASSERT(throwing_futures == 1, "one batch future should throw");

    auto failure_status = executor.get_failure_status();
    TEST_ASSERT(failure_status.task_exception_count == 1,
                "batch exception should increment task exception count once");

    auto async_status = executor.get_async_executor_status();
    TEST_ASSERT(async_status.failed_tasks == 1,
                "batch exception should increment async failed_tasks once");

    executor.shutdown();

    std::cout << "  Submit batch partial exception observable: PASSED"
              << std::endl;
    return true;
}

bool test_submit_batch_no_future_exception_observable() {
    std::cout << "Testing submit_batch_no_future exception observable..."
              << std::endl;

    Executor executor;
    ExecutorConfig config;
    config.min_threads = 2;
    config.max_threads = 2;
    TEST_ASSERT(executor.initialize(config), "executor should initialize");

    std::atomic<int> callback_count{0};
    executor.set_failure_callback([&callback_count](const ExecutorFailureEvent& event) {
        if (event.kind == FailureKind::TaskException) {
            callback_count.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::vector<std::function<void()>> tasks;
    tasks.push_back([]() {});
    tasks.push_back([]() { throw std::runtime_error("no future boom"); });

    executor.submit_batch_no_future(tasks);
    (void)executor.wait_for_completion(std::chrono::seconds{300});

    auto failure_status = executor.get_failure_status();
    TEST_ASSERT(failure_status.task_exception_count == 1,
                "no-future batch exception should increment task exception count");
    TEST_ASSERT(callback_count.load(std::memory_order_relaxed) == 1,
                "no-future batch exception should invoke callback");

    auto async_status = executor.get_async_executor_status();
    TEST_ASSERT(async_status.failed_tasks == 1,
                "no-future batch exception should increment async failed_tasks");

    executor.shutdown();

    std::cout << "  Submit batch no future exception observable: PASSED"
              << std::endl;
    return true;
}

bool test_submit_rejections_observable() {
    std::cout << "Testing async submit rejections observable..." << std::endl;

    Executor executor;
    auto future = executor.submit([]() {});
    future.get();
    executor.shutdown();

    bool submit_threw = false;
    try {
        auto rejected = executor.submit([]() {});
        (void)rejected;
    } catch (const std::exception&) {
        submit_threw = true;
    }
    TEST_ASSERT(submit_threw, "submit after shutdown should throw");

    auto status = executor.get_failure_status();
    TEST_ASSERT(status.submit_rejected_count == 1,
                "submit after shutdown should increment rejection count");

    Executor batch_executor;
    std::vector<std::function<void()>> empty_tasks;
    auto futures = batch_executor.submit_batch(empty_tasks);
    TEST_ASSERT(futures.empty(), "empty batch should return no futures");

    status = batch_executor.get_failure_status();
    TEST_ASSERT(status.submit_rejected_count == 1,
                "empty batch should increment rejection count");

    batch_executor.shutdown();

    std::cout << "  Async submit rejections observable: PASSED" << std::endl;
    return true;
}

int main() {
    bool all_passed = true;
    all_passed &= test_failure_status_recent_events_and_callback();
    all_passed &= test_recent_failure_capacity_zero_keeps_counters();
    all_passed &= test_submit_exception_observable_without_future_get();
    all_passed &= test_submit_priority_exception_observable();
    all_passed &= test_submit_batch_partial_exception_observable();
    all_passed &= test_submit_batch_no_future_exception_observable();
    all_passed &= test_submit_rejections_observable();

    if (all_passed) {
        std::cout << "All executor failure observability tests passed."
                  << std::endl;
        return 0;
    }

    std::cerr << "Some executor failure observability tests failed."
              << std::endl;
    return 1;
}
