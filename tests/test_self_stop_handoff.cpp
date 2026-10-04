#include <gtest/gtest.h>

#include <kairo/config.hpp>
#include <kairo/lockfree_task_executor.hpp>
#include "kairo/realtime_thread_executor.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <future>
#include <memory>
#include <thread>

namespace {

using namespace std::chrono_literals;

// LockFreeTaskExecutor 自停止路径:任务 lambda 中调用 stop() + stop_and_join(),
// 期望 stop_and_join() 返回 false(已请求停止但未 join),外部线程随后 stop_and_join()
// 返回 true 正常 join。
TEST(SelfStopHandoff, SelfStopDoesNotTerminate_LockFreeTaskExecutor) {
    auto executor = std::make_unique<kairo::LockFreeTaskExecutor>(8);
    std::promise<bool> self_stop_result;
    auto self_stop_future = self_stop_result.get_future();

    ASSERT_TRUE(executor->push_task([&] {
        executor->stop();
        self_stop_result.set_value(executor->stop_and_join());
    }));
    ASSERT_TRUE(executor->start());

    ASSERT_EQ(self_stop_future.wait_for(1s), std::future_status::ready);
    EXPECT_FALSE(self_stop_future.get());
    EXPECT_TRUE(executor->stop_and_join());
    executor.reset();
}

TEST(SelfStopHandoff, SelfStopDoesNotTerminate_RealtimeThreadExecutor) {
    kairo::RealtimeThreadConfig config;
    config.thread_name = "external_stop_rt";
    config.cycle_period_ns = 1'000'000;
    std::atomic<kairo::RealtimeThreadExecutor*> executor_ptr{nullptr};
    std::atomic<bool> callback_invoked{false};
    std::promise<bool> self_stop_result;
    auto self_stop_future = self_stop_result.get_future();
    config.cycle_callback = [&] {
        auto* executor = executor_ptr.load(std::memory_order_acquire);
        if (executor && !callback_invoked.exchange(true, std::memory_order_acq_rel)) {
            executor->stop();
            self_stop_result.set_value(executor->stop_and_join());
        }
    };

    auto executor = std::make_unique<kairo::RealtimeThreadExecutor>(
        "external_stop_rt", config);
    executor_ptr.store(executor.get(), std::memory_order_release);
    ASSERT_TRUE(executor->start());

    ASSERT_EQ(self_stop_future.wait_for(1s), std::future_status::ready);
    EXPECT_FALSE(self_stop_future.get());
    EXPECT_TRUE(executor->stop_and_join());
    executor_ptr.store(nullptr, std::memory_order_release);
    executor.reset();
}

// 并发外部线程 stop_and_join:两个外部线程同时调用 stop_and_join,断言两个都返回
// true 且无死锁。stop_mutex_ 应串行化 join,确保只有一个 thread 真正 join。
TEST(SelfStopHandoff, ConcurrentStopFromTwoExternalThreads) {
    kairo::LockFreeTaskExecutor executor(8);
    ASSERT_TRUE(executor.start());

    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    bool first_result = false;
    bool second_result = false;
    auto stop_from_external_thread = [&](bool& result, const char* who) {
        ready.fetch_add(1, std::memory_order_release);
        while (!go.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        std::fprintf(stderr, "[%s] before stop_and_join\n", who);
        result = executor.stop_and_join();
        std::fprintf(stderr, "[%s] after stop_and_join result=%d\n", who, result);
    };

    std::thread first(stop_from_external_thread, std::ref(first_result), "first");
    std::thread second(stop_from_external_thread, std::ref(second_result), "second");
    while (ready.load(std::memory_order_acquire) != 2) {
        std::this_thread::yield();
    }
    go.store(true, std::memory_order_release);
    first.join();
    second.join();

    EXPECT_TRUE(first_result);
    EXPECT_TRUE(second_result);
}

class CountingLockFreeTaskExecutor : public kairo::LockFreeTaskExecutor {
public:
    using LockFreeTaskExecutor::LockFreeTaskExecutor;

    std::atomic<unsigned> worker_creations{0};

protected:
    std::thread create_worker_thread() override {
        worker_creations.fetch_add(1, std::memory_order_relaxed);
        return LockFreeTaskExecutor::create_worker_thread();
    }
};

TEST(SelfStopHandoff, LockFreeConcurrentStartStopNoLeakedThread) {
    constexpr unsigned kIterations = 200;

    for (unsigned iteration = 0; iteration < kIterations; ++iteration) {
        CountingLockFreeTaskExecutor executor(8);
        std::atomic<unsigned> ready{0};
        std::atomic<bool> go{false};
        bool start_result = false;
        bool stop_result = false;

        std::thread starter([&] {
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            start_result = executor.start();
        });
        std::thread stopper([&] {
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            stop_result = executor.stop_and_join();
        });

        while (ready.load(std::memory_order_acquire) != 2) {
            std::this_thread::yield();
        }
        go.store(true, std::memory_order_release);
        starter.join();
        stopper.join();

        EXPECT_TRUE(stop_result) << "iteration " << iteration;
        EXPECT_EQ(executor.worker_creations.load(std::memory_order_relaxed),
                  start_result ? 1U : 0U)
            << "iteration " << iteration;
        EXPECT_FALSE(executor.start()) << "iteration " << iteration;
    }
}

TEST(RealtimeConcurrentStop, StartRejectedUntilJoinCompletes) {
    kairo::RealtimeThreadConfig config;
    config.thread_name = "concurrent_stop_rt";
    config.cycle_period_ns = 1'000'000;

    std::mutex callback_mutex;
    std::condition_variable callback_cv;
    bool release_callback = false;
    std::atomic<bool> callback_notified{false};
    std::promise<void> callback_entered;
    auto callback_entered_future = callback_entered.get_future();
    config.cycle_callback = [&] {
        if (!callback_notified.exchange(true, std::memory_order_acq_rel)) {
            callback_entered.set_value();
        }
        std::unique_lock<std::mutex> lock(callback_mutex);
        callback_cv.wait(lock, [&] { return release_callback; });
    };

    kairo::RealtimeThreadExecutor executor("concurrent_stop_rt", config);
    ASSERT_TRUE(executor.start());
    ASSERT_EQ(callback_entered_future.wait_for(1s), std::future_status::ready);

    std::promise<bool> first_stop_result;
    auto first_stop_future = first_stop_result.get_future();
    std::thread first_stopper([&] {
        first_stop_result.set_value(executor.stop_and_join());
    });

    const auto stopping_deadline = std::chrono::steady_clock::now() + 1s;
    while (executor.get_status().is_running &&
           std::chrono::steady_clock::now() < stopping_deadline) {
        std::this_thread::yield();
    }
    ASSERT_FALSE(executor.get_status().is_running);

    std::promise<bool> second_stop_result;
    auto second_stop_future = second_stop_result.get_future();
    std::thread second_stopper([&] {
        second_stop_result.set_value(executor.stop_and_join());
    });

    EXPECT_EQ(second_stop_future.wait_for(50ms), std::future_status::timeout);
    EXPECT_FALSE(executor.start());

    {
        std::lock_guard<std::mutex> lock(callback_mutex);
        release_callback = true;
    }
    callback_cv.notify_one();

    EXPECT_EQ(first_stop_future.wait_for(1s), std::future_status::ready);
    EXPECT_EQ(second_stop_future.wait_for(1s), std::future_status::ready);
    EXPECT_TRUE(first_stop_future.get());
    EXPECT_TRUE(second_stop_future.get());
    first_stopper.join();
    second_stopper.join();

    EXPECT_TRUE(executor.start());
    EXPECT_TRUE(executor.stop_and_join());
}

// 自停止后剩余任务被丢弃:任务 A 在内部调 stop(),后续任务 B 不应再执行,
// processed_count 应只统计已执行的任务(A)。
TEST(SelfStopHandoff, DrainOnStopRejectedWhenSelfStop) {
    kairo::LockFreeTaskExecutor executor(8);
    std::promise<void> self_stop_done;
    auto self_stop_future = self_stop_done.get_future();
    std::atomic<int> executed{0};

    ASSERT_TRUE(executor.push_task([&] {
        executed.fetch_add(1, std::memory_order_relaxed);
        executor.stop();
        self_stop_done.set_value();
    }));
    ASSERT_TRUE(executor.push_task([&] {
        executed.fetch_add(1, std::memory_order_relaxed);
    }));
    ASSERT_TRUE(executor.start());

    ASSERT_EQ(self_stop_future.wait_for(1s), std::future_status::ready);
    EXPECT_TRUE(executor.stop_and_join());
    EXPECT_EQ(executed.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(executor.processed_count(), 1u);
}

} // namespace
