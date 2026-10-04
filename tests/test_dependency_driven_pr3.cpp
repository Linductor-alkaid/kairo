// PR-3（dependency-driven scheduling 后续）独立验证测试。
//
// 被测行为（相对 PR-2 的增量）：
//   Q1  监控 Queued 补记：parked 任务经 drain_parked_resolutions 出队被接受
//       后，in-flight 生命周期补记 TaskLifecycleState::Queued；在 worker 被
//       占位任务持续占用期间该状态稳定可采样；占位释放后任务经 Running
//       执行并以 Succeeded 终态离开快照（DependencyBlocked → Queued →
//       Running → 终态，设计 §7）。
//   G1  closure 墓地观测：parked 超时触发（赢家路径）后
//       closure_graveyard_size() >= 2；shutdown(false) 的 D2 sweep 在仍有
//       parked 节点时末尾清空墓地（shutdown 后 == 0）。
//   G2  墓地清空完备性：parked 超时已结算、shutdown 时已无任何 parked 节点
//       的干净终局，shutdown 后 closure_graveyard_size() 同样必须为 0
//       （API 契约："shutdown 终局清空"，不限 sweep 是否有 parked 可扫）。
//
// 测试模式：与 PR-1/PR-2 相同——门控握手 + 有限等待；"状态保持稳定"的观察
// 窗口有界，不作为通过/失败的 sleep 时序依据。

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

private:
    std::promise<void> release_channel_;
    std::shared_future<void> release_;
    std::future<void> entered_;
    std::atomic<bool> released_{false};
};

// 有限等待 future<int> 就绪；不就绪返回 false（配合断言防挂死）。
template <typename T>
bool settles_within(kairo::TimerSubmission<T>& submission,
                    std::chrono::milliseconds limit = kSettleLimit);

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

template <typename T>
bool settles_within(kairo::TimerSubmission<T>& submission,
                    std::chrono::milliseconds limit) {
    return settles_within(submission.future, limit);
}

