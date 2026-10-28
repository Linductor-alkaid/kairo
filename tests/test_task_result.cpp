// 0.7.3 §5.0 TaskResult<T> / Executor::submit_v2 契约测试（独立验证代理编写，
// 不修改库代码）。
//
// 契约矩阵（docs/design/scheduling_runtime.md §16）：
//  V1. 值返回：int / std::string / move-only（unique_ptr 产物）经 get() 取出；
//      void 特化 get() 正常返回且完成后 valid() == false。
//  V2. 句柄转移：移动构造/移动赋值后源句柄 invalid、目标句柄可取值。
//  V3. 无效句柄：默认构造与 get() 之后的句柄，get/wait/wait_for/wait_until
//      抛 future_error(no_state)；is_ready() == false。
//  V4. 异常传播：任务异常经 get() 重抛且保留原异常类型（含自定义类型）。
//  V5. 时序：wait_for/wait_until 未就绪返回 timeout，就绪返回 ready。
//  V6. 快速路径命中率：预热后 2000 次 submit_v2 + 立即 get，差分
//      fast/(fast+slow) > 60%（roadmap 验收线）。
//  V7. fast/slow 计数差分：慢任务立即 get 计 slow；is_ready 轮询后 get 计 fast。
//  V8. admission：set_max_in_flight_tasks(1) 后第二次 submit_v2 不抛出、
//      get() 得 CapacityExhaustedException；容量释放后可再提交；max 恢复 0。
//  V9. 空 std::function 拒绝：get() 抛 invalid_argument（void 与值返回形态）。
//  V10. queued soft timeout：排队超时经 get() 抛 TimedOutException，失败事件
//       task_id == "submit_v2"，timeout_count 递增。
//  V11. broken promise：shutdown(false) 丢弃排队任务时 get() 有界返回——
//       任务正常完成值或 future_error(broken_promise) 之一，不允许挂死。
//  V12. 并发：4 线程 x 500 次 submit_v2/get 混合，无死锁、结果正确
//       （配合 TSAN 运行）。

#include <gtest/gtest.h>

#include <kairo/executor.hpp>
#include <kairo/task_result.hpp>
#include <kairo/types.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace kairo;

namespace {

struct CustomError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// 5s 有界等待辅助：断言 future 在时限内就绪（防挂死护栏）。
template<typename T>
bool settles_within(const std::future<T>& future, std::chrono::milliseconds budget) {
    return future.wait_for(budget) == std::future_status::ready;
}

}  // namespace

class TaskResultTest : public ::testing::Test {
protected:
    void SetUp() override {
        ExecutorConfig config;
        config.min_threads = 2;
        config.max_threads = 4;
        ASSERT_TRUE(executor_.initialize(config));
    }

    void TearDown() override { executor_.shutdown(); }

    Executor executor_;
};

// ---------------------------------------------------------------------------
// V1. 值返回与 void 特化
// ---------------------------------------------------------------------------

TEST_F(TaskResultTest, SubmitV2ReturnsValue) {
    auto int_result = executor_.submit_v2([] { return 41 + 1; });
    ASSERT_TRUE(int_result.valid());
    EXPECT_TRUE(int_result.is_ready() || !int_result.is_ready());  // 无前置约束
    EXPECT_EQ(int_result.get(), 42);
    EXPECT_FALSE(int_result.valid());

    auto string_result = executor_.submit_v2(
        [] { return std::string("task-result"); });
    EXPECT_EQ(string_result.get(), "task-result");
}

TEST_F(TaskResultTest, SubmitV2ReturnsMoveOnlyValue) {
    auto result = executor_.submit_v2(
        [] { return std::make_unique<int>(7); });
    ASSERT_TRUE(result.valid());
    std::unique_ptr<int> value = result.get();
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(*value, 7);
    EXPECT_FALSE(result.valid());
}

TEST_F(TaskResultTest, SubmitV2VoidSpecialization) {
    std::atomic<bool> ran{false};
    auto result = executor_.submit_v2([&ran] { ran.store(true); });
    ASSERT_TRUE(result.valid());
    EXPECT_NO_THROW(result.get());
    EXPECT_TRUE(ran.load());
    EXPECT_FALSE(result.valid());
}

TEST_F(TaskResultTest, SubmitV2BindsRuntimeArgs) {
    auto result = executor_.submit_v2(
        [](int lhs, int rhs) { return lhs * rhs; }, 6, 7);
    EXPECT_EQ(result.get(), 42);
}

