// PR-1（dependency-driven scheduling）独立验证测试。
//
// 被测行为：submit_after / submit_after_with_handle 的依赖未就绪任务不再
// 立即入队（parked 在任务图节点上），由依赖终态级联出队提交；依赖失败则
// 不运行 callable 直接结算。核心回归点是"parked 任务不占用 worker"。
//
// 测试模式：门控握手。被测任务阻塞在 std::promise/future 上，测试线程
// 显式放行；所有 future 等待均为有限等待（kSettleLimit），不依赖 sleep
// 时序，超时即判失败（防挂死）。
//
// drain 出队被拒分支（原 test 8，shutdown(false) 后放行依赖触发）现已
// 恢复：retired 保活 + 终局排空链（ThreadPoolExecutor::retired_pools_ /
// ExecutorManager::retired_async_executors_）保证孤儿池 worker 在 facade
// 析构前全部 join，本用例裸跑（无任何收尾缓解）即为该保证的直接回归。
// 此前该模式 ~31% 概率在析构/退出时 UAF 段错误（TSAN heap-use-after-free
// 证实：drain 的 record_submit_rejected / monitor 触碰已释放 facade）。

#include <kairo/executor.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace std::chrono_literals;

namespace {

// 单个 future 的有限等待上限；超时即失败（防挂死）。
constexpr auto kSettleLimit = std::chrono::seconds{10};
// 应当"立即"就绪的路径（提交拒绝/容量拒绝）的收紧上限。
constexpr auto kPromptLimit = std::chrono::seconds{2};

kairo::ExecutorConfig pool_config(std::size_t workers,
                                     std::size_t queue_capacity = 64) {
    kairo::ExecutorConfig config;
    config.min_threads = workers;
    config.max_threads = workers;
    config.queue_capacity = queue_capacity;
    return config;
}

// 门控任务：进入即报数并阻塞在门上，测试线程显式 release() 放行。
class Gate {
public:
    Gate() : release_(release_channel_.get_future().share()) {}

    Gate(const Gate&) = delete;
    Gate& operator=(const Gate&) = delete;

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

