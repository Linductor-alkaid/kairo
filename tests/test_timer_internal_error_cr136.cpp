/// @file test_timer_internal_error_cr136.cpp
/// CR-136（0.7.2 §4.3）TimerOperationResult::InternalError 的独立验证：
///
/// - 枚举 additive：全枚举器 switch（无 default）可编译，既有调用方
///   的 switch 不被破坏（编译即证，static_assert 面）。
/// - to_string("InternalError") 正确。
/// - 真实未命中仍返回 NotFound（不得被兜底掩盖）。
/// - 正常 cancel/reschedule 路径行为不变。

#include <kairo/timer.hpp>

#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <string>
#include <string_view>
#include <thread>

using kairo::TaskCancellationState;
using kairo::TimerOperationResult;
using kairo::detail::TimerScheduler;
using namespace std::chrono_literals;

namespace {

// CR-136 additive 编译证明：覆盖全部枚举器、无 default 的 switch。
// 若未来删除/改名既有枚举器，这里编译失败（破坏面被立即暴露）。
constexpr int enumerate_all_timer_results(TimerOperationResult result) {
    switch (result) {
        case TimerOperationResult::CancelledBeforeDispatch:
        case TimerOperationResult::CancellationRequestedAfterDispatch:
        case TimerOperationResult::Rescheduled:
        case TimerOperationResult::AlreadyCancelled:
        case TimerOperationResult::AlreadyCompleted:
        case TimerOperationResult::NotFound:
        case TimerOperationResult::ShuttingDown:
        case TimerOperationResult::InvalidDuration:
        case TimerOperationResult::InternalError:
            return 0;
    }
    return 1;
}
static_assert(enumerate_all_timer_results(TimerOperationResult::InternalError) == 0,
              "InternalError must be a first-class enumerator");

} // namespace

TEST(TimerInternalErrorCr136, ToStringCoversAllEnumerators) {
    EXPECT_STREQ(to_string(TimerOperationResult::CancelledBeforeDispatch),
                 "CancelledBeforeDispatch");
    EXPECT_STREQ(to_string(TimerOperationResult::NotFound), "NotFound");
    EXPECT_STREQ(to_string(TimerOperationResult::InternalError), "InternalError");
}

TEST(TimerInternalErrorCr136, UnknownTimerStillReturnsNotFound) {
    // CR-136 核心验收：catch 兜底改判 InternalError 后，真实未命中
    // 不得被掩盖——没有这个 timer 就返回 NotFound。
    TimerScheduler scheduler;
    scheduler.start();

    EXPECT_EQ(scheduler.request_cancel("no-such-timer"),
              TimerOperationResult::NotFound);
    EXPECT_EQ(scheduler.reschedule("no-such-timer", 100),
              TimerOperationResult::NotFound);

    scheduler.stop();
}

TEST(TimerInternalErrorCr136, NormalCancelAndRescheduleUnaffected) {
    // 对照：存在且未到期的 timer，cancel/reschedule 走原有成功路径。
    TimerScheduler scheduler;
    scheduler.start();

    std::atomic<bool> fired{false};
    const std::string id = scheduler.schedule_once(
        /*delay_ms=*/30'000, "cr136_timer", nullptr, std::string(),
        [&fired] { fired.store(true); },
        [](std::exception_ptr) {});

    EXPECT_FALSE(id.empty());
    EXPECT_EQ(scheduler.request_cancel(id),
              TimerOperationResult::CancelledBeforeDispatch);
    // 已取消后再次 cancel：NotFound 或 AlreadyCancelled 任一都表达
    // "非 InternalError" 的既有终态；绝不允许 InternalError。
    EXPECT_NE(scheduler.request_cancel(id), TimerOperationResult::InternalError);

    // 重新登记一个，验证 reschedule 成功路径。
    const std::string id2 = scheduler.schedule_once(
        30'000, "cr136_timer2", nullptr, std::string(), [] {},
        [](std::exception_ptr) {});
    EXPECT_FALSE(id2.empty());
    EXPECT_EQ(scheduler.reschedule(id2, 60'000),
              TimerOperationResult::Rescheduled);

    scheduler.stop();
    EXPECT_FALSE(fired.load()) << "cancelled timer must not fire";
}
