// 13_cancellation_and_timers.cpp
// 教程 13：协作取消与定时句柄。
//
// 演示：
//   1. submit_cancellable：executor 注入 StopToken，任务轮询协作退出；
//   2. request_task_cancel：排队取消（任务不开跑）与运行中协作请求；
//   3. TaskCancelled 异常与 CancellationStatus 独立计数（不是 failure）；
//   4. submit_delayed_with_handle / TimerHandle：cancel 与 reschedule；
//   5. ScopedTimerHandle：RAII 析构即取消。
//
// 语义边界：
//   - 取消是"请求"不是"中断"：阻塞在无 wakeup 机制调用上的任务不会被强制打断；
//   - deadline 仍是路由/诊断提示，不会自动触发取消；
//   - TimerHandle 不绑定外部事件循环（asio strand 等），需要同一 strand
//     执行与销毁的定时工作继续由应用侧管理。

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include <kairo/executor.hpp>

using namespace kairo;
using namespace std::chrono_literals;

namespace {

void demonstrate_queued_cancel(Executor& executor) {
    std::cout << "\n[1] queued cancel: task never starts\n";

    // 占住唯一 worker，保证下一个任务停留在队列中。
    auto gate = std::make_shared<std::promise<void>>();
    auto blocker = executor.submit_with_handle([gate]() noexcept {
        gate->get_future().wait();
        return 0;
    });

    std::atomic<bool> ran{false};
    auto submission = executor.submit_with_handle([&ran]() noexcept {
        ran.store(true, std::memory_order_release);
        return 42;
    });

    const auto response = executor.request_task_cancel(submission.handle);
    std::cout << "  request_task_cancel -> "
              << to_string(response.result)
              << " (accepted=" << response.accepted() << ")\n";

    gate->set_value();  // 释放 blocker，让队列流动。
    (void)blocker.future.wait_for(5s);

    try {
        (void)submission.future.get();
        std::cout << "  unexpectedly ran\n";
    } catch (const TaskCancelled& cancelled) {
        std::cout << "  future -> TaskCancelled(reason="
                  << to_string(cancelled.reason()) << ")\n";
    }
    std::cout << "  task ran: " << std::boolalpha << ran.load() << "\n";
}

void demonstrate_running_cooperative_cancel(Executor& executor) {
    std::cout << "\n[2] running cancel: cooperative StopToken\n";

    std::atomic<bool> started{false};
    auto submission = executor.submit_cancellable([&started](StopToken token) noexcept {
        started.store(true, std::memory_order_release);
        int progress = 0;
        while (!token.stop_requested() && progress < 100) {
            std::this_thread::sleep_for(1ms);
            ++progress;
        }
        return progress;  // 收到停止请求后正常返回，保留业务结果。
    });

    while (!started.load()) {
        std::this_thread::yield();
    }

    const auto response = executor.request_task_cancel(submission.handle);
    std::cout << "  request_task_cancel -> " << to_string(response.result) << "\n";

    const int progress = submission.future.get();
    std::cout << "  task stopped early at progress=" << progress << " (max 100)\n";
}

void demonstrate_cancellation_status(Executor& executor) {
    std::cout << "\n[3] cancellation counters are lifecycle, not failures\n";

    const CancellationStatus status = executor.get_cancellation_status();
    std::cout << "  request_count=" << status.request_count
              << " queued_cancelled=" << status.queued_cancelled_count
              << " running_request=" << status.running_request_count
              << " completed_after_request=" << status.completed_after_request_count
              << "\n";

    const ExecutorFailureStatus failures = executor.get_failure_status();
    std::cout << "  failure total_count=" << failures.total_count
              << " (cancellations are not counted here)\n";
}

void demonstrate_timer_handle(Executor& executor) {
    std::cout << "\n[4] TimerHandle: reschedule + cancel before expiry\n";

    std::atomic<int> fired{0};
    auto submission = executor.submit_delayed_with_handle(
        500, [&fired]() noexcept { fired.fetch_add(1, std::memory_order_relaxed); });

    std::cout << "  reschedule_after(30) -> "
              << to_string(submission.handle.reschedule_after(30)) << "\n";
    (void)submission.future.wait_for(10s);

    auto status = submission.handle.status();
    std::cout << "  after fire: state="
              << (status ? to_string(status->state) : "unknown")
              << " execution_count=" << (status ? status->execution_count : 0u)
              << "\n";

    auto doomed = executor.submit_delayed_with_handle(60'000, []() noexcept { return 1; });
    std::cout << "  cancel pending timer -> "
              << to_string(doomed.handle.cancel()) << "\n";
    try {
        (void)doomed.future.get();
    } catch (const TaskCancelled& cancelled) {
        std::cout << "  pending future -> TaskCancelled(reason="
                  << to_string(cancelled.reason()) << ")\n";
    }
}

void demonstrate_scoped_timer(Executor& executor) {
    std::cout << "\n[5] ScopedTimerHandle: destructor cancels\n";

    std::atomic<bool> ran{false};
    std::future<int> future;
    {
        auto submission = executor.submit_delayed_with_handle(60'000,
            [&ran]() noexcept {
                ran.store(true, std::memory_order_release);
                return 1;
            });
        future = std::move(submission.future);
        ScopedTimerHandle scoped(std::move(submission.handle));
        std::cout << "  scoped timer alive, leaving scope...\n";
    }  // ScopedTimerHandle 析构请求一次非阻塞取消。
    (void)future.wait_for(5s);
    std::cout << "  after scope: ran=" << std::boolalpha << ran.load() << "\n";
}

}  // namespace

int main() {
    Executor executor;

    ExecutorConfig config;
    config.min_threads = 2;
    config.max_threads = 4;
    if (!executor.initialize(config)) {
        std::cerr << "failed to initialize executor\n";
        return 1;
    }

    demonstrate_queued_cancel(executor);
    demonstrate_running_cooperative_cancel(executor);
    demonstrate_cancellation_status(executor);
    demonstrate_timer_handle(executor);
    demonstrate_scoped_timer(executor);

    std::cout << "\nshutdown: pending timers resolve with TaskCancelled(Shutdown)\n";
    executor.shutdown();
    std::cout << "tutorial 13 completed\n";
    return 0;
}
