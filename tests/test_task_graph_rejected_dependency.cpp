#include <kairo/executor.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
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

// 拷贝构造抛异常的仿函数：任务本体合法，但 submit_with_handle 把 f 拷进
// 提交 lambda 的捕获时会抛异常——确定性的"提交未送达"，无需竞态窗口。
struct ThrowingCopyTask {
    ThrowingCopyTask() = default;
    ThrowingCopyTask(const ThrowingCopyTask&) {
        throw std::runtime_error("injected submission copy failure");
    }
    ThrowingCopyTask& operator=(const ThrowingCopyTask&) {
        throw std::runtime_error("injected submission copy failure");
    }
    void operator()() const {
        // 不会被执行
    }
};

}  // namespace

// 回归：提交被拒/未送达时任务图节点停留 Pending，依赖该句柄的
// submit_after / when_all 在 worker 线程上无限等待，可耗尽线程池并挂死
// shutdown。修复后所有拒绝路径都会把句柄置为 Failed 并唤醒依赖方。
TEST(RejectedTaskGraphTest, EmptyTaskRejectionFailsDependentsInsteadOfBlocking) {
    kairo::Executor executor;
    ASSERT_TRUE(executor.initialize(config()));

    // 注意：经 submit_with_handle 包装后，空 std::function 不会被 submit 层
    // 的 empty-task 检查拦截（那里看到的是包装 lambda），而是真正入队、
    // 调用时抛 bad_function_call —— 任务图节点经由执行异常路径落 Failed。
    auto rejected = executor.submit_with_handle(std::function<void()>{});
    ASSERT_EQ(rejected.future.wait_for(5s), std::future_status::ready);
    EXPECT_THROW(rejected.future.get(), std::bad_function_call);

    // 依赖失败句柄的任务必须快速失败（依赖异常原样透传），而不是把
    // worker 挂在一个永不到来的终态上。
    auto dependent = executor.submit_after(rejected.handle, [] { return 42; });
    ASSERT_EQ(dependent.wait_for(10s), std::future_status::ready);
    EXPECT_THROW(dependent.get(), std::bad_function_call);

    auto combined = executor.when_all({rejected.handle});
    auto chained = executor.submit_after(combined, [] { return 7; });
    ASSERT_EQ(chained.wait_for(10s), std::future_status::ready);

    // 池仍然健康：普通提交照常完成。
    auto ok = executor.submit([] { return 7; });
    EXPECT_EQ(ok.get(), 7);

    executor.shutdown();
}

TEST(RejectedTaskGraphTest, ThrowingCopySubmissionKeepsGraphTerminalAndPoolHealthy) {
    kairo::Executor executor;
    ASSERT_TRUE(executor.initialize(config()));

    kairo::TaskHandle rejected_handle;
    ASSERT_ANY_THROW({
        auto submission = executor.submit_with_handle(ThrowingCopyTask{});
        rejected_handle = submission.handle;
    });

    // 调用方即使拿到异常路径中的句柄副本，依赖它也必须快速失败，
    // 而不是把 worker 挂在一个 Pending 节点上。
    if (rejected_handle.valid()) {
        auto dependent = executor.submit_after(rejected_handle, [] { return 42; });
        ASSERT_EQ(dependent.wait_for(10s), std::future_status::ready);
        EXPECT_THROW(dependent.get(), std::runtime_error);
    }

    auto ok = executor.submit([] { return 11; });
    EXPECT_EQ(ok.get(), 11);

    executor.shutdown();
}
