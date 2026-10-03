// PR-2（dependency-driven scheduling 后续）独立验证测试。
//
// 被测行为（相对 PR-1 的增量）：
//   D1  parked 超时（提交即起算）：task_timeout_ms > 0 时，parked 依赖图
//       任务由 facade 定时器在预算耗尽后以 TimedOutException 结算，并与
//       取消/池队列计时器经同一 phase CAS 仲裁恰好一个赢家；
//   D2  shutdown 终局结算：shutdown 三条路径在 manager shutdown 返回后对
//       仍 parked 的节点统一失败结算（future 不悬空、admission/registry
//       释放、下游级联）；
//   retention 交互：retention trim 不得驱逐仍被 parked dependent 引用的
//       终态依赖节点；
//   serial dispatch drain bag 化 + TicketGuard 丢发补 mark：submit_on 句柄
//       作为依赖、serial 失败统计先于 dependent future、shutdown 丢弃在途
//       serial 派发时 parked 下游限时结算。
//
// 测试模式：门控握手 + 有限等待（kSettleLimit）。等待"超时预算流逝"的
// 观察窗口是 D1 被测语义本身（时间预算），使用有界 wait_until，不作为
// 通过/失败的 sleep 时序依据。

#include <kairo/executor.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <exception>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

// 单个 future 的有限等待上限；超时即失败（防挂死）。TSAN 下放大。
#if defined(KAIRO_TEST_TSAN)
constexpr auto kSettleLimit = std::chrono::seconds{30};
#else
constexpr auto kSettleLimit = std::chrono::seconds{10};
#endif
// 应当"立即"就绪的路径的收紧上限。
constexpr auto kPromptLimit = std::chrono::seconds{2};

// D1 测试预算：parked 超时在这个量级触发；观察窗口取其 2 倍以上。
constexpr int64_t kTimeoutBudgetMs = 200;
// 等待"预算肯定已流逝"的观察窗口（>= 2x 预算，容忍负载抖动）。
constexpr auto kBudgetElapseWindow = std::chrono::milliseconds{500};

kairo::ExecutorConfig pool_config(std::size_t workers,
                                     std::size_t queue_capacity = 64) {
    kairo::ExecutorConfig config;
    config.min_threads = workers;
    config.max_threads = workers;
    config.queue_capacity = queue_capacity;
    return config;
}

kairo::ExecutorConfig timeout_config(std::size_t workers,
                                        int64_t timeout_ms,
                                        std::size_t max_in_flight = 0) {
    kairo::ExecutorConfig config = pool_config(workers);
    config.task_timeout_ms = timeout_ms;
    config.max_in_flight_tasks = max_in_flight;
    return config;
}

// 门控任务：进入即报数并阻塞在门上，测试线程显式 release() 放行。
// 析构自动放行：保证 ASSERT 失败提前返回时 facade 析构不因 worker 被
// 门占住而挂死（~Executor -> shutdown(true) 会等池排空）。
class Gate {
public:
    Gate() : release_(release_channel_.get_future().share()) {}

    Gate(const Gate&) = delete;
    Gate& operator=(const Gate&) = delete;

    ~Gate() { release(); }

    kairo::TaskSubmission<int> spawn_on(kairo::Executor& executor) {
        auto entered = std::make_shared<std::promise<void>>();
        entered_ = entered->get_future();
        auto release = release_;
        return executor.submit_with_handle([entered, release] {
            entered->set_value();
            release.wait();
            return 0;
        });
    }

    bool entered_within(std::chrono::milliseconds limit = kSettleLimit) {
        return entered_.wait_for(limit) == std::future_status::ready;
    }

