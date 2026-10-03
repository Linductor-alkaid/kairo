#include <cassert>
#include <chrono>
#include <iostream>
#include <future>
#include <thread>
#include <atomic>

#include <kairo/config.hpp>
#include <kairo/types.hpp>
#include <kairo/interfaces.hpp>
#include "kairo/realtime_thread_executor.hpp"
#include "mock_cycle_manager.hpp"

using namespace kairo;
using namespace kairo::test;

#define TEST_ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            std::cerr << "FAILED: " << message << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
            return false; \
        } \
    } while(0)

bool stop_and_join_within(RealtimeThreadExecutor& executor, std::chrono::milliseconds timeout) {
    std::promise<bool> result;
    auto completed = result.get_future();
    std::thread stopper([&executor, &result]() {
        result.set_value(executor.stop_and_join());
    });
    const bool completed_in_time = completed.wait_for(timeout) == std::future_status::ready;
    if (completed_in_time) {
        const bool stopped = completed.get();
        stopper.join();
        return stopped;
    }
    stopper.detach();
    return false;
}

bool test_realtime_executor_with_cycle_manager() {
    std::cout << "Testing RealtimeThreadExecutor with ICycleManager..." << std::endl;

    MockCycleManager mock_manager;

    RealtimeThreadConfig config;
    config.thread_name = "test_cycle_manager";
    config.cycle_period_ns = 10000000;  // 10ms
    config.thread_priority = 0;
    config.cycle_manager = &mock_manager;
    config.cycle_callback = []() {};

    RealtimeThreadExecutor executor("test_executor", config);

    TEST_ASSERT(executor.get_name() == "test_executor", "Executor name should match");
    TEST_ASSERT(executor.start(), "Executor should start successfully");

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto status = executor.get_status();
    TEST_ASSERT(status.is_running == true, "Executor should be running with cycle manager");
    TEST_ASSERT(status.cycle_count > 0, "Cycle count should increase");

    executor.stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    status = executor.get_status();
    TEST_ASSERT(status.is_running == false, "Executor should stop cleanly");

    std::cout << "  RealtimeThreadExecutor with ICycleManager: PASSED" << std::endl;
    return true;
}

bool test_realtime_executor_cycle_manager_stop() {
    std::cout << "Testing RealtimeThreadExecutor cycle manager stop..." << std::endl;

    MockCycleManager mock_manager;

    RealtimeThreadConfig config;
    config.thread_name = "test_stop";
    config.cycle_period_ns = 5000000;  // 5ms
    config.thread_priority = 0;
    config.cycle_manager = &mock_manager;
    config.cycle_callback = []() {};

    RealtimeThreadExecutor executor("test_stop_executor", config);
    TEST_ASSERT(executor.start(), "Start should succeed");

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    executor.stop();

    auto status = executor.get_status();
    TEST_ASSERT(status.is_running == false, "Executor should be stopped");
    TEST_ASSERT(status.cycle_count > 0, "Should have run at least one cycle");

    std::cout << "  Cycle manager stop: PASSED" << std::endl;
    return true;
}

bool test_realtime_executor_cycle_loop_execution() {
    std::cout << "Testing RealtimeThreadExecutor cycle_loop execution..." << std::endl;

    MockCycleManager mock_manager;
    std::atomic<int> cycle_count{0};

    RealtimeThreadConfig config;
    config.thread_name = "test_cycle_loop";
    config.cycle_period_ns = 5000000;  // 5ms
    config.thread_priority = 0;
    config.cycle_manager = &mock_manager;
    config.cycle_callback = [&cycle_count]() { cycle_count.fetch_add(1); };

    RealtimeThreadExecutor executor("test_cycle_loop_executor", config);
    TEST_ASSERT(executor.start(), "Start should succeed");

    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    executor.stop();
    // 在 stop() 之后再读取回调次数，避免竞态：stop() 前读取时工作线程可能多跑一个周期，
    // 导致 status.cycle_count 比当时读到的值多 1（CI 环境更容易触发）。
    int final_callback_count = cycle_count.load();
    auto status = executor.get_status();

    TEST_ASSERT(final_callback_count > 0, "Cycle callback should have been called");
    TEST_ASSERT(status.cycle_count == final_callback_count,
                "Executor cycle count should match callback invocations");

    std::cout << "  Cycle_loop execution: PASSED" << std::endl;
    return true;
}

bool test_realtime_executor_fallback_to_simple() {
    std::cout << "Testing RealtimeThreadExecutor fallback to simple..." << std::endl;

    MockCycleManager mock_manager;
    mock_manager.fail_register = true;

    std::atomic<int> fallback_cycles{0};

    RealtimeThreadConfig config;
    config.thread_name = "test_fallback";
    config.cycle_period_ns = 5000000;
    config.thread_priority = 0;
    config.cycle_manager = &mock_manager;
    config.cycle_callback = [&fallback_cycles]() { fallback_cycles.fetch_add(1); };

    RealtimeThreadExecutor executor("test_fallback_executor", config);
    TEST_ASSERT(executor.start(), "Start should succeed despite register failure");

    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    int count = fallback_cycles.load();
    executor.stop();

    TEST_ASSERT(count > 0, "Should run simple_cycle_loop when register fails");
    auto status = executor.get_status();
    TEST_ASSERT(status.cycle_count > 0, "Executor should have cycle count from fallback");

    std::cout << "  Fallback to simple: PASSED" << std::endl;
    return true;
}