// ---------------------------------------------------------------------------
// V2. 句柄移动语义
// ---------------------------------------------------------------------------

TEST_F(TaskResultTest, MoveTransfersValidity) {
    auto source = executor_.submit_v2([] { return 11; });
    TaskResult<int> moved = std::move(source);
    EXPECT_FALSE(source.valid());
    ASSERT_TRUE(moved.valid());
    EXPECT_EQ(moved.get(), 11);

    auto other = executor_.submit_v2([] { return 22; });
    TaskResult<int> target;
    EXPECT_FALSE(target.valid());
    target = std::move(other);
    EXPECT_FALSE(other.valid());
    ASSERT_TRUE(target.valid());
    EXPECT_EQ(target.get(), 22);
}

// ---------------------------------------------------------------------------
// V3. 无效句柄契约
// ---------------------------------------------------------------------------

TEST_F(TaskResultTest, InvalidHandleThrowsNoState) {
    TaskResult<int> invalid;
    EXPECT_FALSE(invalid.valid());
    EXPECT_FALSE(invalid.is_ready());
    EXPECT_THROW(invalid.get(), std::future_error);
    EXPECT_THROW(invalid.wait(), std::future_error);
    EXPECT_THROW(invalid.wait_for(std::chrono::milliseconds(1)),
                 std::future_error);
    EXPECT_THROW(invalid.wait_until(std::chrono::steady_clock::now()),
                 std::future_error);
    try {
        (void)invalid.get();
    } catch (const std::future_error& error) {
        EXPECT_EQ(error.code(), std::future_errc::no_state);
    }

    TaskResult<void> invalid_void;
    EXPECT_FALSE(invalid_void.valid());
    EXPECT_THROW(invalid_void.get(), std::future_error);
    EXPECT_THROW(invalid_void.wait(), std::future_error);
}

TEST_F(TaskResultTest, GetInvalidatesHandleAndRepeatGetThrowsNoState) {
    auto result = executor_.submit_v2([] { return 5; });
    EXPECT_EQ(result.get(), 5);
    EXPECT_FALSE(result.valid());
    EXPECT_FALSE(result.is_ready());
    EXPECT_THROW(result.get(), std::future_error);
    try {
        (void)result.get();
    } catch (const std::future_error& error) {
        EXPECT_EQ(error.code(), std::future_errc::no_state);
    }
}

// ---------------------------------------------------------------------------
// V4. 异常传播保真
// ---------------------------------------------------------------------------

TEST_F(TaskResultTest, ExceptionPropagationPreservesOriginalType) {
    auto std_result = executor_.submit_v2(
        []() -> int { throw std::runtime_error("boom-42"); });
    try {
        (void)std_result.get();
        FAIL() << "expected std::runtime_error";
    } catch (const std::runtime_error& error) {
        EXPECT_STREQ(error.what(), "boom-42");
    }

    auto custom_result = executor_.submit_v2(
        []() -> int { throw CustomError("custom-payload"); });
    try {
        (void)custom_result.get();
        FAIL() << "expected CustomError";
    } catch (const CustomError& error) {
        EXPECT_STREQ(error.what(), "custom-payload");
    } catch (...) {
        FAIL() << "original exception type was not preserved";
    }
}

// ---------------------------------------------------------------------------
// V5. wait_for / wait_until 时序
// ---------------------------------------------------------------------------

TEST_F(TaskResultTest, WaitForTimesOutThenBecomesReady) {
    std::promise<void> gate;
    std::shared_future<void> gate_future = gate.get_future().share();
    auto result = executor_.submit_v2([gate_future] {
        gate_future.wait();
        return 99;
    });

    const auto started = std::chrono::steady_clock::now();
    EXPECT_EQ(result.wait_for(std::chrono::milliseconds(20)),
              std::future_status::timeout);
    EXPECT_GE(std::chrono::steady_clock::now() - started,
              std::chrono::milliseconds(20));
    EXPECT_FALSE(result.is_ready());
    EXPECT_TRUE(result.valid());

    gate.set_value();
    EXPECT_EQ(result.wait_for(std::chrono::seconds(2)),
              std::future_status::ready);
    EXPECT_TRUE(result.is_ready());
    EXPECT_EQ(result.get(), 99);
}

