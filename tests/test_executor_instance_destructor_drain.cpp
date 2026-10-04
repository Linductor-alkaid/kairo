#include <kairo/executor.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>

using namespace std::chrono_literals;

// 回归：实例模式 ~Executor 原先依赖成员析构链（owned_manager_ 几乎最后
// 销毁）触发池排空，此时 task_graph_mutex_/task_graph_cv_、failure_mutex_ 等
// facade 状态成员已被析构，仍在运行的 wrapper 触碰它们是 use-after-free。
// 修复后析构函数体先显式排空，任何成员销毁开始时池内已无任务。
TEST(ExecutorInstanceDestructorTest, DrainsOwnedPoolBeforeDestroyingState) {
    std::atomic<bool> plain_completed{false};
    {
        kairo::Executor executor;
        kairo::ExecutorConfig cfg;
        cfg.min_threads = 2;
        cfg.max_threads = 2;
        ASSERT_TRUE(executor.initialize(cfg));

        // 普通任务：排空期间 mark_task_graph_* 会触碰任务图锁与 CV。
        executor.submit([&plain_completed] {
            std::this_thread::sleep_for(50ms);
            plain_completed.store(true, std::memory_order_release);
        });
        // 抛异常任务：排空期间 record_task_exception 触碰 failure 锁。
        executor.submit([] { throw std::runtime_error("drain-time failure"); });

        // 立刻离开作用域：析构必须等待任务结束再开始拆成员。
    }
    EXPECT_TRUE(plain_completed.load(std::memory_order_acquire));
}

// 析构路径排空后，生命周期状态应为 Stopped（与显式 shutdown 的可观测
// 行为一致），且重复析构安全（shutdown 幂等）。
TEST(ExecutorInstanceDestructorTest, DestructorAfterExplicitShutdownIsHarmless) {
    std::atomic<bool> ran{false};
    {
        kairo::Executor executor;
        kairo::ExecutorConfig cfg;
        cfg.min_threads = 1;
        cfg.max_threads = 1;
        ASSERT_TRUE(executor.initialize(cfg));
        executor.submit([&ran] { ran.store(true, std::memory_order_release); });
        executor.shutdown();
        EXPECT_TRUE(ran.load(std::memory_order_acquire));
        // 显式 shutdown 后析构：内部二次 shutdown 应是幂等空操作。
    }
    SUCCEED();
}
