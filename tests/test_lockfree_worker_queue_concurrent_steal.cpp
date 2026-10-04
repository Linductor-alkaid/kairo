#include "kairo/thread_pool/lockfree_worker_queue.hpp"
#include "kairo/task/task.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace kairo;

static bool test_steal_fifo_order() {
    constexpr size_t TASKS = 10;
    LockFreeWorkerQueue queue(32);

    for (size_t i = 0; i < TASKS; ++i) {
        Task task;
        task.task_id = "fifo_" + std::to_string(i);
        task.priority = TaskPriority::NORMAL;
        task.submit_time_ns = static_cast<int64_t>(1000 + i);

        if (!queue.push(task)) {
            std::cerr << "FAILED: push failed at task " << i << std::endl;
            return false;
        }
    }

    std::vector<int64_t> stolen_times;
    stolen_times.reserve(TASKS);

    std::thread stealer([&]() {
        Task task;
        for (size_t i = 0; i < TASKS; ++i) {
            if (!queue.steal(task)) {
                return;
            }
            stolen_times.push_back(task.submit_time_ns);
        }
    });
    stealer.join();

    if (stolen_times.size() != TASKS) {
        std::cerr << "FAILED: stole " << stolen_times.size() << " tasks, expected " << TASKS << std::endl;
        return false;
    }

    for (size_t i = 0; i < TASKS; ++i) {
        const int64_t expected_time = static_cast<int64_t>(1000 + i);
        if (stolen_times[i] != expected_time) {
            std::cerr << "FAILED: steal order at index " << i << " was "
                      << stolen_times[i] << ", expected " << expected_time << std::endl;
            return false;
        }
    }

    return true;
}

int main() {
    if (!test_steal_fifo_order()) {
        return 1;
    }

    constexpr size_t PRODUCERS = 4;
    constexpr size_t TASKS_PER_PRODUCER = 1000;
    constexpr size_t STEALERS = 4;
    constexpr size_t EXPECTED = PRODUCERS * TASKS_PER_PRODUCER;

    LockFreeWorkerQueue queue(EXPECTED + 256);
    std::atomic<size_t> pushed{0};
    std::atomic<size_t> consumed{0};
    std::atomic<size_t> counter{0};
    std::atomic<bool> start{false};
    std::atomic<bool> producers_done{false};
    std::atomic<bool> stop{false};

    std::vector<std::thread> producers;
    producers.reserve(PRODUCERS);
    for (size_t p = 0; p < PRODUCERS; ++p) {
        producers.emplace_back([&, p]() {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            for (size_t i = 0; i < TASKS_PER_PRODUCER; ++i) {
                Task task;
                task.task_id = "task_" + std::to_string(p) + "_" + std::to_string(i);
                task.priority = TaskPriority::NORMAL;
                task.function = [&counter]() {
                    counter.fetch_add(1, std::memory_order_relaxed);
                };

                while (!queue.push(task)) {
                    std::this_thread::yield();
                }
                pushed.fetch_add(1, std::memory_order_release);
            }
        });
    }

    auto consume_task = [&](bool steal) {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        Task task;
        for (;;) {
            const bool got_task = steal ? queue.steal(task) : queue.pop(task);
            if (got_task) {
                if (task.function) {
                    task.function();
                }
                consumed.fetch_add(1, std::memory_order_release);
                continue;
            }

            if (producers_done.load(std::memory_order_acquire) &&
                consumed.load(std::memory_order_acquire) == EXPECTED) {
                break;
            }
            if (stop.load(std::memory_order_acquire)) {
                break;
            }
            std::this_thread::yield();
        }
    };

    std::vector<std::thread> consumers;
    consumers.reserve(STEALERS + 1);
    consumers.emplace_back(consume_task, false);
    for (size_t s = 0; s < STEALERS; ++s) {
        consumers.emplace_back(consume_task, true);
    }

    start.store(true, std::memory_order_release);

    for (auto& producer : producers) {
        producer.join();
    }
    producers_done.store(true, std::memory_order_release);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (consumed.load(std::memory_order_acquire) != EXPECTED &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    stop.store(true, std::memory_order_release);

    for (auto& consumer : consumers) {
        consumer.join();
    }

    if (pushed.load(std::memory_order_acquire) != EXPECTED) {
        std::cerr << "FAILED: pushed " << pushed.load() << " tasks, expected " << EXPECTED << std::endl;
        return 1;
    }
    if (consumed.load(std::memory_order_acquire) != EXPECTED) {
        std::cerr << "FAILED: consumed " << consumed.load() << " tasks, expected " << EXPECTED << std::endl;
        return 1;
    }
    if (counter.load(std::memory_order_acquire) != EXPECTED) {
        std::cerr << "FAILED: executed " << counter.load() << " tasks, expected " << EXPECTED << std::endl;
        return 1;
    }

    std::cout << "PASSED: concurrent pop/steal consumed exactly " << EXPECTED << " tasks" << std::endl;
    return 0;
}