TEST_F(TaskResultTest, WaitUntilDeadlineSemantics) {
    std::promise<void> gate;
    std::shared_future<void> gate_future = gate.get_future().share();
    auto result = executor_.submit_v2([gate_future] {
        gate_future.wait();
        return 7;
    });

    // 过去时刻的 deadline：未就绪 → timeout（无挂死）。
    EXPECT_EQ(result.wait_until(std::chrono::steady_clock::now() -
                                std::chrono::milliseconds(1)),
              std::future_status::timeout);

    gate.set_value();
    EXPECT_EQ(result.wait_until(std::chrono::steady_clock::now() +
                                std::chrono::seconds(2)),
              std::future_status::ready);
    EXPECT_EQ(result.get(), 7);
}

TEST_F(TaskResultTest, WaitBlocksUntilReady) {
    std::atomic<bool> done{false};
    auto result = executor_.submit_v2([&done] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        done.store(true);
    });
    result.wait();
    EXPECT_TRUE(done.load());
    EXPECT_TRUE(result.is_ready());
}

// ---------------------------------------------------------------------------
// V6. 快速路径命中率（roadmap 验收线 > 60%）
// ---------------------------------------------------------------------------

TEST_F(TaskResultTest, FastPathHitRateAboveAcceptanceLine) {
    // 预热：worker 起来、进入稳态。
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(executor_.submit_v2([] { return 1; }).get(), 1);
    }

    const AllocationMetrics before = executor_.get_allocation_metrics();
    constexpr int kIterations = 2000;
    for (int i = 0; i < kIterations; ++i) {
        auto result = executor_.submit_v2([i] { return i; });
        (void)result.get();
    }
    const AllocationMetrics after = executor_.get_allocation_metrics();

    const uint64_t fast = after.fast_path_get_count - before.fast_path_get_count;
    const uint64_t slow = after.slow_path_get_count - before.slow_path_get_count;
    EXPECT_EQ(fast + slow, static_cast<uint64_t>(kIterations))
        << "every measured get() must be accounted in fast/slow";
    const double hit_rate =
        static_cast<double>(fast) / static_cast<double>(fast + slow);
    EXPECT_GT(hit_rate, 0.60)
        << "fast=" << fast << " slow=" << slow
        << " hit_rate=" << hit_rate << " (acceptance line: > 60%)";
}

// ---------------------------------------------------------------------------
// V7. fast/slow 计数差分
// ---------------------------------------------------------------------------

TEST_F(TaskResultTest, FastSlowCountersDifferential) {
    AllocationMetrics before = executor_.get_allocation_metrics();

    // slow：任务睡眠远超 100 次 yield 的 spin 窗口，立即 get 必落慢速路径。
    auto slow_result = executor_.submit_v2([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        return 1;
    });
    EXPECT_EQ(slow_result.get(), 1);

    // fast：is_ready() 轮询不计数；就绪后 get() 首查命中。
    auto fast_result = executor_.submit_v2([] { return 2; });
    while (!fast_result.is_ready()) {
        std::this_thread::yield();
    }
    EXPECT_EQ(fast_result.get(), 2);

    AllocationMetrics after = executor_.get_allocation_metrics();
    EXPECT_EQ(after.slow_path_get_count - before.slow_path_get_count, 1u);
    EXPECT_EQ(after.fast_path_get_count - before.fast_path_get_count, 1u);
}

// ---------------------------------------------------------------------------
// V8. admission（总量有界）
// ---------------------------------------------------------------------------

TEST_F(TaskResultTest, AdmissionRejectionIsExceptionReadyNotThrown) {
    executor_.set_max_in_flight_tasks(1);
    ASSERT_EQ(executor_.get_max_in_flight_tasks(), 1u);

    std::promise<void> release;
    std::shared_future<void> release_future = release.get_future().share();
    auto blocker = executor_.submit([release_future] { release_future.wait(); });
    // 容量 1 被 blocker 占用后第二次提交必须被拒——但 submit_v2 本身
    // 不抛出，拒绝以 TaskResult 异常就绪（异常就绪不抛出契约）。
    ASSERT_EQ(executor_.get_in_flight_submissions(), 1u);
    auto rejected = executor_.submit_v2([] { return 1; });
    EXPECT_EQ(executor_.get_in_flight_submissions(), 1u)
        << "rejected submissions must not consume admission capacity";
    EXPECT_THROW(rejected.get(), CapacityExhaustedException);
    EXPECT_EQ(executor_.get_failure_status().capacity_exhausted_count, 1u);

    // 释放容量后可再提交，且计数归还。
    release.set_value();
    blocker.get();
    auto accepted = executor_.submit_v2([] { return 3; });
    EXPECT_EQ(accepted.get(), 3);
    EXPECT_EQ(executor_.get_in_flight_submissions(), 0u);

    // max 恢复 0 清理。
    executor_.set_max_in_flight_tasks(0);
    EXPECT_EQ(executor_.get_max_in_flight_tasks(), 0u);
    EXPECT_EQ(executor_.get_in_flight_submissions(), 0u);
}