    void release() {
        if (released_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        try {
            release_channel_.set_value();
        } catch (const std::future_error&) {
            // 已放行（幂等）。
        }
    }

    // 有界自放行：shutdown(true) 收敛测试用——门在 budget 内自动退出，
    // 让池排空、parked 链经 drain 路径收敛；断言本身只依赖有限等待。
    void release_after(std::chrono::milliseconds budget) {
        auto channel = release_;
        channel.wait_for(budget);
        release();
    }

private:
    std::promise<void> release_channel_;
    std::shared_future<void> release_;
    std::future<void> entered_;
    std::atomic<bool> released_{false};
};

// 有限等待 future<int> 就绪；不就绪返回 false（配合断言防挂死）。
bool settles_within(std::future<int>& future,
                    std::chrono::milliseconds limit = kSettleLimit) {
    return future.valid() &&
           future.wait_for(limit) == std::future_status::ready;
}

bool settles_within(std::future<void>& future,
                    std::chrono::milliseconds limit = kSettleLimit) {
    return future.valid() &&
           future.wait_for(limit) == std::future_status::ready;
}

// 有界等待 timeout 统计达到期望值。
//
// 顺序边界：on_timeout 路径（executor.hpp）中 promise->set_exception（唤醒
// own future 的等待者）先于 record_task_timeout 写入；库契约只保证"统计
// 先于级联结算"（D1-a 经 grandchild 验证），不保证先于本节点 own future。
// 因此在"无下游 dependent"的用例里，own future 结算后读取计数必须用有界
// 等待，而非直接断言（后者会以 ~1/600 概率读到尚未写入的 0）。
bool timeout_count_reaches(kairo::Executor& executor, uint64_t expected) {
    const auto deadline = std::chrono::steady_clock::now() + kSettleLimit;
    for (;;) {
        if (executor.get_failure_status().timeout_count >= expected) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
}

// 从快照中读取任务的 lifecycle 状态；任务不在快照中返回 false。
bool lifecycle_state_of(kairo::Executor& executor,
                        const kairo::TaskHandle& handle,
                        kairo::TaskLifecycleState& state) {
    const auto snapshot = executor.get_snapshot();
    for (const auto& task : snapshot.in_flight_tasks) {
        if (task.task_id == handle.id()) {
            state = task.state;
            return true;
        }
    }
    return false;
}

// 异常检视：捕获后按引用转交判定闭包，返回闭包结果。
template <typename Judge>
bool inspect_exception(std::future<int>& future, Judge&& judge) {
    try {
        (void)future.get();
    } catch (...) {
        return judge(std::current_exception());
    }
    return false;
}

std::string exception_message(const std::exception_ptr& exception) {
    try {
        if (exception) {
            std::rethrow_exception(exception);
        }
    } catch (const std::exception& error) {
        return error.what();
    } catch (...) {
        return "<non-std exception>";
    }
    return "<no exception>";
}

// 等待到指定时限（观察"预算已流逝"窗口的辅助；有界，非通过性时序）。
void wait_until_after(std::chrono::steady_clock::time_point deadline) {
    std::this_thread::sleep_until(deadline);
}

// -------------------------------------------------------------------------
// D1：parked 超时（提交即起算）
// -------------------------------------------------------------------------

// D1-a：parked 超预算 → dependent future 限时就绪、TimedOutException 语义、
// callable 未运行、下游 parked 依赖级联失败、timeout 统计可见。
//
// 全新 facade（从未提交过 delayed/periodic 任务）：这是 D1 的主路径——
// 用户只配置 task_timeout_ms 并使用依赖图， facade 定时器线程必须就绪。
TEST(DependencyDrivenPR2Test, ParkedTimeoutFiresOnFreshFacadeAndCascades) {
    kairo::Executor executor;
    ASSERT_TRUE(executor.initialize(
        timeout_config(2, kTimeoutBudgetMs)));

    Gate gate;
    auto upstream = gate.spawn_on(executor);
    ASSERT_TRUE(gate.entered_within());

    std::atomic<bool> ran_dependent{false};
    std::atomic<bool> ran_grandchild{false};
    const auto submitted_at = std::chrono::steady_clock::now();
    auto dependent = executor.submit_after_with_handle(
        upstream.handle, [&ran_dependent] {
            ran_dependent.store(true, std::memory_order_release);
            return 1;
        });
    auto grandchild = executor.submit_after_with_handle(
        dependent.handle, [&ran_grandchild] {
            ran_grandchild.store(true, std::memory_order_release);
            return 2;
        });

    // 未超预算前：dependent/grandchild 驻留 DependencyBlocked。
    {
        kairo::TaskLifecycleState state{};
        ASSERT_TRUE(lifecycle_state_of(executor, dependent.handle, state));
        EXPECT_EQ(state, kairo::TaskLifecycleState::DependencyBlocked);
    }

    // 超预算后：dependent future 限时（kSettleLimit 内）就绪。
    ASSERT_TRUE(settles_within(dependent.future));
    const auto settled_at = std::chrono::steady_clock::now();
    const auto elapsed_ms = std::chrono::duration_cast<
        std::chrono::milliseconds>(settled_at - submitted_at);
    EXPECT_GE(elapsed_ms.count(), kTimeoutBudgetMs / 2)
        << "timeout fired before half the budget elapsed";
    EXPECT_LT(elapsed_ms.count(), 5000)
        << "timeout did not fire near the budget";

    {
        bool timed_out = false;
        std::string message;
        bool other = false;
        try {
            (void)dependent.future.get();
        } catch (const kairo::TimedOutException& error) {
            timed_out = true;
            message = error.what();
        } catch (...) {
            other = true;
        }
        EXPECT_TRUE(timed_out) << "other=" << other;
        EXPECT_EQ(message, "Task timed out after " +
                               std::to_string(kTimeoutBudgetMs) + "ms");
    }
    EXPECT_FALSE(ran_dependent.load(std::memory_order_acquire));

    // 下游 parked 依赖级联失败：同一 TimedOutException 语义，未运行。
    ASSERT_TRUE(settles_within(grandchild.future));
    {
        bool timed_out = false;
        try {
            (void)grandchild.future.get();
        } catch (const kairo::TimedOutException&) {
            timed_out = true;
        } catch (...) {
            timed_out = false;
        }
        EXPECT_TRUE(timed_out);
    }
    EXPECT_FALSE(ran_grandchild.load(std::memory_order_acquire));

    // 超时统计：record 先于级联写入，grandchild 就绪后必然可见。
    EXPECT_GE(executor.get_failure_status().timeout_count, 1U);

    // 定时器获胜后，任务图节点应为终态（不在 in-flight 快照中滞留）。
    kairo::TaskLifecycleState state{};
    EXPECT_FALSE(lifecycle_state_of(executor, dependent.handle, state));

    gate.release();
    ASSERT_TRUE(settles_within(upstream.future));
    executor.shutdown();
}

// D1-a 控制组：先提交一个立即到期的 delayed 任务（启动 facade 定时器线程）
// 后，parked 超时应同样触发。若上一用例失败而本用例通过，则根因锁定为
// "定时器线程未启动时 schedule_once 静默失活"。
TEST(DependencyDrivenPR2Test, ParkedTimeoutFiresAfterTimerThreadPrimed) {
    kairo::Executor executor;
    ASSERT_TRUE(executor.initialize(
        timeout_config(2, kTimeoutBudgetMs)));

    // 预热定时器线程（5ms 一次性任务）。
    auto warmup = executor.submit_delayed(5, [] {});
    ASSERT_TRUE(settles_within(warmup));

    Gate gate;
    auto upstream = gate.spawn_on(executor);
    ASSERT_TRUE(gate.entered_within());

    std::atomic<bool> ran{false};
    const auto submitted_at = std::chrono::steady_clock::now();
    auto dependent = executor.submit_after_with_handle(
        upstream.handle, [&ran] {
            ran.store(true, std::memory_order_release);
            return 1;
        });

    ASSERT_TRUE(settles_within(dependent.future));
    const auto elapsed_ms = std::chrono::duration_cast<
        std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                   submitted_at);
    EXPECT_GE(elapsed_ms.count(), kTimeoutBudgetMs / 2);
    EXPECT_LT(elapsed_ms.count(), 5000);
    {
        bool timed_out = false;
        std::string message;
        try {
            (void)dependent.future.get();
        } catch (const kairo::TimedOutException& error) {
            timed_out = true;
            message = error.what();
        } catch (...) {
            timed_out = false;
        }
        EXPECT_TRUE(timed_out);
        EXPECT_EQ(message, "Task timed out after " +
                               std::to_string(kTimeoutBudgetMs) + "ms");
    }
    EXPECT_FALSE(ran.load(std::memory_order_acquire));
    // own future 结算不与统计写入定序（见 timeout_count_reaches 说明）。
    EXPECT_TRUE(timeout_count_reaches(executor, 1));

    gate.release();
    ASSERT_TRUE(settles_within(upstream.future));
    executor.shutdown();
}

// D1-b：预算内完成 → 正常执行、不触发超时。
TEST(DependencyDrivenPR2Test, ParkedWithinBudgetCompletesWithoutTimeout) {
    kairo::Executor executor;
    ASSERT_TRUE(executor.initialize(
        timeout_config(2, kTimeoutBudgetMs)));

    Gate gate;
    auto upstream = gate.spawn_on(executor);
    ASSERT_TRUE(gate.entered_within());

    auto dependent = executor.submit_after_with_handle(
        upstream.handle, [] { return 42; });

    // 确认驻留后立刻放行（事件驱动，远在预算内）。
    kairo::TaskLifecycleState state{};
    ASSERT_TRUE(lifecycle_state_of(executor, dependent.handle, state));
    gate.release();

    ASSERT_TRUE(settles_within(dependent.future));
    EXPECT_EQ(dependent.future.get(), 42);

    // 等预算窗口彻底流逝后确认未触发超时（观察窗口，非通过性时序）。
    wait_until_after(std::chrono::steady_clock::now() + kBudgetElapseWindow);
    EXPECT_EQ(executor.get_failure_status().timeout_count, 0U);

    executor.shutdown();
}

// D1-c：task_timeout_ms=0（默认）时 parked 永不超时（既有行为回归）。
TEST(DependencyDrivenPR2Test, ParkedNeverTimesOutWhenDisabled) {
    kairo::Executor executor;
    ASSERT_TRUE(executor.initialize(pool_config(2)));

    Gate gate;
    auto upstream = gate.spawn_on(executor);
    ASSERT_TRUE(gate.entered_within());

    std::atomic<bool> ran{false};
    const auto submitted_at = std::chrono::steady_clock::now();
    auto dependent = executor.submit_after_with_handle(
        upstream.handle, [&ran] {
            ran.store(true, std::memory_order_release);
            return 7;
        });

    // 等观察窗口流逝：必须仍驻留、future 未就绪、无超时统计。
    wait_until_after(std::chrono::steady_clock::now() + kBudgetElapseWindow);
    EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - submitted_at)
                  .count(),
              kTimeoutBudgetMs);
    kairo::TaskLifecycleState state{};
    ASSERT_TRUE(lifecycle_state_of(executor, dependent.handle, state));
    EXPECT_EQ(state, kairo::TaskLifecycleState::DependencyBlocked);
    EXPECT_FALSE(ran.load(std::memory_order_acquire));
    EXPECT_EQ(executor.get_failure_status().timeout_count, 0U);

    gate.release();
    ASSERT_TRUE(settles_within(dependent.future));
    EXPECT_EQ(dependent.future.get(), 7);

    executor.shutdown();
}

