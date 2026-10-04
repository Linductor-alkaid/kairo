#include <cassert>
#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <stdexcept>
#include <chrono>

// 包含util模块的头文件
#include "kairo/util/lockfree_queue.hpp"
#include "kairo/util/exception_handler.hpp"
#include "kairo/util/thread_utils.hpp"

using namespace kairo::util;

// 测试辅助宏
#define TEST_ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            std::cerr << "FAILED: " << message << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
            return false; \
        } \
    } while(0)

// ========== LockFreeQueue 测试 ==========

bool test_lockfree_queue_basic() {
    std::cout << "Testing LockFreeQueue basic operations..." << std::endl;

    PAUSE_INSTRUCTION();
    
    LockFreeQueue<int> queue(8);
    
    // 测试空队列
    TEST_ASSERT(queue.empty(), "Queue should be empty initially");
    TEST_ASSERT(queue.size() == 0, "Queue size should be 0 initially");
    
    // 测试push
    TEST_ASSERT(queue.push(1), "Should be able to push first element");
    TEST_ASSERT(!queue.empty(), "Queue should not be empty after push");
    TEST_ASSERT(queue.size() == 1, "Queue size should be 1");
    
    // 测试pop
    int value = 0;
    TEST_ASSERT(queue.pop(value), "Should be able to pop element");
    TEST_ASSERT(value == 1, "Popped value should be 1");
    TEST_ASSERT(queue.empty(), "Queue should be empty after pop");
    
    // 测试多个元素
    for (int i = 0; i < 5; ++i) {
        TEST_ASSERT(queue.push(i), "Should be able to push element");
    }
    TEST_ASSERT(queue.size() == 5, "Queue size should be 5");
    
    for (int i = 0; i < 5; ++i) {
        int v = -1;
        TEST_ASSERT(queue.pop(v), "Should be able to pop element");
        TEST_ASSERT(v == i, "Popped value should match");
    }
    
    std::cout << "  LockFreeQueue basic operations: PASSED" << std::endl;
    return true;
}

bool test_lockfree_queue_capacity() {
    std::cout << "Testing LockFreeQueue capacity..." << std::endl;
    
    LockFreeQueue<int> queue(4);
    
    // 填充队列到容量
    for (int i = 0; i < 3; ++i) {  // 容量-1，因为环形缓冲区需要留一个空位
        TEST_ASSERT(queue.push(i), "Should be able to push element");
    }
    
    // 尝试推入超出容量的元素应该失败
    TEST_ASSERT(!queue.push(999), "Should not be able to push when queue is full");
    
    // 弹出后应该可以继续推入
    int value = 0;
    TEST_ASSERT(queue.pop(value), "Should be able to pop element");
    TEST_ASSERT(queue.push(999), "Should be able to push after pop");
    
    std::cout << "  LockFreeQueue capacity: PASSED" << std::endl;
    return true;
}

bool test_lockfree_queue_concurrent() {
    std::cout << "Testing LockFreeQueue concurrent operations..." << std::endl;
    
    LockFreeQueue<int> queue(1024);
    const int num_elements = 1000;
    std::atomic<int> push_count{0};
    std::atomic<int> pop_count{0};
    
    // 生产者线程
    std::thread producer([&]() {
        for (int i = 0; i < num_elements; ++i) {
            while (!queue.push(i)) {
                std::this_thread::yield();
            }
            push_count.fetch_add(1);
        }
    });
    
    // 消费者线程
    std::thread consumer([&]() {
        int value = 0;
        while (pop_count.load() < num_elements) {
            if (queue.pop(value)) {
                pop_count.fetch_add(1);
            } else {
                std::this_thread::yield();
            }
        }
    });
    
    producer.join();
    consumer.join();
    
    TEST_ASSERT(push_count.load() == num_elements, "All elements should be pushed");
    TEST_ASSERT(pop_count.load() == num_elements, "All elements should be popped");
    TEST_ASSERT(queue.empty(), "Queue should be empty after all operations");
    
    std::cout << "  LockFreeQueue concurrent operations: PASSED" << std::endl;
    return true;
}

// ========== ExceptionHandler 测试 ==========

bool test_exception_handler_basic() {
    std::cout << "Testing ExceptionHandler basic operations..." << std::endl;
    
    ExceptionHandler handler;
    
    // 测试异常处理（无回调）
    try {
        throw std::runtime_error("Test exception");
    } catch (...) {
        handler.handle_task_exception("test_executor", std::current_exception());
    }
    
    // 测试超时处理（无回调）
    handler.handle_task_timeout("test_executor", "task_1");
    
    std::cout << "  ExceptionHandler basic operations: PASSED" << std::endl;
    return true;
}

