#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

#define private public
#include <kairo/executor.hpp>
#undef private

using namespace kairo;

#define TEST_ASSERT(condition, message)                                           \
    do {                                                                         \
        if (!(condition)) {                                                       \
            std::cerr << "FAILED: " << message << " at " << __FILE__ << ":"    \
                      << __LINE__ << std::endl;                                  \
            return false;                                                        \
        }                                                                        \
    } while (0)

class BatchObservingAsyncExecutor : public IAsyncExecutor {
public:
    std::string get_name() const override {
        return "batch_observer";
    }

    AsyncExecutorStatus get_status() const override {
        AsyncExecutorStatus status;
        status.name = get_name();
        status.is_running = running_;
        status.completed_tasks = completed_tasks_;
        return status;
    }

    bool start() override {
        running_ = true;
        return true;
    }

    void stop() override {
        running_ = false;
    }

    void wait_for_completion() override {}

    int submit_call_count() const {
        return submit_call_count_;
    }

    int try_batch_call_count() const {
        return try_batch_call_count_;
    }

    int batch_impl_call_count() const {
        return batch_impl_call_count_;
    }

    size_t last_batch_size() const {
        return last_batch_size_;
    }

protected:
    void submit_impl(std::function<void()> task) override {
        ++submit_call_count_;
        if (task) {
            task();
            ++completed_tasks_;
        }
    }

    void submit_batch_impl(std::vector<std::function<void()>> tasks) override {
        ++batch_impl_call_count_;
        run_tasks(std::move(tasks));
    }

    bool try_submit_batch_impl(std::vector<std::function<void()>> tasks) override {
        ++try_batch_call_count_;
        last_batch_size_ = tasks.size();
        run_tasks(std::move(tasks));
        return true;
    }

private:
    void run_tasks(std::vector<std::function<void()>> tasks) {
        for (auto& task : tasks) {
            if (task) {
                task();
                ++completed_tasks_;
            }
        }
    }

    bool running_ = true;
    int submit_call_count_ = 0;
    int try_batch_call_count_ = 0;
    int batch_impl_call_count_ = 0;
    size_t last_batch_size_ = 0;
    int64_t completed_tasks_ = 0;
};

bool ExecutorFacadeSubmitBatchUsesBatchPath() {
    std::cout << "Testing Executor facade submit_batch uses batch path..."
              << std::endl;

    Executor executor;
    auto mock = std::make_unique<BatchObservingAsyncExecutor>();
    auto* mock_ptr = mock.get();
    executor.manager_->default_async_executor_ = std::move(mock);

    constexpr int num_tasks = 1000;
    std::atomic<int> batch_completed{0};
    std::vector<std::function<void()>> tasks;
    tasks.reserve(num_tasks);
    for (int i = 0; i < num_tasks; ++i) {
        tasks.emplace_back([&batch_completed]() {
            batch_completed.fetch_add(1, std::memory_order_relaxed);
        });
    }

    auto batch_start = std::chrono::steady_clock::now();
    auto futures = executor.submit_batch(tasks);
    auto batch_end = std::chrono::steady_clock::now();

    TEST_ASSERT(futures.size() == tasks.size(),
                "submit_batch should return one future per task");
    for (auto& future : futures) {
        TEST_ASSERT(future.wait_for(std::chrono::seconds(0)) ==
                        std::future_status::ready,
                    "all batch futures should be ready");
        future.get();
    }

    TEST_ASSERT(batch_completed.load() == num_tasks,
                "all batch tasks should execute");
    TEST_ASSERT(mock_ptr->try_batch_call_count() == 1,
                "facade submit_batch should call try_submit_batch_impl once");
    TEST_ASSERT(mock_ptr->last_batch_size() == tasks.size(),
                "batch path should receive the whole task vector");
    TEST_ASSERT(mock_ptr->submit_call_count() == 0,
                "facade submit_batch should not loop through submit");
    TEST_ASSERT(mock_ptr->batch_impl_call_count() == 0,
                "mock try_submit_batch_impl should be the observed batch path");

    std::atomic<int> loop_completed{0};
    std::vector<std::future<void>> loop_futures;
    loop_futures.reserve(num_tasks);

    auto loop_start = std::chrono::steady_clock::now();
    for (int i = 0; i < num_tasks; ++i) {
        loop_futures.push_back(executor.submit([&loop_completed]() {
            loop_completed.fetch_add(1, std::memory_order_relaxed);
        }));
    }
    auto loop_end = std::chrono::steady_clock::now();

    for (auto& future : loop_futures) {
        future.get();
    }
    TEST_ASSERT(loop_completed.load() == num_tasks,
                "all loop-submitted tasks should execute");
    TEST_ASSERT(mock_ptr->submit_call_count() == num_tasks,
                "loop submit comparison should use single-submit path");

    auto batch_us = std::chrono::duration_cast<std::chrono::microseconds>(
                        batch_end - batch_start)
                        .count();
    auto loop_us = std::chrono::duration_cast<std::chrono::microseconds>(
                       loop_end - loop_start)
                       .count();

    std::cout << "  timing: submit_batch=" << batch_us
              << " us, loop submit=" << loop_us << " us" << std::endl;
    std::cout << "  Executor facade submit_batch batch path: PASSED"
              << std::endl;
    return true;
}

int main() {
    bool all_passed = true;
    all_passed &= ExecutorFacadeSubmitBatchUsesBatchPath();

    if (all_passed) {
        std::cout << "All executor facade submit_batch tests passed."
                  << std::endl;
        return 0;
    }
    std::cerr << "Some executor facade submit_batch tests failed." << std::endl;
    return 1;
}