// D1-d（方向一）：取消先于超时 → 取消获胜，超时窗口流逝后不得再结算。
// 同步提交后立刻取消，两个事件被压进同一 200ms 预算窗口。
TEST(DependencyDrivenPR2Test, CancelWinsRaceAgainstParkedTimeout) {
    kairo::Executor executor;
    ASSERT_TRUE(executor.initialize(
        timeout_config(2, kTimeoutBudgetMs)));

    Gate gate;
    auto upstream = gate.spawn_on(executor);
    ASSERT_TRUE(gate.entered_within());

    std::atomic<bool> ran{false};
    auto dependent = executor.submit_after_with_handle(
        upstream.handle, [&ran] {
            ran.store(true, std::memory_order_release);
            return 3;
        });

    kairo::TaskLifecycleState state{};
    ASSERT_TRUE(lifecycle_state_of(executor, dependent.handle, state));
    EXPECT_EQ(state, kairo::TaskLifecycleState::DependencyBlocked);

    // 立即取消（预算窗口内）。
    const auto response = executor.request_task_cancel(dependent.handle);
    EXPECT_EQ(response.result,
              kairo::TaskCancellationResult::RequestedBeforeStart);

    ASSERT_TRUE(settles_within(dependent.future));
    {
        bool cancelled = false;
        kairo::TaskCancellationReason reason =
            kairo::TaskCancellationReason::Shutdown;
        try {
            (void)dependent.future.get();
        } catch (const kairo::TaskCancelled& error) {
            cancelled = true;
            reason = error.reason();
        } catch (...) {
            cancelled = false;
        }
        EXPECT_TRUE(cancelled);
        EXPECT_EQ(reason, kairo::TaskCancellationReason::Explicit);
    }
    EXPECT_FALSE(ran.load(std::memory_order_acquire));

    // 超时窗口流逝：不产生超时结算/统计（phase CAS 单赢家）。
    wait_until_after(std::chrono::steady_clock::now() + kBudgetElapseWindow);
    EXPECT_EQ(executor.get_failure_status().timeout_count, 0U);

    // 依赖随后正常完成，不受影响。
    gate.release();
    ASSERT_TRUE(settles_within(upstream.future));
    EXPECT_EQ(upstream.future.get(), 0);

    executor.shutdown();
}

