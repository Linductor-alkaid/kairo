#include <kairo/executor.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <future>
#include <stdexcept>
#include <string>
#include <vector>

using namespace std::chrono_literals;

namespace {

kairo::ExecutorConfig config() {
    kairo::ExecutorConfig cfg;
    cfg.min_threads = 2;
    cfg.max_threads = 2;
    cfg.queue_capacity = 64;
    return cfg;
}

TEST(ExecutorTaskGraphTest, SubmitAfterRunsAfterDependency) {
    kairo::Executor executor;
    ASSERT_TRUE(executor.initialize(config()));

    std::atomic<int> order{0};
    auto root = executor.submit_with_handle([&] {
        EXPECT_EQ(order.fetch_add(1, std::memory_order_acq_rel), 0);
        return 21;
    });

    auto dependent = executor.submit_after(root.handle, [&] {
        EXPECT_EQ(order.fetch_add(1, std::memory_order_acq_rel), 1);
        return 42;
    });

    EXPECT_EQ(root.future.get(), 21);
    EXPECT_EQ(dependent.get(), 42);
    EXPECT_EQ(order.load(std::memory_order_acquire), 2);

    executor.shutdown();
}

TEST(ExecutorTaskGraphTest, SnapshotIdentifiesDependencyBlockedHandle) {
    kairo::Executor executor;
    ASSERT_TRUE(executor.initialize(config()));
    executor.set_in_flight_task_capacity(16);

    std::promise<void> root_started;
    std::promise<void> release_root;
    const auto release = release_root.get_future().share();
    auto root = executor.submit_with_handle([&] {
        root_started.set_value();
        release.wait();
        return 1;
    });
    root_started.get_future().wait();

    auto dependent = executor.submit_after_with_handle(root.handle, [] { return 2; });
    const auto snapshot = executor.get_snapshot();
    const auto blocked = std::find_if(
        snapshot.in_flight_tasks.begin(), snapshot.in_flight_tasks.end(),
        [&](const kairo::TaskLifecycleSnapshot& task) {
            return task.task_id == dependent.handle.id();
        });
    ASSERT_NE(blocked, snapshot.in_flight_tasks.end());
    EXPECT_EQ(blocked->task_type, "task_graph");
    EXPECT_EQ(blocked->state, kairo::TaskLifecycleState::DependencyBlocked);

    release_root.set_value();
    EXPECT_EQ(root.future.get(), 1);
    EXPECT_EQ(dependent.future.get(), 2);
    const auto completed = executor.get_snapshot();
    EXPECT_EQ(std::count_if(completed.in_flight_tasks.begin(), completed.in_flight_tasks.end(),
                            [&](const kairo::TaskLifecycleSnapshot& task) {
                                return task.task_id == root.handle.id() ||
                                       task.task_id == dependent.handle.id();
                            }),
              0);
    executor.shutdown();
}

TEST(ExecutorTaskGraphTest, WhenAllWaitsForAllDependencies) {
    kairo::Executor executor;
    ASSERT_TRUE(executor.initialize(config()));

    std::atomic<int> completed{0};
    auto first = executor.submit_with_handle([&] {
        completed.fetch_add(1, std::memory_order_acq_rel);
        return 1;
    });
    auto second = executor.submit_with_handle([&] {
        completed.fetch_add(1, std::memory_order_acq_rel);
        return 2;
    });

    auto all = executor.when_all({first.handle, second.handle});
    auto after_all = executor.submit_after(all, [&] {
        EXPECT_EQ(completed.load(std::memory_order_acquire), 2);
        return 3;
    });

    EXPECT_EQ(first.future.get(), 1);
    EXPECT_EQ(second.future.get(), 2);
    EXPECT_EQ(after_all.get(), 3);

    executor.shutdown();
}

TEST(ExecutorTaskGraphTest, NestedWhenAllPropagatesCompletion) {
    kairo::Executor executor;
    ASSERT_TRUE(executor.initialize(config()));

    auto first = executor.submit_with_handle([] {
        return 1;
    });
    auto second = executor.submit_with_handle([] {
        return 2;
    });

    auto inner = executor.when_all({first.handle, second.handle});
    auto outer = executor.when_all({inner});

    auto after_outer = executor.submit_after(outer, [] {
        return 3;
    });

    EXPECT_EQ(first.future.get(), 1);
    EXPECT_EQ(second.future.get(), 2);
    EXPECT_EQ(after_outer.get(), 3);

    executor.shutdown();
}

TEST(ExecutorTaskGraphTest, DependencyFailureSkipsDependentTask) {
    kairo::Executor executor;
    ASSERT_TRUE(executor.initialize(config()));

    auto failing = executor.submit_with_handle([]() -> int {
        throw std::runtime_error("root failed");
    });

    std::atomic<bool> ran{false};
    auto dependent = executor.submit_after(failing.handle, [&] {
        ran.store(true, std::memory_order_release);
        return 7;
    });

    EXPECT_THROW(failing.future.get(), std::runtime_error);
    EXPECT_THROW(dependent.get(), std::runtime_error);
    EXPECT_FALSE(ran.load(std::memory_order_acquire));
    EXPECT_GE(executor.get_failure_status().task_exception_count, 1U);

    executor.shutdown();
}

TEST(ExecutorTaskGraphTest, WhenAllFailureSkipsDependentTask) {
    kairo::Executor executor;
    ASSERT_TRUE(executor.initialize(config()));

    auto successful = executor.submit_with_handle([] { return 1; });
    auto failing = executor.submit_with_handle([]() -> int {
        throw std::runtime_error("precondition failed");
    });
    const auto all = executor.when_all({successful.handle, failing.handle});

    std::atomic<bool> ran{false};
    auto dependent = executor.submit_after(all, [&] {
        ran.store(true, std::memory_order_release);
        return 3;
    });

    EXPECT_EQ(successful.future.get(), 1);
    EXPECT_THROW(failing.future.get(), std::runtime_error);
    EXPECT_THROW(dependent.get(), std::runtime_error);
    EXPECT_FALSE(ran.load(std::memory_order_acquire));

    executor.shutdown();
}

TEST(ExecutorTaskGraphTest, RejectsHandleFromAnotherExecutor) {
    kairo::Executor first_executor;
    kairo::Executor second_executor;
    ASSERT_TRUE(first_executor.initialize(config()));
    ASSERT_TRUE(second_executor.initialize(config()));

    auto foreign = first_executor.submit_with_handle([] { return 1; });
    std::atomic<bool> ran{false};
    auto dependent = second_executor.submit_after(foreign.handle, [&] {
        ran.store(true, std::memory_order_release);
    });

    EXPECT_EQ(foreign.future.get(), 1);
    EXPECT_THROW(dependent.get(), std::runtime_error);
    EXPECT_FALSE(ran.load(std::memory_order_acquire));
    EXPECT_GE(second_executor.get_failure_status().submit_rejected_count, 1U);

    first_executor.shutdown();
    second_executor.shutdown();
}

TEST(ExecutorTaskGraphTest, CompletedHandleCanStillCreateDependentTask) {
    kairo::Executor executor;
    ASSERT_TRUE(executor.initialize(config()));

    auto completed = executor.submit_with_handle([] { return 7; });
    EXPECT_EQ(completed.future.get(), 7);

    auto dependent = executor.submit_after(completed.handle, [] { return 8; });
    EXPECT_EQ(dependent.get(), 8);

    executor.shutdown();
}

TEST(ExecutorTaskGraphTest, TerminalHandleRetentionIsBounded) {
    kairo::Executor executor;
    auto cfg = config();
    cfg.task_graph_retention_capacity = 1;
    ASSERT_TRUE(executor.initialize(cfg));

    auto first = executor.submit_with_handle([] { return 1; });
    EXPECT_EQ(first.future.get(), 1);

    auto second = executor.submit_with_handle([] { return 2; });
    EXPECT_EQ(second.future.get(), 2);

    auto retained_dependent = executor.submit_after(second.handle, [] { return 4; });
    EXPECT_EQ(retained_dependent.get(), 4);

    auto expired_dependent = executor.submit_after(first.handle, [] { return 3; });
    EXPECT_THROW(expired_dependent.get(), std::runtime_error);
    EXPECT_EQ(executor.task_graph_retention_capacity(), 1U);

    executor.shutdown();
}

TEST(ExecutorTaskGraphTest, ZeroRetentionExpiresTerminalHandlesImmediately) {
    kairo::Executor executor;
    auto cfg = config();
    cfg.task_graph_retention_capacity = 0;
    ASSERT_TRUE(executor.initialize(cfg));

    auto root = executor.submit_with_handle([] { return 1; });
    EXPECT_EQ(root.future.get(), 1);

    auto dependent = executor.submit_after(root.handle, [] { return 2; });
    EXPECT_THROW(dependent.get(), std::runtime_error);

    executor.shutdown();
}

TEST(ExecutorTaskGraphTest, ActiveDependentPreventsEarlyHandleExpiration) {
    kairo::Executor executor;
    auto cfg = config();
    cfg.task_graph_retention_capacity = 0;
    ASSERT_TRUE(executor.initialize(cfg));

    std::promise<void> root_started;
    std::promise<void> release_root;
    const auto release = release_root.get_future().share();
    auto root = executor.submit_with_handle([&] {
        root_started.set_value();
        release.wait();
        return 1;
    });
    root_started.get_future().wait();

    auto dependent = executor.submit_after(root.handle, [] { return 2; });
    release_root.set_value();

    EXPECT_EQ(root.future.get(), 1);
    EXPECT_EQ(dependent.get(), 2);

    executor.shutdown();
}

TEST(ExecutorTaskGraphTest, InvalidHandleReturnsReadyExceptionalFuture) {
    kairo::Executor executor;
    ASSERT_TRUE(executor.initialize(config()));

    std::atomic<bool> ran{false};
    auto future = executor.submit_after(kairo::TaskHandle{}, [&] {
        ran.store(true, std::memory_order_release);
    });

    EXPECT_THROW(future.get(), std::runtime_error);
    EXPECT_FALSE(ran.load(std::memory_order_acquire));
    EXPECT_GE(executor.get_failure_status().submit_rejected_count, 1U);

    executor.shutdown();
}

TEST(ExecutorTaskGraphTest, ShutdownMakesPendingDependencyObservable) {
    kairo::Executor executor;
    ASSERT_TRUE(executor.initialize(config()));

    kairo::TaskHandle invalid;
    auto future = executor.submit_after(invalid, [] {
        return 1;
    });

    executor.shutdown(false);
    EXPECT_THROW(future.get(), std::runtime_error);
}

} // namespace