    void release() { release_channel_.set_value(); }

private:
    std::promise<void> release_channel_;
    std::shared_future<void> release_;
    std::future<void> entered_;
};

// 有限等待 future 就绪；不就绪返回 false（配合 ASSERT_TRUE 防挂死）。
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

// 1a：宽依赖（1 依赖 8 前置）。前置未全部成功前 callable 不执行
// （DependencyBlocked 驻留），全部成功后执行且结果正确。
TEST(DependencyDrivenSchedulingTest, WideDependencyRunsOnlyAfterAllPrereqsSucceed) {
    kairo::Executor executor;
    ASSERT_TRUE(executor.initialize(pool_config(8)));

    constexpr std::size_t kPrereqs = 8;
    std::vector<Gate> gates(kPrereqs);
    std::vector<kairo::TaskSubmission<int>> upstreams;
    for (auto& gate : gates) {
        upstreams.push_back(gate.spawn_on(executor));
    }
    for (auto& gate : gates) {
        ASSERT_TRUE(gate.entered_within());
    }

    std::atomic<bool> ran{false};
    std::vector<kairo::TaskHandle> handles;
    handles.reserve(kPrereqs);
    for (const auto& upstream : upstreams) {
        handles.push_back(upstream.handle);
    }
    auto dependent = executor.submit_after_with_handle(
        handles, [&ran] {
            ran.store(true, std::memory_order_release);
            return 99;
        });

    // 全部前置被门阻塞：dependent 必须 parked（DependencyBlocked），未运行。
    kairo::TaskLifecycleState state{};
    ASSERT_TRUE(lifecycle_state_of(executor, dependent.handle, state));
    EXPECT_EQ(state, kairo::TaskLifecycleState::DependencyBlocked);
    EXPECT_FALSE(ran.load(std::memory_order_acquire));

    // 只放行前 7 个：仍缺最后一个，dependent 继续驻留、不运行。
    for (std::size_t i = 0; i + 1 < kPrereqs; ++i) {
        gates[i].release();
    }
    for (std::size_t i = 0; i + 1 < kPrereqs; ++i) {
        ASSERT_TRUE(settles_within(upstreams[i].future));
        EXPECT_EQ(upstreams[i].future.get(), 0);
    }
    EXPECT_TRUE(lifecycle_state_of(executor, dependent.handle, state));
    EXPECT_EQ(state, kairo::TaskLifecycleState::DependencyBlocked);
    EXPECT_FALSE(ran.load(std::memory_order_acquire));

    // 放行最后一个：级联出队，dependent 执行并得到正确结果。
    gates.back().release();
    ASSERT_TRUE(settles_within(upstreams.back().future));
    EXPECT_EQ(upstreams.back().future.get(), 0);
    ASSERT_TRUE(settles_within(dependent.future));
    EXPECT_EQ(dependent.future.get(), 99);
    EXPECT_TRUE(ran.load(std::memory_order_acquire));

    executor.shutdown();
}

// 1b：深链（链长 16，worker=2）。submit_after 链全部按序完成。
TEST(DependencyDrivenSchedulingTest, DeepChainCompletesInOrderOnTwoWorkers) {
    kairo::Executor executor;
    ASSERT_TRUE(executor.initialize(pool_config(2)));

    constexpr int kChainLength = 16;
    std::atomic<int> sequence{0};
    std::vector<std::future<int>> futures;

    auto first = executor.submit_with_handle([&sequence] {
        EXPECT_EQ(sequence.fetch_add(1, std::memory_order_acq_rel), 0);
        return 1;
    });

    kairo::TaskHandle previous = first.handle;
    for (int value = 2; value <= kChainLength; ++value) {
        auto next = executor.submit_after_with_handle(
            previous, [&sequence, value] {
                EXPECT_EQ(sequence.fetch_add(1, std::memory_order_acq_rel),
                          value - 1);
                return value;
            });
        futures.push_back(std::move(next.future));
        previous = next.handle;
    }

    ASSERT_TRUE(settles_within(first.future));
    EXPECT_EQ(first.future.get(), 1);
    for (int value = 2; value <= kChainLength; ++value) {
        ASSERT_TRUE(settles_within(
            futures[static_cast<std::size_t>(value - 2)]));
        EXPECT_EQ(futures[static_cast<std::size_t>(value - 2)].get(), value);
    }
    EXPECT_EQ(sequence.load(std::memory_order_acquire), kChainLength);

    executor.shutdown();
}

// 1c：菱形 A → (B, C) → D。D 依赖 B、C 两者。
TEST(DependencyDrivenSchedulingTest, DiamondDependentWaitsForBothBranches) {
    kairo::Executor executor;
    ASSERT_TRUE(executor.initialize(pool_config(2)));

    Gate gate;
    auto a = gate.spawn_on(executor);
    ASSERT_TRUE(gate.entered_within());

    std::atomic<int> order{0};
    auto b = executor.submit_after_with_handle(a.handle, [&order] {
        // B 与 C 互为兄弟（都只依赖 A），二者之间无先后约束；
        // 只断言各自先于 D 执行（各贡献一次计数）。
        EXPECT_LT(order.fetch_add(1, std::memory_order_acq_rel), 2);
        return 2;
    });
    auto c = executor.submit_after_with_handle(a.handle, [&order] {
        EXPECT_LT(order.fetch_add(1, std::memory_order_acq_rel), 2);
        return 3;
    });
    auto d = executor.submit_after_with_handle(
        std::vector<kairo::TaskHandle>{b.handle, c.handle}, [&order] {
            EXPECT_EQ(order.load(std::memory_order_acquire), 2);
            return 4;
        });

    // A 仍被门阻塞：B、C、D 均未执行，D 处于 DependencyBlocked。
    kairo::TaskLifecycleState state{};
    ASSERT_TRUE(lifecycle_state_of(executor, d.handle, state));
    EXPECT_EQ(state, kairo::TaskLifecycleState::DependencyBlocked);

    gate.release();
    ASSERT_TRUE(settles_within(a.future));
    EXPECT_EQ(a.future.get(), 0);
    ASSERT_TRUE(settles_within(b.future));
    EXPECT_EQ(b.future.get(), 2);
    ASSERT_TRUE(settles_within(c.future));
    EXPECT_EQ(c.future.get(), 3);
    ASSERT_TRUE(settles_within(d.future));
    EXPECT_EQ(d.future.get(), 4);
    EXPECT_EQ(order.load(std::memory_order_acquire), 2);

    executor.shutdown();
}

// 2：免饿死核心回归。worker=2，P1/P2 门控占满 worker，X1/X2 排队，
// D1/D2 submit_after(X1/X2)。parked 实现下放行门后全部任务在时限内完成
// （旧实现 D1/D2 若占用 worker 等待 X1/X2 将挂死）。
TEST(DependencyDrivenSchedulingTest, ParkedDependentsDoNotStarveWorkerPool) {
    kairo::Executor executor;
    ASSERT_TRUE(executor.initialize(pool_config(2)));

    Gate blocker_a;
    Gate blocker_b;
    auto p1 = blocker_a.spawn_on(executor);
    auto p2 = blocker_b.spawn_on(executor);
    ASSERT_TRUE(blocker_a.entered_within());
    ASSERT_TRUE(blocker_b.entered_within());  // 两个 worker 均被占住

    std::atomic<int> plain_ran{0};
    std::atomic<int> dependent_ran{0};
    auto x1 = executor.submit_with_handle([&plain_ran] {
        plain_ran.fetch_add(1, std::memory_order_acq_rel);
        return 11;
    });
    auto x2 = executor.submit_with_handle([&plain_ran] {
        plain_ran.fetch_add(1, std::memory_order_acq_rel);
        return 12;
    });

    auto d1 = executor.submit_after_with_handle(x1.handle, [&dependent_ran] {
        dependent_ran.fetch_add(1, std::memory_order_acq_rel);
        return 21;
    });
    auto d2 = executor.submit_after_with_handle(x2.handle, [&dependent_ran] {
        dependent_ran.fetch_add(1, std::memory_order_acq_rel);
        return 22;
    });

    // 依赖未就绪：D1/D2 不入队（DependencyBlocked 驻留），不吃 worker。
    kairo::TaskLifecycleState state{};
    ASSERT_TRUE(lifecycle_state_of(executor, d1.handle, state));
    EXPECT_EQ(state, kairo::TaskLifecycleState::DependencyBlocked);
    ASSERT_TRUE(lifecycle_state_of(executor, d2.handle, state));
    EXPECT_EQ(state, kairo::TaskLifecycleState::DependencyBlocked);
    EXPECT_EQ(plain_ran.load(std::memory_order_acquire), 0);
    EXPECT_EQ(dependent_ran.load(std::memory_order_acquire), 0);

    blocker_a.release();
    blocker_b.release();

    // X1/X2 先完成，D1/D2 随后；全程有限等待内（旧实现此处挂死）。
    ASSERT_TRUE(settles_within(x1.future));
    EXPECT_EQ(x1.future.get(), 11);
    ASSERT_TRUE(settles_within(x2.future));
    EXPECT_EQ(x2.future.get(), 12);
    ASSERT_TRUE(settles_within(d1.future));
    EXPECT_EQ(d1.future.get(), 21);
    ASSERT_TRUE(settles_within(d2.future));
    EXPECT_EQ(d2.future.get(), 22);
    EXPECT_EQ(plain_ran.load(std::memory_order_acquire), 2);
    EXPECT_EQ(dependent_ran.load(std::memory_order_acquire), 2);

    executor.shutdown();
}

// 3：失败级联。A 抛异常 → B=after(A)、C=after(B) 的 future 有限等待内
// 就绪并带依赖失败语义；B、C 的 callable 确实未运行。
TEST(DependencyDrivenSchedulingTest,
     FailureCascadeSettlesDependentsWithoutRunningThem) {
    kairo::Executor executor;
    ASSERT_TRUE(executor.initialize(pool_config(2)));

    auto failing = executor.submit_with_handle([]() -> int {
        throw std::runtime_error("boom");
    });

    std::atomic<bool> ran_b{false};
    std::atomic<bool> ran_c{false};
    auto b = executor.submit_after_with_handle(failing.handle, [&ran_b] {
        ran_b.store(true, std::memory_order_release);
        return 1;
    });
    auto c = executor.submit_after_with_handle(b.handle, [&ran_c] {
        ran_c.store(true, std::memory_order_release);
        return 2;
    });

    ASSERT_TRUE(settles_within(failing.future));
    {
        bool runtime_failure = false;
        std::string message;
        try {
            (void)failing.future.get();
        } catch (const std::runtime_error& error) {
            runtime_failure = true;
            message = error.what();
        }
        EXPECT_TRUE(runtime_failure);
        EXPECT_EQ(message, "boom");
    }

    // B：有限等待内就绪，携带 A 的异常语义，callable 未运行。
    ASSERT_TRUE(settles_within(b.future));
    {
        bool runtime_failure = false;
        bool cancelled = false;
        std::string message;
        try {
            (void)b.future.get();
        } catch (const kairo::TaskCancelled& error) {
            cancelled = true;
            message = error.what();
        } catch (const std::runtime_error& error) {
            runtime_failure = true;
            message = error.what();
        }
        EXPECT_TRUE(runtime_failure);
        EXPECT_FALSE(cancelled);
        EXPECT_EQ(message, "boom");
    }
    EXPECT_FALSE(ran_b.load(std::memory_order_acquire));

    // C：级联失败语义（非取消类），callable 未运行。
    ASSERT_TRUE(settles_within(c.future));
    {
        bool runtime_failure = false;
        bool cancelled = false;
        try {
            (void)c.future.get();
        } catch (const kairo::TaskCancelled&) {
            cancelled = true;
        } catch (const std::runtime_error&) {
            runtime_failure = true;
        }
        EXPECT_TRUE(runtime_failure);
        EXPECT_FALSE(cancelled);
    }
    EXPECT_FALSE(ran_c.load(std::memory_order_acquire));

    // 注意：不在此处断言 get_failure_status().task_exception_count。
    // wrapper 的结算顺序是 promise->set_exception（future 就绪）先于
    // record_task_exception（failure 统计），future.get() 返回后立即读
    // 统计是与 worker 线程的固有竞态（首轮验证中 10 次出现 2 次 0 vs 1）。

    executor.shutdown();
}

// 4：提交时依赖已满足 → 与普通提交一致（立即入队执行、结果正确）。
TEST(DependencyDrivenSchedulingTest, SatisfiedDependenciesBehaveLikePlainSubmit) {
    kairo::Executor executor;
    ASSERT_TRUE(executor.initialize(pool_config(2)));

    auto a = executor.submit_with_handle([] { return 5; });
    auto b = executor.submit_with_handle([] { return 7; });
    ASSERT_TRUE(settles_within(a.future));
    EXPECT_EQ(a.future.get(), 5);
    ASSERT_TRUE(settles_within(b.future));
    EXPECT_EQ(b.future.get(), 7);

    auto multi = executor.submit_after(
        std::vector<kairo::TaskHandle>{a.handle, b.handle},
        [] { return 12; });
    auto single = executor.submit_after(a.handle, [] { return 6; });

    // 若依赖已满足仍被 parked，这两个 future 永远不会就绪 → 有限等待失败。
    ASSERT_TRUE(settles_within(multi));
    EXPECT_EQ(multi.get(), 12);
    ASSERT_TRUE(settles_within(single));
    EXPECT_EQ(single.get(), 6);

    executor.shutdown();
}

// 5a：when_all 作为上游。when_all({A,B}) 未收敛前 dependent 驻留，
// 收敛后执行。
TEST(DependencyDrivenSchedulingTest, WhenAllUpstreamGatesDependentUntilResolved) {
    kairo::Executor executor;
    ASSERT_TRUE(executor.initialize(pool_config(4)));

    Gate gate;
    auto a = gate.spawn_on(executor);
    ASSERT_TRUE(gate.entered_within());

    auto b = executor.submit_with_handle([] { return 2; });
    ASSERT_TRUE(settles_within(b.future));
    EXPECT_EQ(b.future.get(), 2);

    const auto all = executor.when_all({a.handle, b.handle});
    std::atomic<bool> ran{false};
    auto d = executor.submit_after_with_handle(all, [&ran] {
        ran.store(true, std::memory_order_release);
        return 30;
    });

    // A 仍被门阻塞：when_all 未收敛，D 驻留。
    kairo::TaskLifecycleState state{};
    ASSERT_TRUE(lifecycle_state_of(executor, d.handle, state));
    EXPECT_EQ(state, kairo::TaskLifecycleState::DependencyBlocked);
    EXPECT_FALSE(ran.load(std::memory_order_acquire));

    gate.release();
    ASSERT_TRUE(settles_within(a.future));
    EXPECT_EQ(a.future.get(), 0);
    ASSERT_TRUE(settles_within(d.future));
    EXPECT_EQ(d.future.get(), 30);
    EXPECT_TRUE(ran.load(std::memory_order_acquire));

    executor.shutdown();
}

// 5b：submit_after 结果作为 when_all 输入的组合。A → X=after(A)，
// W=when_all({X, C})，Y=after(W)，全部正确收敛。
TEST(DependencyDrivenSchedulingTest, SubmitAfterResultComposesWithWhenAll) {
    kairo::Executor executor;
    ASSERT_TRUE(executor.initialize(pool_config(4)));

    Gate gate;
    auto a = gate.spawn_on(executor);
    ASSERT_TRUE(gate.entered_within());

    auto x = executor.submit_after_with_handle(a.handle, [] { return 10; });
    auto c = executor.submit_with_handle([] { return 3; });
    ASSERT_TRUE(settles_within(c.future));
    EXPECT_EQ(c.future.get(), 3);

    const auto all = executor.when_all({x.handle, c.handle});
    auto y = executor.submit_after_with_handle(all, [] { return 13; });

    // X 仍被 A 门控驻留：W 未收敛，Y 驻留。
    kairo::TaskLifecycleState state{};
    ASSERT_TRUE(lifecycle_state_of(executor, y.handle, state));
    EXPECT_EQ(state, kairo::TaskLifecycleState::DependencyBlocked);

    gate.release();
    ASSERT_TRUE(settles_within(a.future));
    EXPECT_EQ(a.future.get(), 0);
    ASSERT_TRUE(settles_within(x.future));
    EXPECT_EQ(x.future.get(), 10);
    ASSERT_TRUE(settles_within(y.future));
    EXPECT_EQ(y.future.get(), 13);

    executor.shutdown();
}

// 6：parked 取消。依赖未满足时 request_task_cancel → future 有限等待内
// 以 TaskCancelled 就绪；依赖随后完成时不重复结算、callable 不运行。
TEST(DependencyDrivenSchedulingTest,
     ParkedDependentCancelSettlesOnceWithoutRunning) {
    kairo::Executor executor;
    ASSERT_TRUE(executor.initialize(pool_config(2)));

    Gate gate;
    auto upstream = gate.spawn_on(executor);
    ASSERT_TRUE(gate.entered_within());

    std::atomic<bool> ran{false};
    auto dependent = executor.submit_after_with_handle(
        upstream.handle, [&ran] {
            ran.store(true, std::memory_order_release);
            return 2;
        });

    kairo::TaskLifecycleState state{};
    ASSERT_TRUE(lifecycle_state_of(executor, dependent.handle, state));
    EXPECT_EQ(state, kairo::TaskLifecycleState::DependencyBlocked);

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
        }
        EXPECT_TRUE(cancelled);
        EXPECT_EQ(reason, kairo::TaskCancellationReason::Explicit);
    }
    EXPECT_FALSE(ran.load(std::memory_order_acquire));

    // 依赖随后完成：不得重复结算（重复结算会使 promise 二次满足），
    // 也不得因级联出队而运行 callable。
    gate.release();
    ASSERT_TRUE(settles_within(upstream.future));
    EXPECT_EQ(upstream.future.get(), 0);
    EXPECT_FALSE(ran.load(std::memory_order_acquire));

    const auto cancellation = executor.get_cancellation_status();
    EXPECT_GE(cancellation.queued_cancelled_count, 1U);

    executor.shutdown();
}