// D1-d（方向二）：超时先于取消 → 超时获胜，后续取消落 AlreadyCompleted。
TEST(DependencyDrivenPR2Test, TimeoutWinsRaceAgainstLateCancel) {
    kairo::Executor executor;
    ASSERT_TRUE(executor.initialize(
        timeout_config(2, kTimeoutBudgetMs)));

    // 预热定时器线程（与主路径用例同因；见 ParkedTimeoutFiresAfterTimerThreadPrimed）。
    auto warmup = executor.submit_delayed(5, [] {});
    ASSERT_TRUE(settles_within(warmup));

    Gate gate;
    auto upstream = gate.spawn_on(executor);
    ASSERT_TRUE(gate.entered_within());

    auto dependent = executor.submit_after_with_handle(
        upstream.handle, [] { return 4; });

    // 等超时获胜（future 就绪即 phase 已终态）。
    ASSERT_TRUE(settles_within(dependent.future));
    {
        bool timed_out = false;
        try {
            (void)dependent.future.get();
        } catch (const kairo::TimedOutException&) {
            timed_out = true;
        } catch (...) {
            timed_out = false;
        }
        ASSERT_TRUE(timed_out);
    }

    // 迟到的取消必须落 AlreadyCompleted（不得改写终态、不得二次结算）。
    const auto response = executor.request_task_cancel(dependent.handle);
    EXPECT_EQ(response.result,
              kairo::TaskCancellationResult::AlreadyCompleted);

    // own future 结算不与统计写入定序（见 timeout_count_reaches 说明）。
    EXPECT_TRUE(timeout_count_reaches(executor, 1));

    gate.release();
    ASSERT_TRUE(settles_within(upstream.future));
    executor.shutdown();
}

// -------------------------------------------------------------------------
// D2：shutdown 终局结算（sweep）
// -------------------------------------------------------------------------

// D2-a：shutdown(false) 时存在"上游在队列中、worker 被占、依赖链永不可能
// 在 sweep 前推进"的 parked 链 → 所有 parked future 在 gate 仍阻塞时（即
// 依赖不可能已级联时）就被 sweep 结算；admission 计数随后归零。
TEST(DependencyDrivenPR2Test, ShutdownFalseSweepsParkedChainWithBlockedWorker) {
    kairo::Executor executor;
    auto config = pool_config(1);
    config.max_in_flight_tasks = 16;
    ASSERT_TRUE(executor.initialize(config));

    // 唯一 worker 被门占住；U 排队；D/D2 parked。
    Gate gate;
    auto gate_task = gate.spawn_on(executor);
    ASSERT_TRUE(gate.entered_within());

    std::atomic<bool> ran_u{false};
    auto upstream = executor.submit_with_handle([&ran_u] {
        ran_u.store(true, std::memory_order_release);
        return 10;
    });
    std::atomic<bool> ran_d{false};
    std::atomic<bool> ran_d2{false};
    auto dependent = executor.submit_after_with_handle(
        upstream.handle, [&ran_d] {
            ran_d.store(true, std::memory_order_release);
            return 11;
        });
    auto grandchild = executor.submit_after_with_handle(
        dependent.handle, [&ran_d2] {
            ran_d2.store(true, std::memory_order_release);
            return 12;
        });

    EXPECT_EQ(executor.get_in_flight_submissions(), 4U);

    executor.shutdown(/*wait_for_tasks=*/false);

    // 关键窗口：gate 仍阻塞（U 不可能已运行/级联），parked future 必须已被
    // sweep 结算。
    ASSERT_TRUE(settles_within(dependent.future, kPromptLimit));
    {
        const bool settled = inspect_exception(dependent.future, [](std::exception_ptr e) {
            return exception_message(e).find("Executor is shutting down") !=
                   std::string::npos;
        });
        EXPECT_TRUE(settled) << "dependent did not settle with shutdown sweep "
                                "semantics";
    }
    EXPECT_FALSE(ran_d.load(std::memory_order_acquire));

    ASSERT_TRUE(settles_within(grandchild.future, kPromptLimit));
    {
        const bool settled = inspect_exception(grandchild.future, [](std::exception_ptr e) {
            return exception_message(e).find("Executor is shutting down") !=
                   std::string::npos;
        });
        EXPECT_TRUE(settled);
    }
    EXPECT_FALSE(ran_d2.load(std::memory_order_acquire));
    // sweep 已释放两个 parked 名额：只剩 gate + 队列中的 U。
    EXPECT_EQ(executor.get_in_flight_submissions(), 2U);

    // 收尾：放行 gate → worker 排空队列（U 执行）→ 计数归零。
    gate.release();
    ASSERT_TRUE(settles_within(gate_task.future));
    EXPECT_TRUE(settles_within(upstream.future));
    EXPECT_EQ(executor.get_in_flight_submissions(), 0U);

    executor.shutdown();
}