// ---------------------------------------------------------------------------
// V9. 空 std::function 拒绝
// ---------------------------------------------------------------------------

TEST_F(TaskResultTest, EmptyStdFunctionRejectedAsInvalidArgument) {
    std::function<int()> empty_int;
    auto rejected_int = executor_.submit_v2(std::move(empty_int));
    ASSERT_TRUE(rejected_int.valid());
    EXPECT_TRUE(rejected_int.is_ready())
        << "empty-task rejection settles before return";
    try {
        (void)rejected_int.get();
        FAIL() << "expected std::invalid_argument";
    } catch (const std::invalid_argument& error) {
        EXPECT_STREQ(error.what(), "empty task");
    }
    EXPECT_EQ(executor_.get_failure_status().submit_rejected_count >= 1, true);

    // void 形态（std::function<void()> 空目标）的拒绝同样是"异常就绪"，
    // get() 重抛 invalid_argument（retrieve_value noexcept 缺陷已修复，
    // commit 455e389；修复前此路径会 terminate）。
    std::function<void()> empty_void;
    auto rejected_void = executor_.submit_v2(std::move(empty_void));
    ASSERT_TRUE(rejected_void.valid());
    EXPECT_THROW(rejected_void.get(), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// V10. queued soft timeout（task_id == "submit_v2" 观测）
// ---------------------------------------------------------------------------

TEST_F(TaskResultTest, QueuedSoftTimeoutSettlesTimedOutAndRecordsTaskId) {
    Executor single;
    ExecutorConfig config;
    config.min_threads = 1;
    config.max_threads = 1;
    config.task_timeout_ms = 60;
    ASSERT_TRUE(single.initialize(config));

    // blocker 占住唯一 worker；v2 任务排队至超时线后被跳过执行。
    auto blocker = single.submit([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    });
    auto timed_out = single.submit_v2([]() -> int {
        ADD_FAILURE() << "timed-out task must be skipped, not executed";
        return 0;
    });

    ASSERT_EQ(timed_out.wait_for(std::chrono::seconds(3)),
              std::future_status::ready);
    try {
        (void)timed_out.get();
        FAIL() << "expected TimedOutException";
    } catch (const TimedOutException&) {
    } catch (const std::runtime_error& error) {
        FAIL() << "unexpected exception type: " << error.what();
    }
    blocker.wait();

    const ExecutorFailureStatus status = single.get_failure_status();
    EXPECT_GE(status.timeout_count, 1u);
    bool saw_submit_v2_timeout = false;
    for (const auto& event : single.get_recent_failures()) {
        if (event.kind == FailureKind::TaskTimeout &&
            event.task_id == "submit_v2") {
            saw_submit_v2_timeout = true;
        }
    }
    EXPECT_TRUE(saw_submit_v2_timeout)
        << "queued soft timeout must record task_id \"submit_v2\"";
    single.shutdown();
}

// ---------------------------------------------------------------------------
// V11. broken promise：shutdown(false) 丢弃排队任务
// ---------------------------------------------------------------------------

TEST_F(TaskResultTest, BrokenPromiseBoundedOnShutdownWithoutDrain) {
    Executor single;
    ExecutorConfig config;
    config.min_threads = 1;
    config.max_threads = 1;
    ASSERT_TRUE(single.initialize(config));

    // blocker 占住唯一 worker，保证 v2 任务入队而非执行；随后
    // shutdown(false) 丢弃队列 → 包装 functor 销毁 → broken_promise。
    // 与此同时任务也可能已被执行完成——契约允许两种终态，唯一不允许
    // 的是 get() 永久挂起。
    auto blocker = single.submit([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    });
    auto result = single.submit_v2([] { return 1234; });
    single.shutdown(false);

    enum class Outcome { kNone, kValue, kBrokenPromise, kOther };
    std::promise<Outcome> outcome_promise;
    std::future<Outcome> outcome = outcome_promise.get_future();
    std::thread consumer([&result, &outcome_promise] {
        try {
            const int value = result.get();
            outcome_promise.set_value(value == 1234 ? Outcome::kValue
                                                    : Outcome::kOther);
        } catch (const std::future_error& error) {
            if (error.code() == std::future_errc::broken_promise) {
                outcome_promise.set_value(Outcome::kBrokenPromise);
            } else {
                outcome_promise.set_value(Outcome::kOther);
            }
        } catch (...) {
            outcome_promise.set_value(Outcome::kOther);
        }
    });

    ASSERT_TRUE(settles_within(outcome, std::chrono::seconds(5)))
        << "get() must be bounded after shutdown(false): observed neither the "
           "task value nor future_error(broken_promise) within 5s";
    const Outcome observed = outcome.get();
    EXPECT_TRUE(observed == Outcome::kValue ||
                observed == Outcome::kBrokenPromise)
        << "observed outcome must be task value or broken_promise, got "
        << static_cast<int>(observed);
    consumer.join();
    blocker.wait();
}

// ---------------------------------------------------------------------------
// V12. 并发混合 submit_v2/get
// ---------------------------------------------------------------------------

TEST_F(TaskResultTest, ConcurrentSubmitAndGetMixed) {
    constexpr int kThreads = 4;
    constexpr int kIterations = 500;
    std::atomic<int64_t> total{0};
    std::vector<std::thread> workers;
    std::atomic<bool> stop_flag{false};
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([this, t, &total, &stop_flag] {
            int64_t local = 0;
            for (int i = 0; i < kIterations && !stop_flag.load(); ++i) {
                if ((i & 1) == 0) {
                    auto result = this->executor_.submit_v2(
                        [t, i] { return t * kIterations + i; });
                    local += result.get();
                } else {
                    // void/异常/立即就绪形态混合，覆盖不同结算路径。
                    switch (i % 6) {
                    case 1: {
                        auto r = this->executor_.submit_v2([] {});
                        r.get();
                        break;
                    }
                    case 3: {
                        auto r = this->executor_.submit_v2(
                            []() -> int { throw std::runtime_error("mix"); });
                        try {
                            (void)r.get();
                        } catch (const std::runtime_error&) {
                            // 预期路径。
                        }
                        break;
                    }
                    default: {
                        auto r = this->executor_.submit_v2(
                            [] { return 1; });
                        local += r.get();
                        break;
                    }
                    }
                }
            }
            total.fetch_add(local, std::memory_order_relaxed);
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    // 偶数批次贡献：sum(t*kN + i) over t in [0,4), i even in [0,500)。
    int64_t expected_even = 0;
    for (int t = 0; t < kThreads; ++t) {
        for (int i = 0; i < kIterations; i += 2) {
            expected_even += t * kIterations + i;
        }
    }
    // default 分支（i % 6 ∈ {5} 且 i 奇数）每次额外 +1；每个 worker
    // 线程各跑一遍完整循环，计数按线程数放大。
    int64_t expected_default = 0;
    for (int i = 0; i < kIterations; ++i) {
        if ((i % 2) != 0 && (i % 6) != 1 && (i % 6) != 3) {
            ++expected_default;
        }
    }
    expected_default *= kThreads;
    EXPECT_EQ(total.load(), expected_even + expected_default);
    EXPECT_EQ(executor_.get_in_flight_submissions(), 0u);
}

// ---------------------------------------------------------------------------
// 已发现实现缺陷（独立验证代理，2026-10-28）：void 任务的异常经
// TaskResult<void>::get() 重抛时 std::terminate。
//
// 根因：include/kairo/task_result.hpp void 特化的
// TaskResultSharedState<void>::retrieve_value() 被声明为 noexcept（非
// void 特化无 noexcept），其函数体内的 std::rethrow_exception 在
// noexcept 函数中触发 std::terminate。submit()（std::promise 路径）
// 对 void 任务异常正常重抛，submit_v2 的"与 submit 异常语义同构"契约
// 在 void 形态被破坏。
// 最小复现：
//   Executor ex; /* initialize */
//   auto r = ex.submit_v2([] { throw std::logic_error("boom"); });
//   try { r.get(); } catch (...) {}   // <- terminate，而非进入 catch
// 建议修复：移除 void 特化 retrieve_value() 的 noexcept 限定。
// 该用例必须保持本文件最后一个：缺陷触发会 abort 整个测试进程，
// 遮蔽其余用例的执行结果。
// ---------------------------------------------------------------------------

TEST_F(TaskResultTest, VoidTaskExceptionRethrowsInsteadOfTerminate) {
    auto void_result = executor_.submit_v2(
        [] { throw std::logic_error("void-boom"); });
    EXPECT_THROW(void_result.get(), std::logic_error);
}