// 7：parked dependent 占 admission 名额（D3 语义）。max_in_flight_tasks=4：
// 门控上游（1）+ 3 个 parked dependent 占满，第 4 个 dependent 提交被
// CapacityExhausted 拒绝；门放行排空后容量恢复。
TEST(DependencyDrivenSchedulingTest,
     ParkedDependentsConsumeAdmissionSlotsAndRejectOverflow) {
    kairo::Executor executor;
    auto config = pool_config(1);
    config.max_in_flight_tasks = 4;
    ASSERT_TRUE(executor.initialize(config));

    Gate gate;
    auto upstream = gate.spawn_on(executor);  // 占 1 个名额 + 唯一 worker
    ASSERT_TRUE(gate.entered_within());

    std::vector<kairo::TaskSubmission<int>> parked;
    for (int i = 0; i < 3; ++i) {
        parked.push_back(executor.submit_after_with_handle(
            upstream.handle, [i] { return 100 + i; }));
    }
    EXPECT_EQ(executor.get_in_flight_submissions(), 4U);

    // 第 4 个 dependent：容量耗尽，future 立即（有限等待内）以
    // CapacityExhaustedException 就绪。
    auto rejected = executor.submit_after_with_handle(upstream.handle,
                                                      [] { return 7; });
    ASSERT_TRUE(settles_within(rejected.future, kPromptLimit));
    {
        bool capacity_exhausted = false;
        try {
            (void)rejected.future.get();
        } catch (const kairo::CapacityExhaustedException&) {
            capacity_exhausted = true;
        }
        EXPECT_TRUE(capacity_exhausted);
    }
    EXPECT_EQ(executor.get_in_flight_submissions(), 4U);
    EXPECT_GE(executor.get_failure_status().capacity_exhausted_count, 1U);

    // 放行：上游与 parked dependents 全部完成，计数归零，容量恢复。
    gate.release();
    ASSERT_TRUE(settles_within(upstream.future));
    EXPECT_EQ(upstream.future.get(), 0);
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(settles_within(
            parked[static_cast<std::size_t>(i)].future));
        EXPECT_EQ(parked[static_cast<std::size_t>(i)].future.get(), 100 + i);
    }
    auto recovery = executor.submit([] { return 1; });
    ASSERT_TRUE(settles_within(recovery));
    EXPECT_EQ(recovery.get(), 1);
    EXPECT_EQ(executor.get_in_flight_submissions(), 0U);

    executor.shutdown();
}