// D2-b：shutdown(true) 带 parked 链 → 限时收敛。shutdown(true) 先等池排空
// 再停池（Executor::shutdown 的 wait 先于 manager shutdown），因此等待期间
// gate 放行后链路经"活池"正常 drain 收敛——dependent 既可能被重新接纳执行
// （在停池前出队），也可能在停池后被拒绝结算，两者都是合法收敛；本用例的
// 契约是"不悬空、限时收敛"（旧实现此处 parked future 永久悬空）。
TEST(DependencyDrivenPR2Test, ShutdownTrueResolvesParkedChainWithinBound) {
    kairo::Executor executor;
    auto config = pool_config(1);
    config.max_in_flight_tasks = 16;
    ASSERT_TRUE(executor.initialize(config));

    Gate gate;
    auto gate_task = gate.spawn_on(executor);
    ASSERT_TRUE(gate.entered_within());

    auto upstream = executor.submit_with_handle([] { return 20; });
    auto dependent = executor.submit_after_with_handle(
        upstream.handle, [] { return 21; });
    auto grandchild = executor.submit_after_with_handle(
        dependent.handle, [] { return 22; });

    kairo::TaskLifecycleState state{};
    ASSERT_TRUE(lifecycle_state_of(executor, dependent.handle, state));
    EXPECT_EQ(state, kairo::TaskLifecycleState::DependencyBlocked);

    // 辅助线程在 250ms 预算后放行 gate：shutdown(true) 主线程进入池等待
    // 阶段（微秒级）后链路才可能推进。
    std::thread releaser([&gate] {
        gate.release_after(std::chrono::milliseconds{250});
    });

    const auto shutdown_started = std::chrono::steady_clock::now();
    executor.shutdown(/*wait_for_tasks=*/true);
    const auto shutdown_ms = std::chrono::duration_cast<
        std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                   shutdown_started);
    releaser.join();
    EXPECT_LT(shutdown_ms.count(), 30000)
        << "shutdown(true) did not converge with a parked chain";

    // 所有 future 必须在有限时间内结算（值或拒绝异常均为合法终态）。
    ASSERT_TRUE(settles_within(dependent.future, kPromptLimit));
    ASSERT_TRUE(settles_within(grandchild.future, kPromptLimit));
    EXPECT_EQ(executor.get_in_flight_submissions(), 0U);

    executor.shutdown();
}

// D2-c：worker 线程内发起 shutdown（三条 sweep 调用路径的第一条分支）——
// sweep 在 worker 上执行，parked 下游仍须限时结算。
TEST(DependencyDrivenPR2Test, ShutdownFromWorkerSweepsParkedDependents) {
    kairo::Executor executor;
    auto config = pool_config(2);
    config.max_in_flight_tasks = 16;
    ASSERT_TRUE(executor.initialize(config));

    Gate gate;
    auto gate_task = gate.spawn_on(executor);
    ASSERT_TRUE(gate.entered_within());

    std::atomic<bool> ran{false};
    auto dependent = executor.submit_after_with_handle(
        gate_task.handle, [&ran] {
            ran.store(true, std::memory_order_release);
            return 30;
        });
    kairo::TaskLifecycleState state{};
    ASSERT_TRUE(lifecycle_state_of(executor, dependent.handle, state));
    EXPECT_EQ(state, kairo::TaskLifecycleState::DependencyBlocked);

    // 第二个 worker 上发起 shutdown(false)：manager shutdown 走
    // is_current_worker_thread 分支，返回后 facade 在该 worker 上执行 sweep。
    std::promise<void> shutdown_done;
    auto shutdown_done_future = shutdown_done.get_future();
    auto shutdowner = executor.submit_with_handle([&executor, &shutdown_done] {
        (void)executor.shutdown(/*wait_for_tasks=*/false);
        shutdown_done.set_value();
    });

    ASSERT_TRUE(settles_within(dependent.future));
    {
        const bool settled = inspect_exception(dependent.future, [](std::exception_ptr e) {
            return exception_message(e).find("Executor is shutting down") !=
                   std::string::npos;
        });
        EXPECT_TRUE(settled);
    }
    EXPECT_FALSE(ran.load(std::memory_order_acquire));
    ASSERT_TRUE(shutdown_done_future.wait_for(kSettleLimit) ==
                std::future_status::ready);
    // shutdowner 的 admission 在其 wrapper 收尾时释放；先等它终态再读计数。
    ASSERT_TRUE(settles_within(shutdowner.future));

    EXPECT_EQ(executor.get_in_flight_submissions(), 1U);  // 仅剩 gate
    gate.release();
    ASSERT_TRUE(settles_within(gate_task.future));
    ASSERT_TRUE(settles_within(shutdowner.future));
    EXPECT_EQ(executor.get_in_flight_submissions(), 0U);

    executor.shutdown();
}