bool test_realtime_executor_fallback_start_fails() {
    std::cout << "Testing RealtimeThreadExecutor fallback when start_cycle fails..." << std::endl;

    MockCycleManager mock_manager;
    mock_manager.fail_start = true;

    std::atomic<int> fallback_cycles{0};

    RealtimeThreadConfig config;
    config.thread_name = "test_fallback_start";
    config.cycle_period_ns = 5000000;
    config.thread_priority = 0;
    config.cycle_manager = &mock_manager;
    config.cycle_callback = [&fallback_cycles]() { fallback_cycles.fetch_add(1); };

    RealtimeThreadExecutor executor("test_fallback_start_executor", config);
    TEST_ASSERT(executor.start(), "Start should succeed");

    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    int count = fallback_cycles.load();
    executor.stop();

    TEST_ASSERT(count > 0, "Should run simple_cycle_loop when start_cycle fails");

    std::cout << "  Fallback when start_cycle fails: PASSED" << std::endl;
    return true;
}

bool test_throwing_cycle_manager_register() {
    std::cout << "Testing ThrowingCycleManagerRegister..." << std::endl;
    MockCycleManager mock_manager;
    mock_manager.throw_register = true;
    RealtimeThreadConfig config{"throw_register", 5000000, 0, {}, []() {}, &mock_manager};
    RealtimeThreadExecutor executor("throw_register_executor", config);
    TEST_ASSERT(executor.start(), "Start should isolate register_cycle exceptions");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    TEST_ASSERT(stop_and_join_within(executor, std::chrono::seconds(1)),
                "stop_and_join should complete within one second after register exception");
    TEST_ASSERT(executor.get_status().cycle_manager_error_count > 0,
                "register_cycle exception should be observable");
    std::cout << "  ThrowingCycleManagerRegister: PASSED" << std::endl;
    return true;
}

bool test_throwing_cycle_manager_start() {
    std::cout << "Testing ThrowingCycleManagerStart..." << std::endl;
    MockCycleManager mock_manager;
    mock_manager.throw_start = true;
    RealtimeThreadConfig config{"throw_start", 5000000, 0, {}, []() {}, &mock_manager};
    RealtimeThreadExecutor executor("throw_start_executor", config);
    TEST_ASSERT(executor.start(), "Start should isolate start_cycle exceptions");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    TEST_ASSERT(stop_and_join_within(executor, std::chrono::seconds(1)),
                "stop_and_join should complete within one second after start exception");
    TEST_ASSERT(executor.get_status().cycle_manager_error_count > 0,
                "start_cycle exception should be observable");
    std::cout << "  ThrowingCycleManagerStart: PASSED" << std::endl;
    return true;
}

bool test_throwing_cycle_manager_stop() {
    std::cout << "Testing ThrowingCycleManagerStop..." << std::endl;
    MockCycleManager mock_manager;
    mock_manager.throw_stop = true;
    RealtimeThreadConfig config{"throw_stop", 5000000, 0, {}, []() {}, &mock_manager};
    RealtimeThreadExecutor executor("throw_stop_executor", config);
    TEST_ASSERT(executor.start(), "Start should succeed");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    TEST_ASSERT(stop_and_join_within(executor, std::chrono::seconds(1)),
                "stop_and_join should complete within one second after stop exception");
    TEST_ASSERT(executor.get_status().cycle_manager_error_count > 0,
                "stop_cycle exception should be observable");
    TEST_ASSERT(!executor.get_status().is_running, "Executor should stop after stop_cycle exception");
    std::cout << "  ThrowingCycleManagerStop: PASSED" << std::endl;
    return true;
}

bool test_callback_self_stop_with_cycle_manager() {
    std::cout << "Testing CallbackSelfStopWithCycleManager..." << std::endl;
    MockCycleManager mock_manager;
    std::atomic<RealtimeThreadExecutor*> executor_ptr{nullptr};
    std::atomic<bool> callback_returned{false};
    RealtimeThreadConfig config;
    config.thread_name = "self_stop";
    config.cycle_period_ns = 5000000;
    config.cycle_manager = &mock_manager;
    config.cycle_callback = [&]() {
        if (auto* executor = executor_ptr.load(std::memory_order_acquire)) {
            executor->stop_and_join();
            callback_returned.store(true, std::memory_order_release);
        }
    };
    RealtimeThreadExecutor executor("self_stop_executor", config);
    executor_ptr.store(&executor, std::memory_order_release);
    TEST_ASSERT(executor.start(), "Start should succeed");
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    TEST_ASSERT(callback_returned.load(std::memory_order_acquire),
                "Self-stop callback should return without deadlock");
    TEST_ASSERT(stop_and_join_within(executor, std::chrono::seconds(1)),
                "External join should complete self-stop within one second");
    TEST_ASSERT(!executor.get_status().is_running, "Executor should remain stopped");
    std::cout << "  CallbackSelfStopWithCycleManager: PASSED" << std::endl;
    return true;
}

int main() {
    std::cout << "========== ICycleManager 集成测试 ==========" << std::endl;
    std::cout << std::endl;

    bool all_passed = true;
    all_passed &= test_realtime_executor_with_cycle_manager();
    all_passed &= test_realtime_executor_cycle_manager_stop();
    all_passed &= test_realtime_executor_cycle_loop_execution();
    all_passed &= test_realtime_executor_fallback_to_simple();
    all_passed &= test_realtime_executor_fallback_start_fails();
    all_passed &= test_throwing_cycle_manager_register();
    all_passed &= test_throwing_cycle_manager_start();
    all_passed &= test_throwing_cycle_manager_stop();
    all_passed &= test_callback_self_stop_with_cycle_manager();

    std::cout << std::endl;
    if (all_passed) {
        std::cout << "========== 所有 ICycleManager 集成测试通过 ==========" << std::endl;
        return 0;
    } else {
        std::cout << "========== 部分测试失败 ==========" << std::endl;
        return 1;
    }
}
