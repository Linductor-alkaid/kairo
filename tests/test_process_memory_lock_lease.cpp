// P-004: 进程级 mlockall/munlockall 的可释放全局租约。
// 语义要点：
//  - 引用计数：第一个租约获取时 mlockall，最后一个释放时 munlockall；
//  - 获取失败不产生租约、不消耗引用计数；
//  - RealtimeThreadExecutor 与 BlockingIoExecutor 各自持有租约：停止第一个
//    不解锁，全部停止后恰好解锁一次。
#include "kairo/util/thread_utils.hpp"
#include "kairo/config.hpp"
#include "kairo/realtime_thread_executor.hpp"
#include "kairo/blocking_io_executor.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>

#include <gtest/gtest.h>

namespace {

using kairo::util::ProcessMemoryLockLease;
using kairo::util::ProcessMemoryLockResult;
using kairo::util::ProcessMemoryLockSyscalls;

class CountingSyscalls : public ProcessMemoryLockSyscalls {
public:
    ProcessMemoryLockResult mlockall_current_future() override {
        ++mlock_calls_;
        if (!mlock_succeeds_) {
            return {false, fake_error_code_};
        }
        return {true, 0};
    }

    int munlockall() override {
        ++munlock_calls_;
        return 0;
    }

    int mlock_calls() const { return mlock_calls_.load(); }
    int munlock_calls() const { return munlock_calls_.load(); }
    void set_mlock_succeeds(bool ok) { mlock_succeeds_ = ok; }
    void set_fake_error_code(int code) { fake_error_code_ = code; }

private:
    std::atomic<int> mlock_calls_{0};
    std::atomic<int> munlock_calls_{0};
    bool mlock_succeeds_ = true;
    int fake_error_code_ = 12;  // ENOMEM
};

// RAII：断言失败提前返回时也恢复真实系统调用，且要求测试结束时没有存活租约。
class SyscallRestoreGuard {
public:
    explicit SyscallRestoreGuard(ProcessMemoryLockSyscalls* syscalls) {
        ProcessMemoryLockLease::set_syscalls_for_test(syscalls);
    }
    ~SyscallRestoreGuard() {
        ProcessMemoryLockLease::set_syscalls_for_test(nullptr);
    }
};

TEST(ProcessMemoryLockLease, ReferenceCountedRelease) {
    CountingSyscalls syscalls;
    SyscallRestoreGuard guard(&syscalls);

    auto first = ProcessMemoryLockLease::try_acquire();
    ASSERT_TRUE(first.holds_lock());
    ASSERT_EQ(first.error_code(), 0);
    auto second = ProcessMemoryLockLease::try_acquire();
    ASSERT_TRUE(second.holds_lock());
    // 第二个租约只追加引用，不重复锁定进程。
    EXPECT_EQ(syscalls.mlock_calls(), 1);

    first = ProcessMemoryLockLease{};  // 移动赋空租约 = 释放
    EXPECT_FALSE(first.holds_lock());
    // 仍有持有者：不得解锁。
    EXPECT_EQ(syscalls.munlock_calls(), 0);

    second = ProcessMemoryLockLease{};
    // 最后一个租约释放时恰好解锁一次。
    EXPECT_EQ(syscalls.munlock_calls(), 1);
    EXPECT_EQ(syscalls.mlock_calls(), 1);
}

TEST(ProcessMemoryLockLease, FailedMlockCreatesNoLease) {
    CountingSyscalls syscalls;
    SyscallRestoreGuard guard(&syscalls);
    syscalls.set_mlock_succeeds(false);
    syscalls.set_fake_error_code(12);

    auto lease = ProcessMemoryLockLease::try_acquire();
    EXPECT_FALSE(lease.holds_lock());
    EXPECT_EQ(lease.error_code(), 12);

    lease = ProcessMemoryLockLease{};
    // 失败的获取不产生租约：析构不得触发 munlockall。
    EXPECT_EQ(syscalls.munlock_calls(), 0);

    // 失败也不消耗引用计数：下一次成功获取要重新 mlockall。
    syscalls.set_mlock_succeeds(true);
    auto retry = ProcessMemoryLockLease::try_acquire();
    EXPECT_TRUE(retry.holds_lock());
    EXPECT_EQ(syscalls.mlock_calls(), 2);
    retry = ProcessMemoryLockLease{};
    EXPECT_EQ(syscalls.munlock_calls(), 1);
}

// 阻塞 I/O 测试 worker：等到 stop 请求后返回，期间不占 CPU。
class WaitingWorker final : public kairo::IBlockingIoWorker {
public:
    void run(kairo::StopToken stop_token) override {
        while (!stop_token.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    void wakeup() noexcept override {}
};

// 等待条件成立（带超时），用于启动调优在 worker 线程上异步可见。
bool wait_for(const std::function<bool()>& predicate) {
    for (int i = 0; i < 3000; ++i) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

TEST(ProcessMemoryLockLease, ExecutorsHoldLeasesUntilLastStop) {
    CountingSyscalls syscalls;
    SyscallRestoreGuard guard(&syscalls);

    kairo::RealtimeThreadConfig rt_config;
    rt_config.enable_process_memory_lock = true;
    rt_config.thread_name = "p004_lease";
    rt_config.cycle_period_ns = 20'000'000;  // 20ms，避免忙转
    rt_config.timer_slack_ns = 0;            // 保留内核默认，减少 CI 环境抖动
    rt_config.cycle_callback = [] {};
    kairo::RealtimeThreadExecutor rt_executor("p004_rt", rt_config);
    ASSERT_TRUE(rt_executor.start());

    kairo::BlockingIoConfig io_config;
    io_config.enable_memory_lock = true;
    io_config.thread_name = "p004_io";
    kairo::BlockingIoExecutor io_executor(
        "p004_io", io_config, std::make_unique<WaitingWorker>());
    ASSERT_TRUE(io_executor.start());

    // 两个执行器的 worker 线程各自取得租约（共享同一次 mlockall）。
    ASSERT_TRUE(wait_for([&] {
        return rt_executor.get_status().process_memory_lock_applied &&
               io_executor.get_status().memory_locked;
    })) << "both executors should acquire the memory-lock lease";
    EXPECT_EQ(syscalls.mlock_calls(), 1);
    EXPECT_EQ(syscalls.munlock_calls(), 0);

    // 停止第一个执行器（worker join 后租约已释放），但不得解锁。
    rt_executor.stop_and_join();
    EXPECT_EQ(syscalls.munlock_calls(), 0);

    // 停止最后一个执行器：恰好解锁一次，且全程只锁定过一次。
    io_executor.stop();
    EXPECT_EQ(syscalls.mlock_calls(), 1);
    EXPECT_EQ(syscalls.munlock_calls(), 1);
}

}  // namespace