// -------------------------------------------------------------------------
// retention 交互：trim 不得驱逐仍被 parked dependent 引用的依赖节点
// -------------------------------------------------------------------------

TEST(DependencyDrivenPR2Test, ParkedDependentSurvivesRetentionChurn) {
    kairo::Executor executor;
    ASSERT_TRUE(executor.initialize(pool_config(3)));

    // A 快速成功成为终态节点；B 门控驻留。
    auto a = executor.submit_with_handle([] { return 1; });
    ASSERT_TRUE(settles_within(a.future));
    EXPECT_EQ(a.future.get(), 1);

    Gate gate_b;
    auto b = gate_b.spawn_on(executor);
    ASSERT_TRUE(gate_b.entered_within());

    // D 依赖 {A, B}：A 已成功，unmet=1，parked；A 是被 D 引用的终态节点。
    std::atomic<bool> ran_d{false};
    auto d = executor.submit_after_with_handle(
        std::vector<kairo::TaskHandle>{a.handle, b.handle}, [&ran_d] {
            ran_d.store(true, std::memory_order_release);
            return 42;
        });
    kairo::TaskLifecycleState state{};
    ASSERT_TRUE(lifecycle_state_of(executor, d.handle, state));
    EXPECT_EQ(state, kairo::TaskLifecycleState::DependencyBlocked);

    // 极小 retention + 大量无关任务搅动 trim。
    executor.set_task_graph_retention_capacity(2);
    constexpr int kChurn = 150;
    for (int i = 0; i < kChurn; ++i) {
        auto churn = executor.submit_with_handle([i] { return i; });
        ASSERT_TRUE(settles_within(churn.future));
        EXPECT_EQ(churn.future.get(), i);
    }

    // 放行 B：D 必须以正确结果完成，而非 "dependency handle is invalid"。
    gate_b.release();
    ASSERT_TRUE(settles_within(d.future));
    EXPECT_EQ(d.future.get(), 42);
    EXPECT_TRUE(ran_d.load(std::memory_order_acquire));

    executor.shutdown();
}

// -------------------------------------------------------------------------
// serial dispatch（submit_on）与依赖图组合
// -------------------------------------------------------------------------

// submit_on 句柄作为 submit_after 依赖：成功/失败两路径。
TEST(DependencyDrivenPR2Test, SerialHandleAsDependencySuccessAndFailure) {
    kairo::Executor executor;
    ASSERT_TRUE(executor.initialize(pool_config(2)));
    kairo::SerialExecutionContext context;

    auto serial_ok = executor.submit_on_with_handle(context, [] { return 11; });
    std::atomic<bool> ran_dep{false};
    auto dependent = executor.submit_after_with_handle(
        serial_ok.handle, [&ran_dep] {
            ran_dep.store(true, std::memory_order_release);
            return 22;
        });

    ASSERT_TRUE(settles_within(serial_ok.future));
    EXPECT_EQ(serial_ok.future.get(), 11);
    ASSERT_TRUE(settles_within(dependent.future));
    EXPECT_EQ(dependent.future.get(), 22);
    EXPECT_TRUE(ran_dep.load(std::memory_order_acquire));

    // 失败路径：serial 任务抛异常 → parked dependent 携带同一异常语义结算。
    auto serial_bad = executor.submit_on_with_handle(
        context, []() -> int { throw std::runtime_error("serial boom"); });
    std::atomic<bool> ran_bad_dep{false};
    auto bad_dependent = executor.submit_after_with_handle(
        serial_bad.handle, [&ran_bad_dep] {
            ran_bad_dep.store(true, std::memory_order_release);
            return 99;
        });

    ASSERT_TRUE(settles_within(serial_bad.future));
    {
        bool runtime_failure = false;
        std::string message;
        try {
            (void)serial_bad.future.get();
        } catch (const std::runtime_error& error) {
            runtime_failure = true;
            message = error.what();
        }
        EXPECT_TRUE(runtime_failure);
        EXPECT_EQ(message, "serial boom");
    }

    ASSERT_TRUE(settles_within(bad_dependent.future));
    {
        bool runtime_failure = false;
        bool cancelled = false;
        std::string message;
        try {
            (void)bad_dependent.future.get();
        } catch (const kairo::TaskCancelled&) {
            cancelled = true;
        } catch (const std::runtime_error& error) {
            runtime_failure = true;
            message = error.what();
        }
        EXPECT_TRUE(runtime_failure);
        EXPECT_FALSE(cancelled);
        EXPECT_EQ(message, "serial boom");
    }
    EXPECT_FALSE(ran_bad_dep.load(std::memory_order_acquire));

    context.shutdown();
    executor.shutdown();
}