// 有界等待谓词成立（轮询诊断快照/接口用，5ms 步进）。
template <typename Predicate>
bool waits_until(Predicate&& predicate,
                 std::chrono::milliseconds limit = kSettleLimit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    for (;;) {
        if (predicate()) {
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

// -------------------------------------------------------------------------
// Q1：drain 出队被接受后补记 Queued（worker 被占期间稳定可采样）
// -------------------------------------------------------------------------

// 单 worker 池。上游依赖 U 以门 A 占住 worker；占位任务 P 排在其后（池队列
// FIFO）。释放门 A：U 完成 → drain 将 parked 的 D 定向入队（补记 Queued），
// worker 紧接着取走队头的 P 并阻塞在门 B 上——此后 D 滞留队列，其 Queued
// 状态无需竞速即可确定性采样。释放门 B 后 D 经 Running 执行、Succeeded 离场。
TEST(DependencyDrivenPR3Test, ParkedDrainRecordsQueuedWhileWorkerOccupied) {
    kairo::Executor executor;
    auto config = pool_config(1);
    config.max_in_flight_tasks = 16;
    ASSERT_TRUE(executor.initialize(config));

    // 上游依赖：门 A 占住唯一 worker。
    Gate gate_upstream;
    auto upstream = gate_upstream.spawn_on(executor);
    ASSERT_TRUE(gate_upstream.entered_within());

    // 占位任务：排在池队列中，稍后占住 worker 阻挡 D 被取走。
    Gate gate_placeholder;
    auto placeholder = gate_placeholder.spawn_on(executor);

    // dependent：上游未终态 → parked 驻留 DependencyBlocked。
    std::atomic<bool> ran_dependent{false};
    // callable 内自采样：进入执行即应处于 Running（mark_task_graph_running
    // 先于 callable 调用）。句柄槽位在提交返回后回填——callable 受两道门
    // 保护，必然晚于回填执行。
    std::atomic<int> self_observed_running{-1};
    auto self = std::make_shared<kairo::TaskHandle>();
    auto dependent = executor.submit_after_with_handle(
        upstream.handle,
        [&ran_dependent, &executor, &self_observed_running, self] {
            ran_dependent.store(true, std::memory_order_release);
            bool running = false;
            const auto snapshot = executor.get_snapshot();
            for (const auto& task : snapshot.in_flight_tasks) {
                if (task.task_id == self->id()) {
                    running = task.state ==
                              kairo::TaskLifecycleState::Running;
                    break;
                }
            }
            self_observed_running.store(running ? 1 : 0,
                                        std::memory_order_release);
            return 42;
        });
    *self = dependent.handle;

    // 提交即驻留 DependencyBlocked（PR-1 既有行为回归）。
    {
        kairo::TaskLifecycleState state{};
        ASSERT_TRUE(lifecycle_state_of(executor, dependent.handle, state));
        EXPECT_EQ(state, kairo::TaskLifecycleState::DependencyBlocked);
    }

    // 释放门 A：U 完成 → drain 定向入队 D（补记 Queued）→ worker 取走队头
    // 的占位任务并阻塞在门 B。占位任务"已进入"握手保证 drain 已执行且
    // worker 已被重新占住。
    gate_upstream.release();
    ASSERT_TRUE(settles_within(upstream.future));
    EXPECT_EQ(upstream.future.get(), 0);
    ASSERT_TRUE(gate_placeholder.entered_within());

    // 确定性采样：D 已出队（被接受）但 worker 被占 → 必须 = Queued。
    {
        kairo::TaskLifecycleState state{};
        ASSERT_TRUE(lifecycle_state_of(executor, dependent.handle, state))
            << "dependent left the in-flight snapshot before running";
        EXPECT_EQ(state, kairo::TaskLifecycleState::Queued);
    }
    // 状态稳定性观察窗口：worker 仍被占位任务占住，D 必须持续 Queued、
    // 未被执行（有界窗口，非通过性时序）。
    std::this_thread::sleep_for(std::chrono::milliseconds{300});
    {
        kairo::TaskLifecycleState state{};
        ASSERT_TRUE(lifecycle_state_of(executor, dependent.handle, state));
        EXPECT_EQ(state, kairo::TaskLifecycleState::Queued)
            << "Queued record was lost or prematurely advanced";
        EXPECT_FALSE(ran_dependent.load(std::memory_order_acquire));
    }

    // 释放占位任务：worker 取走 D → Running → Succeeded。
    gate_placeholder.release();
    ASSERT_TRUE(settles_within(placeholder.future));
    ASSERT_TRUE(settles_within(dependent.future));
    EXPECT_EQ(dependent.future.get(), 42);
    EXPECT_TRUE(ran_dependent.load(std::memory_order_acquire));
    // callable 自采样：执行期间自身生命周期为 Running（补记链
    // DependencyBlocked → Queued → Running 的最后一环）。
    EXPECT_EQ(self_observed_running.load(std::memory_order_acquire), 1)
        << "dependent callable did not observe its own Running state";

    // 终态离场：不再滞留 in-flight 快照。
    {
        kairo::TaskLifecycleState state{};
        EXPECT_FALSE(lifecycle_state_of(executor, dependent.handle, state));
    }

    EXPECT_EQ(executor.get_in_flight_submissions(), 0U);
    executor.shutdown();
}

// -------------------------------------------------------------------------
// G1：parked 超时 → 墓地 >= 2；含 parked 节点的 shutdown sweep 清空墓地
// -------------------------------------------------------------------------

TEST(DependencyDrivenPR3Test,
     GraveyardGrowsOnParkedTimeoutAndSweepClearsOnShutdown) {
    kairo::Executor executor;
    ASSERT_TRUE(executor.initialize(
        timeout_config(2, kTimeoutBudgetMs, /*max_in_flight=*/16)));

    // 预热定时器线程（PR-2 同因：避免 fresh facade 定时器路径的已知噪声）。
    auto warmup = executor.submit_delayed(5, [] {});
    ASSERT_TRUE(settles_within(warmup));

    Gate gate;
    auto upstream = gate.spawn_on(executor);
    ASSERT_TRUE(gate.entered_within());

    std::atomic<bool> ran{false};
    auto dependent = executor.submit_after_with_handle(
        upstream.handle, [&ran] {
            ran.store(true, std::memory_order_release);
            return 1;
        });

    // parked 超预算：超时赢家结算，future 限时就绪。
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
    EXPECT_FALSE(ran.load(std::memory_order_acquire));

    // 赢家路径把 promise/state 捕获转入墓地（先于 future 结算完成的写入
    // 可能晚于 future 就绪，故有界轮询）：>= 2 即 promise + state。
    ASSERT_TRUE(waits_until([&executor] {
        return executor.closure_graveyard_size() >= 2U;
    })) << "closure_graveyard_size did not observe the parked timeout, size="
        << executor.closure_graveyard_size();

    // 再驻留一个 parked 节点后 shutdown(false)：D2 sweep 结算它并在末尾
    // 清空墓地。shutdown 先停定时器线程，其超时定时器不再触发。
    auto still_parked = executor.submit_after_with_handle(
        upstream.handle, [] { return 2; });
    {
        kairo::TaskLifecycleState state{};
        ASSERT_TRUE(lifecycle_state_of(executor, still_parked.handle, state));
        EXPECT_EQ(state, kairo::TaskLifecycleState::DependencyBlocked);
    }

    executor.shutdown(/*wait_for_tasks=*/false);

    // sweep 末尾清空：shutdown 返回后墓地必须为空。
    EXPECT_EQ(executor.closure_graveyard_size(), 0U)
        << "shutdown sweep did not clear the closure graveyard";

    // sweep 结算仍 parked 的节点（不悬空），收尾放行门并确认计数归零。
    ASSERT_TRUE(settles_within(still_parked.future, kPromptLimit));
    gate.release();
    ASSERT_TRUE(settles_within(upstream.future));
    EXPECT_EQ(executor.get_in_flight_submissions(), 0U);

    executor.shutdown();
}

// -------------------------------------------------------------------------
// G2：干净终局（shutdown 时已无 parked 节点）同样必须清空墓地
// -------------------------------------------------------------------------

// 超时结算完成后放行上游、全部任务终态，再 shutdown。此时已无任何 parked
// 节点，但 API 契约是"shutdown 终局清空"——墓地清空不得依赖 sweep 是否
// 恰好有 parked 节点可扫。
TEST(DependencyDrivenPR3Test,
     GraveyardClearedOnCleanShutdownWithoutParkedTasks) {
    kairo::Executor executor;
    ASSERT_TRUE(executor.initialize(
        timeout_config(2, kTimeoutBudgetMs, /*max_in_flight=*/16)));

    auto warmup = executor.submit_delayed(5, [] {});
    ASSERT_TRUE(settles_within(warmup));

    Gate gate;
    auto upstream = gate.spawn_on(executor);
    ASSERT_TRUE(gate.entered_within());

    auto dependent = executor.submit_after_with_handle(
        upstream.handle, [] { return 3; });

    // parked 超时触发并落入墓地。
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
    ASSERT_TRUE(waits_until([&executor] {
        return executor.closure_graveyard_size() >= 2U;
    })) << "closure_graveyard_size did not observe the parked timeout, size="
        << executor.closure_graveyard_size();

    // 等观察窗口流逝：上游任务自身排队超时定时器的输家路径也已落墓地，
    // 规模稳定后记录终局前快照。
    std::this_thread::sleep_for(kBudgetElapseWindow);
    const auto size_before_shutdown = executor.closure_graveyard_size();
    EXPECT_GE(size_before_shutdown, 2U);

    // 放行上游 → 全部任务终态（无任何 parked 节点）→ shutdown。
    gate.release();
    ASSERT_TRUE(settles_within(upstream.future));
    EXPECT_EQ(executor.get_in_flight_submissions(), 0U);

    executor.shutdown();

    // 契约：shutdown 终局清空墓地，与是否存在 parked 节点无关。
    EXPECT_EQ(executor.closure_graveyard_size(), 0U)
        << "shutdown left " << executor.closure_graveyard_size()
        << " graveyard entries behind on a clean (no parked tasks) shutdown";
}

}  // namespace