// 8：关停后依赖才完成的 parked dependent——出队被拒须按既有提交拒绝
// 语义结算 future，不得让 future 永久悬空（drain 被拒分支的回归）。
//
// 关停后立即放行依赖并析构 facade，是对"retired 保活 + 终局排空链"的
// 直接回归：deferred drain 在上游 future 就绪之后才写 facade 状态
// （record_submit_rejected / monitor），若孤儿池 worker 未被 join，
// 此处裸跑（无任何收尾缓解）即以高概率触发 UAF 段错误。
TEST(DependencyDrivenSchedulingTest,
     DependencyCompletedAfterShutdownSettlesParkedAsRejected) {
    kairo::Executor executor;
    ASSERT_TRUE(executor.initialize(pool_config(2)));

    Gate gate;
    auto upstream = gate.spawn_on(executor);
    ASSERT_TRUE(gate.entered_within());

    auto dependent =
        executor.submit_after_with_handle(upstream.handle, [] { return 2; });

    kairo::TaskLifecycleState state{};
    ASSERT_TRUE(lifecycle_state_of(executor, dependent.handle, state));
    EXPECT_EQ(state, kairo::TaskLifecycleState::DependencyBlocked);

    executor.shutdown(/*wait_for_tasks=*/false);

    gate.release();
    ASSERT_TRUE(settles_within(upstream.future));
    EXPECT_EQ(upstream.future.get(), 0);

    // parked 出队被拒：future 有限等待内以拒绝语义异常就绪。
    ASSERT_TRUE(settles_within(dependent.future));
    {
        bool settled_with_exception = false;
        try {
            (void)dependent.future.get();
            // 无值返回类型为 int；正常完成不应发生（执行器已停止）。
        } catch (const std::exception&) {
            settled_with_exception = true;
        }
        EXPECT_TRUE(settled_with_exception);
    }

    // 无任何收尾等待，直接析构：retired 链必须保证孤儿池 worker（含其
    // drain 尾）已全部 join。
    executor.shutdown();
}
}  // namespace