// serial 任务失败后，失败统计必须先于 dependent future 可见
// （drain bag 化 + record_task_exception 提前的观测不变式）。
TEST(DependencyDrivenPR2Test,
     SerialFailureStatsVisibleBeforeDependentFuture) {
    kairo::Executor executor;
    ASSERT_TRUE(executor.initialize(pool_config(2)));
    kairo::SerialExecutionContext context;

    const auto before = executor.get_failure_status().task_exception_count;

    auto serial_bad = executor.submit_on_with_handle(
        context, []() -> int { throw std::runtime_error("stats boom"); });
    auto dependent = executor.submit_after_with_handle(
        serial_bad.handle, [] { return 1; });

    ASSERT_TRUE(settles_within(dependent.future));
    // dependent future 就绪晚于上游 mark_task_graph_failed，而 mark 前统计
    // 已写入：此处读取必须无竞态地看到计数。
    const auto after = executor.get_failure_status().task_exception_count;
    EXPECT_GT(after, before);

    context.shutdown();
    executor.shutdown();
}

// shutdown(false) 时在途 serial 派发被丢弃/拒绝 → 其 parked 下游限时结算，
// serial callable 不运行，admission 收敛。
TEST(DependencyDrivenPR2Test,
     ShutdownFalseSettlesParkedDownstreamOfSerialDispatch) {
    kairo::Executor executor;
    auto config = pool_config(1);
    config.max_in_flight_tasks = 16;
    ASSERT_TRUE(executor.initialize(config));
    kairo::SerialExecutionContext context;

    Gate gate;
    auto gate_task = gate.spawn_on(executor);
    ASSERT_TRUE(gate.entered_within());  // 唯一 worker 被占

    std::atomic<bool> ran_serial{false};
    auto serial = executor.submit_on_with_handle(context, [&ran_serial] {
        ran_serial.store(true, std::memory_order_release);
        return 5;
    });
    // publish_task 排在池队列中（worker 被占），dependent parked。
    std::atomic<bool> ran_dep{false};
    auto dependent = executor.submit_after_with_handle(
        serial.handle, [&ran_dep] {
            ran_dep.store(true, std::memory_order_release);
            return 6;
        });
    EXPECT_EQ(executor.get_in_flight_submissions(), 3U);

    // 先停串行上下文，再 shutdown(false)：排空中的池执行 publish_task 时
    // post_reserved 必然拒绝（或包装被丢弃时 TicketGuard 兜底）——两条
    // 路径都须把 serial 与 parked 下游结算为 ExecutorStopping 语义。
    // 池为严格排空语义，publish_task 在 gate 放行、worker 排空队列时执行，
    // 因此先放行 gate 再做有限等待断言。
    context.shutdown();
    executor.shutdown(/*wait_for_tasks=*/false);

    gate.release();
    ASSERT_TRUE(settles_within(gate_task.future));

    ASSERT_TRUE(settles_within(serial.future, kPromptLimit));
    {
        bool stopping = false;
        std::string message;
        try {
            (void)serial.future.get();
        } catch (const kairo::ExecutorStopping& error) {
            stopping = true;
            message = error.what();
        } catch (...) {
            stopping = false;
        }
        EXPECT_TRUE(stopping) << "serial settled as: " << message;
    }
    EXPECT_FALSE(ran_serial.load(std::memory_order_acquire));

    ASSERT_TRUE(settles_within(dependent.future, kPromptLimit));
    {
        // 合法结算语义二选一：(a) serial 拒绝级联的 ExecutorStopping；
        // (b) shutdown(false) 的 D2 sweep 抢先结算的 "Executor is shutting
        // down" runtime_error。两者都是 shutdown 路径失败，不允许的是
        // 正常值或其他异常。
        bool stopping = false;
        bool swept = false;
        std::string detail;
        try {
            (void)dependent.future.get();
        } catch (const kairo::ExecutorStopping& error) {
            stopping = true;
            detail = error.what();
        } catch (const std::exception& error) {
            swept = exception_message(std::current_exception())
                        .find("Executor is shutting down") !=
                    std::string::npos;
            detail = error.what();
        } catch (...) {
            detail = "<non-std>";
        }
        EXPECT_TRUE(stopping || swept)
            << "dependent settled as: " << detail;
    }
    EXPECT_FALSE(ran_dep.load(std::memory_order_acquire));

    EXPECT_EQ(executor.get_in_flight_submissions(), 0U);

    executor.shutdown();
}

// -------------------------------------------------------------------------
// admission 四路径恰好一次
// -------------------------------------------------------------------------