bool test_exception_handler_callback() {
    std::cout << "Testing ExceptionHandler callback..." << std::endl;
    
    ExceptionHandler handler;
    std::atomic<bool> exception_called{false};
    std::atomic<bool> timeout_called{false};
    std::string caught_executor_name;
    std::string caught_task_id;
    std::string caught_exception_message;
    bool exception_message_valid = false;
    
    // 设置异常回调
    handler.set_exception_callback([&](const std::string& name, std::exception_ptr ex) {
        exception_called = true;
        caught_executor_name = name;
        try {
            std::rethrow_exception(ex);
        } catch (const std::exception& e) {
            // 记录异常消息，在回调外部验证（不能在void lambda中使用TEST_ASSERT）
            caught_exception_message = e.what();
            exception_message_valid = (std::string(e.what()) == "Test exception");
        }
    });
    
    // 设置超时回调
    handler.set_timeout_callback([&](const std::string& name, const std::string& task_id) {
        timeout_called = true;
        caught_executor_name = name;
        caught_task_id = task_id;
    });
    
    // 触发异常
    try {
        throw std::runtime_error("Test exception");
    } catch (...) {
        handler.handle_task_exception("test_executor", std::current_exception());
    }
    
    TEST_ASSERT(exception_called.load(), "Exception callback should be called");
    TEST_ASSERT(caught_executor_name == "test_executor", "Executor name should match");
    TEST_ASSERT(exception_message_valid, "Exception message should match");
    TEST_ASSERT(caught_exception_message == "Test exception", "Exception message should be 'Test exception'");
    
    // 触发超时
    handler.handle_task_timeout("test_executor", "task_1");
    TEST_ASSERT(timeout_called.load(), "Timeout callback should be called");
    TEST_ASSERT(caught_executor_name == "test_executor", "Executor name should match");
    TEST_ASSERT(caught_task_id == "task_1", "Task ID should match");
    
    std::cout << "  ExceptionHandler callback: PASSED" << std::endl;
    return true;
}

bool test_exception_handler_thread_safe() {
    std::cout << "Testing ExceptionHandler thread safety..." << std::endl;
    
    ExceptionHandler handler;
    std::atomic<int> callback_count{0};
    
    // 设置回调
    handler.set_exception_callback([&](const std::string&, std::exception_ptr) {
        callback_count.fetch_add(1);
    });
    
    // 多个线程同时调用
    const int num_threads = 10;
    const int calls_per_thread = 100;
    std::vector<std::thread> threads;
    
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&]() {
            for (int j = 0; j < calls_per_thread; ++j) {
                try {
                    throw std::runtime_error("Test");
                } catch (...) {
                    handler.handle_task_exception("executor", std::current_exception());
                }
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    TEST_ASSERT(callback_count.load() == num_threads * calls_per_thread,
               "All callbacks should be called");
    
    std::cout << "  ExceptionHandler thread safety: PASSED" << std::endl;
    return true;
}

// ========== ThreadUtils 测试 ==========

bool test_thread_utils_priority() {
    std::cout << "Testing ThreadUtils priority..." << std::endl;
    
    // 获取当前线程优先级
    int current_priority = get_current_thread_priority();
    std::cout << "  Current thread priority: " << current_priority << std::endl;
    
    // 测试在新线程中获取优先级
    std::thread test_thread([&]() {
        int new_priority = get_current_thread_priority();
        std::cout << "  New thread priority: " << new_priority << std::endl;
    });
    
    // 尝试设置优先级（可能失败，取决于权限）
    auto thread_handle = test_thread.native_handle();
    bool result = set_thread_priority(thread_handle, 0);
    std::cout << "  Set priority result: " << (result ? "success" : "failed") << std::endl;
    
    test_thread.join();
    
    std::cout << "  ThreadUtils priority: PASSED (may fail on some systems due to permissions)" << std::endl;
    return true;
}

bool test_thread_utils_affinity() {
    std::cout << "Testing ThreadUtils affinity..." << std::endl;
    
    // 获取当前线程亲和性
    std::vector<int> current_affinity = get_current_thread_affinity();
    std::cout << "  Current thread affinity: ";
    for (int cpu : current_affinity) {
        std::cout << cpu << " ";
    }
    std::cout << std::endl;
    
    // 测试在新线程中获取亲和性
    std::thread test_thread([&]() {
        std::vector<int> new_affinity = get_current_thread_affinity();
        std::cout << "  New thread affinity: ";
        for (int cpu : new_affinity) {
            std::cout << cpu << " ";
        }
        std::cout << std::endl;
    });
    
    // 尝试设置亲和性（可能失败，取决于权限）
    auto thread_handle = test_thread.native_handle();
    if (!current_affinity.empty()) {
        std::vector<int> single_cpu = {current_affinity[0]};
        bool result = set_cpu_affinity(thread_handle, single_cpu);
        std::cout << "  Set affinity result: " << (result ? "success" : "failed") << std::endl;
    }
    
    test_thread.join();
    
    std::cout << "  ThreadUtils affinity: PASSED (may fail on some systems due to permissions)" << std::endl;
    return true;
}

// ========== 主测试函数 ==========

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Executor Util Module Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;
    
    bool all_passed = true;
    
    // LockFreeQueue 测试
    std::cout << "--- LockFreeQueue Tests ---" << std::endl;
    all_passed &= test_lockfree_queue_basic();
    all_passed &= test_lockfree_queue_capacity();
    all_passed &= test_lockfree_queue_concurrent();
    std::cout << std::endl;
    
    // ExceptionHandler 测试
    std::cout << "--- ExceptionHandler Tests ---" << std::endl;
    all_passed &= test_exception_handler_basic();
    all_passed &= test_exception_handler_callback();
    all_passed &= test_exception_handler_thread_safe();
    std::cout << std::endl;
    
    // ThreadUtils 测试
    std::cout << "--- ThreadUtils Tests ---" << std::endl;
    all_passed &= test_thread_utils_priority();
    all_passed &= test_thread_utils_affinity();
    std::cout << std::endl;
    
    // 总结
    std::cout << "========================================" << std::endl;
    if (all_passed) {
        std::cout << "All tests PASSED!" << std::endl;
        return 0;
    } else {
        std::cout << "Some tests FAILED!" << std::endl;
        return 1;
    }
}