// 路径一：依赖失败级联结算 parked dependent → 计数回基线。
TEST(DependencyDrivenPR2Test, AdmissionReleasedOnDependencyFailurePath) {
    kairo::Executor executor;
    auto config = pool_config(2);
    config.max_in_flight_tasks = 8;
    ASSERT_TRUE(executor.initialize(config));

    Gate gate_upstream;
    auto upstream = gate_upstream.spawn_on(executor);
    ASSERT_TRUE(gate_upstream.entered_within());

    // 失败依赖同样门控驻留：dependent 提交时它尚未终态，且两个 worker 均
    // 被占住，提交计数断言无竞态。
    Gate gate_failing;
    auto failing = executor.submit_after_with_handle(
        gate_failing.spawn_on(executor).handle,
        []() -> int { throw std::runtime_error("admission boom"); });
    ASSERT_TRUE(gate_failing.entered_within());

    std::atomic<bool> ran{false};
    auto dependent = executor.submit_after_with_handle(
        std::vector<kairo::TaskHandle>{upstream.handle, failing.handle},
        [&ran] {
            ran.store(true, std::memory_order_release);
            return 1;
        });

    EXPECT_EQ(executor.get_in_flight_submissions(), 4U);

    // 放行失败依赖：failing 抛出 → 级联失败结算 dependent。
    gate_failing.release();
    ASSERT_TRUE(settles_within(failing.future));
    {
        bool runtime_failure = false;
        try {
            (void)failing.future.get();
        } catch (const std::runtime_error&) {
            runtime_failure = true;
        }
        EXPECT_TRUE(runtime_failure);
    }
    ASSERT_TRUE(settles_within(dependent.future));
    {
        bool runtime_failure = false;
        try {
            (void)dependent.future.get();
        } catch (const std::runtime_error&) {
            runtime_failure = true;
        }
        EXPECT_TRUE(runtime_failure);
    }
    EXPECT_FALSE(ran.load(std::memory_order_acquire));
    // 恰好一次释放的最终证据：全部在途任务终态后计数必须精确归零
    // （漏释放会留在正数；AdmissionReleaser 的 CAS 防御重复释放）。
    gate_upstream.release();
    ASSERT_TRUE(settles_within(upstream.future));
    EXPECT_EQ(executor.get_in_flight_submissions(), 0U);

    executor.shutdown();
}

// 路径二：parked 超时结算 → 计数回基线。
TEST(DependencyDrivenPR2Test, AdmissionReleasedOnParkedTimeoutPath) {
    kairo::Executor executor;
    auto config = timeout_config(2, kTimeoutBudgetMs, /*max_in_flight=*/8);
    ASSERT_TRUE(executor.initialize(config));

    // 预热定时器线程（见 D1 主用例说明）。
    auto warmup = executor.submit_delayed(5, [] {});
    ASSERT_TRUE(settles_within(warmup));

    Gate gate;
    auto upstream = gate.spawn_on(executor);
    ASSERT_TRUE(gate.entered_within());

    auto dependent = executor.submit_after_with_handle(
        upstream.handle, [] { return 1; });
    EXPECT_EQ(executor.get_in_flight_submissions(), 2U);

    ASSERT_TRUE(settles_within(dependent.future));
    EXPECT_EQ(executor.get_in_flight_submissions(), 1U);  // 仅剩 gate

    gate.release();
    ASSERT_TRUE(settles_within(upstream.future));
    EXPECT_EQ(executor.get_in_flight_submissions(), 0U);

    executor.shutdown();
}

// 路径三：parked 取消结算 → 计数回基线。
TEST(DependencyDrivenPR2Test, AdmissionReleasedOnParkedCancelPath) {
    kairo::Executor executor;
    auto config = pool_config(2);
    config.max_in_flight_tasks = 8;
    ASSERT_TRUE(executor.initialize(config));

    Gate gate;
    auto upstream = gate.spawn_on(executor);
    ASSERT_TRUE(gate.entered_within());

    auto dependent = executor.submit_after_with_handle(
        upstream.handle, [] { return 1; });
    EXPECT_EQ(executor.get_in_flight_submissions(), 2U);

    const auto response = executor.request_task_cancel(dependent.handle);
    EXPECT_EQ(response.result,
              kairo::TaskCancellationResult::RequestedBeforeStart);
    ASSERT_TRUE(settles_within(dependent.future));
    EXPECT_EQ(executor.get_in_flight_submissions(), 1U);  // 仅剩 gate

    gate.release();
    ASSERT_TRUE(settles_within(upstream.future));
    EXPECT_EQ(executor.get_in_flight_submissions(), 0U);

    executor.shutdown();
}

// 路径四：shutdown sweep 结算 parked 链 → 计数在全部在途任务收尾后回零。
TEST(DependencyDrivenPR2Test, AdmissionReleasedOnShutdownSweepPath) {
    kairo::Executor executor;
    auto config = pool_config(1);
    config.max_in_flight_tasks = 8;
    ASSERT_TRUE(executor.initialize(config));

    Gate gate;
    auto gate_task = gate.spawn_on(executor);
    ASSERT_TRUE(gate.entered_within());

    auto upstream = executor.submit_with_handle([] { return 1; });
    auto dependent = executor.submit_after_with_handle(
        upstream.handle, [] { return 2; });
    EXPECT_EQ(executor.get_in_flight_submissions(), 3U);

    executor.shutdown(/*wait_for_tasks=*/false);
    ASSERT_TRUE(settles_within(dependent.future, kPromptLimit));
    // sweep 释放 dependent；gate + 队列中的 upstream 仍占 2。
    EXPECT_EQ(executor.get_in_flight_submissions(), 2U);

    gate.release();
    ASSERT_TRUE(settles_within(gate_task.future));
    EXPECT_TRUE(settles_within(upstream.future));
    EXPECT_EQ(executor.get_in_flight_submissions(), 0U);

    executor.shutdown();
}

}  // namespace
