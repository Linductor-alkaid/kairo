// 0.7.2 §4.2 聚合级执行反馈契约测试（独立验证代理编写，不修改库代码）。
//
// 覆盖：
//  A1. LockFreeTaskExecutor：默认无预算（over_budget 恒 0）/ 预算两侧界限
//      （1ns 全超、1s 全不超）/ 运行期改预算生效 / 负预算按 0 / 异常任务
//      仍计入 executed_count / processed_count 与 executed_count 一致。
//  A2. RealtimeThreadExecutor（经 facade 注册）：push N 任务后
//      executed_count >= N；over_budget/period 与 get_status() 的
//      cycle_timeout_count/cycle_count 同源一致（停机后冻结比对）。
//  A3. Facade：未知名零值；按名解析 lockfree/realtime；
//      get_all_aggregate_execution_stats() 覆盖两类全部注册执行器。
//  A4. 接口默认实现：IRealtimeExecutor 默认零值；IGpuExecutor 默认
//      supports_completion_timing()==false 且 submit_kernel_with_timing
//      照常提交、不调用 sink。
//
// 确定性说明：不使用任何"时延必须小于 X"的断言。预算两侧分别取 1ns
// （任何任务执行必然严格大于 1ns——两次时钟读数本身远超 1ns）与 1s
// （空任务必然远小于 1s），从而在不依赖精确时延的前提下覆盖
// "严格大于才计数"（duration > budget）的两侧行为。"恰等于预算"的
// 精确边界在墙钟口径下不可稳定构造（见报告中的残差风险说明）。

#include <gtest/gtest.h>

#include <kairo/executor.hpp>
#include <kairo/lockfree_task_executor.hpp>
#include <kairo/scheduler.hpp>
#include <kairo/types.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

using namespace kairo;

namespace {

// 平台时钟粒度鲁棒的 µs 级工作负载（volatile 防优化剔除）。MSVC/QPC
// 的 steady_clock 粒度 ~100ns，空任务的两次采样可能实测 0ns——"1ns
// 预算必计超"的断言在粗粒度时钟上不成立（Linux CI 首轮未暴露）。
// ~µs 级自旋保证任何平台的实测时长都远大于 1ns 且远小于 1s。
void burn_microseconds() {
    volatile int sink = 0;
    for (int i = 0; i < 2000; ++i) {
        sink = sink + i;
    }
    static_cast<void>(sink);
}


constexpr auto kPollTimeout = std::chrono::milliseconds(2000);

template <typename Predicate>
bool wait_until(Predicate predicate,
                std::chrono::milliseconds timeout = kPollTimeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

// 初始化带指定线程数的默认池（遵循 tests/test_scheduling_runtime.cpp 约定）。
void init_executor(Executor& executor, size_t threads) {
    ExecutorConfig config;
    config.min_threads = threads;
    config.max_threads = threads;
    ASSERT_TRUE(executor.initialize(config));
}

RealtimeThreadConfig make_rt_config(const std::string& thread_name,
                                    int64_t period_ns = 2'000'000) {
    RealtimeThreadConfig config;
    config.thread_name = thread_name;
    config.cycle_period_ns = period_ns;
    config.thread_priority = 0;
    config.enable_process_memory_lock = false;
    config.timer_slack_ns = 0;
    config.cycle_callback = []() {};
    return config;
}

// 推送 N 个计数任务并等待其全部执行完成。
void push_and_wait(Executor& executor, const std::string& rt_name,
                   std::atomic<int>& counter, int n) {
    for (int i = 0; i < n; ++i) {
        ASSERT_TRUE(executor.push_realtime_task(
            rt_name, [&counter] { counter.fetch_add(1); }));
    }
    ASSERT_TRUE(wait_until([&counter, n] {
        return counter.load() >= n;
    }));
}

// 最小 IRealtimeExecutor：不 override get_aggregate_execution_stats，
// 用于验证接口默认实现返回零值。
class MinimalRealtimeExecutor final : public IRealtimeExecutor {
public:
    bool start() override { return true; }
    void stop() override {}
    ExecutorResult push_task(std::function<void()> task) override {
        (void)task;  // 测试桩：不真正执行
        return ExecutorResult::success();
    }
    std::string get_name() const override { return "minimal_rt"; }
    RealtimeExecutorStatus get_status() const override { return {}; }
};

// 最小 IGpuExecutor：不 override supports_completion_timing /
// submit_kernel_with_timing，用于验证接口默认实现（照常提交、不调 sink）。
class MinimalGpuExecutor final : public IGpuExecutor {
public:
    explicit MinimalGpuExecutor(std::string name) : name_(std::move(name)) {}

    std::string get_name() const override { return name_; }

    gpu::GpuDeviceInfo get_device_info() const override {
        gpu::GpuDeviceInfo info;
        info.name = name_ + "_device";
        info.backend = gpu::GpuBackend::CUDA;
        info.device_id = 0;
        return info;
    }

    gpu::GpuExecutorStatus get_status() const override {
        gpu::GpuExecutorStatus status;
        status.name = name_;
        status.is_running = true;
        status.backend = gpu::GpuBackend::CUDA;
        return status;
    }

    bool start() override { return true; }
    void stop() override {}
    void wait_for_completion() override {}
    void* allocate_device_memory(size_t) override { return nullptr; }
    void free_device_memory(void*) override {}
    bool copy_to_device(void*, const void*, size_t, bool, int) override {
        return true;
    }
    bool copy_to_host(void*, const void*, size_t, bool, int) override {
        return true;
    }
    bool copy_device_to_device(void*, const void*, size_t, bool, int) override {
        return true;
    }
    void synchronize() override {}
    void synchronize_stream(int) override {}
    int create_stream() override { return 1; }
    void destroy_stream(int) override {}
    bool add_stream_callback(int, std::function<void()>) override {
        return true;
    }

    std::future<void> submit_kernel_impl(
        std::function<void(void*)> kernel_func,
        const gpu::GpuTaskConfig& config) override {
        (void)config;
        std::promise<void> promise;
        auto future = promise.get_future();
        if (kernel_func) {
            ++submit_kernel_impl_calls_;
            kernel_func(nullptr);
        }
        promise.set_value();
        return future;
    }

    int submit_kernel_impl_calls() const { return submit_kernel_impl_calls_; }

private:
    std::string name_;
    std::atomic<int> submit_kernel_impl_calls_{0};
};

}  // namespace

// ---------------------------------------------------------------------------
// A1. LockFreeTaskExecutor 聚合反馈
// ---------------------------------------------------------------------------

TEST(LockfreeAggregateStats, DefaultNoBudgetCountsExecutedWithoutOverBudget) {
    LockFreeTaskExecutor executor(1024, 2, false);
    ASSERT_TRUE(executor.start());

    constexpr int kTasks = 100;
    std::atomic<int> ran{0};
    for (int i = 0; i < kTasks; ++i) {
        ASSERT_TRUE(executor.push_task([&ran] { ran.fetch_add(1); }));
    }
    EXPECT_TRUE(wait_until([&ran] { return ran.load() >= kTasks; }));

    EXPECT_TRUE(executor.stop_and_join());  // 排空并冻结计数

    const auto stats = executor.get_aggregate_execution_stats();
    EXPECT_EQ(stats.executed_count, static_cast<uint64_t>(kTasks));
    EXPECT_EQ(stats.over_budget_count, 0u)
        << "no declared budget must keep over_budget_count at zero";
    EXPECT_EQ(stats.period_count, 0u)
        << "lockfree backend has no period semantics";
    EXPECT_EQ(stats.executed_count, executor.processed_count())
        << "executed_count must agree with processed_count()";
    EXPECT_EQ(ran.load(), kTasks);
}

TEST(LockfreeAggregateStats, BudgetTwoSidedLimit) {
    // lockfree 执行器 stop 后不可重启（生命周期契约），两个预算阶段各用
    // 一个独立实例。
    // 上侧：1s 预算，空任务不可能超过 → 全部不计超。
    {
        LockFreeTaskExecutor executor(1024, 2, false);
        executor.set_execution_budget_ns(1'000'000'000);
        ASSERT_TRUE(executor.start());
        std::atomic<int> ran{0};
        for (int i = 0; i < 50; ++i) {
            ASSERT_TRUE(executor.push_task([&ran] {
                burn_microseconds();
                ran.fetch_add(1);
            }));
        }
        ASSERT_TRUE(wait_until([&ran] { return ran.load() >= 50; }));
        EXPECT_TRUE(executor.stop_and_join());
        const auto stats = executor.get_aggregate_execution_stats();
        EXPECT_EQ(stats.executed_count, 50u);
        EXPECT_EQ(stats.over_budget_count, 0u);
    }

    // 下侧：1ns 预算，任务携带 µs 级真实工作——任何平台的实测时长都
    // 严格大于 1ns（空任务在 ~100ns 时钟粒度下可能实测 0ns）→ 全部
    // 计超。同时验证"恰等于预算不算超"所依赖的严格比较口径：所有
    // 观测时长都 > 1ns，因此计数必须精确等于任务数。
    {
        LockFreeTaskExecutor executor(1024, 2, false);
        executor.set_execution_budget_ns(1);
        ASSERT_TRUE(executor.start());
        std::atomic<int> ran{0};
        for (int i = 0; i < 50; ++i) {
            ASSERT_TRUE(executor.push_task([&ran] {
                burn_microseconds();
                ran.fetch_add(1);
            }));
        }
        ASSERT_TRUE(wait_until([&ran] { return ran.load() >= 50; }));
        EXPECT_TRUE(executor.stop_and_join());

        const auto stats = executor.get_aggregate_execution_stats();
        EXPECT_EQ(stats.executed_count, 50u);
        EXPECT_EQ(stats.over_budget_count, 50u)
            << "every task under a 1ns budget must be counted as over-budget";
        EXPECT_EQ(stats.period_count, 0u);
    }
}

TEST(LockfreeAggregateStats, RuntimeBudgetChangeTakesEffect) {
    LockFreeTaskExecutor executor(1024, 2, false);
    executor.set_execution_budget_ns(1'000'000'000);  // 1s：第一阶段不计超
    ASSERT_TRUE(executor.start());

    std::atomic<int> ran{0};
    int expected_total = 0;
    auto push_batch = [&](int n) {
        expected_total += n;
        for (int i = 0; i < n; ++i) {
            ASSERT_TRUE(executor.push_task([&ran] {
                burn_microseconds();
                ran.fetch_add(1);
            }));
        }
        ASSERT_TRUE(wait_until([&ran, &expected_total] {
            return ran.load() >= expected_total;
        }));
    };

    push_batch(20);
    EXPECT_EQ(executor.get_aggregate_execution_stats().over_budget_count, 0u);

    // 运行期收紧到 1ns：后续任务（µs 级工作，实测 > 1ns）全部计超。
    executor.set_execution_budget_ns(1);
    push_batch(20);
    {
        const auto stats = executor.get_aggregate_execution_stats();
        EXPECT_EQ(stats.executed_count, 40u);
        EXPECT_EQ(stats.over_budget_count, 20u);
    }

    // 运行期归零（关闭计时）：计数停止增长，executed 继续增长。
    executor.set_execution_budget_ns(0);
    push_batch(20);
    EXPECT_TRUE(executor.stop_and_join());

    const auto stats = executor.get_aggregate_execution_stats();
    EXPECT_EQ(stats.executed_count, 60u);
    EXPECT_EQ(stats.over_budget_count, 20u)
        << "disabling the budget must stop over-budget accounting";
}

TEST(LockfreeAggregateStats, NegativeBudgetTreatedAsDisabled) {
    LockFreeTaskExecutor executor(1024, 2, false);
    executor.set_execution_budget_ns(-1234);  // 负值按 0（关闭）处理
    ASSERT_TRUE(executor.start());

    std::atomic<int> ran{0};
    for (int i = 0; i < 30; ++i) {
        ASSERT_TRUE(executor.push_task([&ran] { ran.fetch_add(1); }));
    }
    ASSERT_TRUE(wait_until([&ran] { return ran.load() >= 30; }));
    EXPECT_TRUE(executor.stop_and_join());

    const auto stats = executor.get_aggregate_execution_stats();
    EXPECT_EQ(stats.executed_count, 30u);
    EXPECT_EQ(stats.over_budget_count, 0u)
        << "negative budget must be clamped to disabled (0)";
}

TEST(LockfreeAggregateStats, ThrowingTaskStillCountedAsExecuted) {
    LockFreeTaskExecutor executor(1024, 2, false);
    executor.set_execution_budget_ns(1'000'000'000);
    ASSERT_TRUE(executor.start());

    std::atomic<int> ran{0};
    ASSERT_TRUE(executor.push_task([&ran] { ran.fetch_add(1); }));
    ASSERT_TRUE(executor.push_task(
        [] { throw std::runtime_error("aggregate stats probe"); }));
    ASSERT_TRUE(executor.push_task([&ran] { ran.fetch_add(1); }));
    ASSERT_TRUE(wait_until([&ran] { return ran.load() >= 2; }));
    EXPECT_TRUE(executor.stop_and_join());

    const auto stats = executor.get_aggregate_execution_stats();
    EXPECT_EQ(stats.executed_count, 3u)
        << "exception-terminated tasks must count as executed";
    EXPECT_EQ(executor.exception_count(), 1u);
    EXPECT_EQ(stats.over_budget_count, 0u);
}

// ---------------------------------------------------------------------------
// A2. Realtime 执行器聚合反馈（经 facade 注册）
// ---------------------------------------------------------------------------

TEST(RealtimeAggregateStats, ExecutedCountAndStatusSourceConsistency) {
    Executor executor;
    init_executor(executor, 2);

    const std::string rt_name = "agg_rt_consistency";
    ASSERT_TRUE(executor.register_realtime_task(rt_name,
                                                make_rt_config(rt_name, 1'000'000)));
    ASSERT_TRUE(executor.start_realtime_task(rt_name));

    std::atomic<int> ran{0};
    // 一个 5ms 任务在 1ms 周期里必然造成周期超时（over_budget 语义）。
    ASSERT_TRUE(executor.push_realtime_task(rt_name, [] {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }));
    push_and_wait(executor, rt_name, ran, 10);
    EXPECT_GE(ran.load(), 10);

    // 先停机冻结计数，两侧快照才可比（运行中允许继续推进）。
    executor.stop_realtime_task(rt_name);

    const auto stats = executor.get_aggregate_execution_stats(rt_name);
    const auto status = executor.get_realtime_executor_status(rt_name);
    EXPECT_GE(stats.executed_count, 11u);
    EXPECT_GE(stats.over_budget_count, 1u)
        << "5ms task in a 1ms period must overrun at least one cycle";
    EXPECT_GE(stats.period_count, 1u);
    EXPECT_EQ(stats.over_budget_count,
              static_cast<uint64_t>(status.cycle_timeout_count))
        << "over_budget_count must be the same source as cycle_timeout_count";
    EXPECT_EQ(stats.period_count,
              static_cast<uint64_t>(status.cycle_count))
        << "period_count must be the same source as cycle_count";
}

TEST(RealtimeAggregateStats, InitialSnapshotIsZeroed) {
    Executor executor;
    init_executor(executor, 2);

    const std::string rt_name = "agg_rt_fresh";
    ASSERT_TRUE(executor.register_realtime_task(rt_name,
                                                make_rt_config(rt_name)));
    // 未启动、未推任务：executed 恒 0；周期未跑时 period 亦 0。
    const auto stats = executor.get_aggregate_execution_stats(rt_name);
    EXPECT_EQ(stats.executed_count, 0u);
    EXPECT_EQ(stats.over_budget_count, 0u);
    EXPECT_EQ(stats.period_count, 0u);
}

// ---------------------------------------------------------------------------
// A3. Facade 聚合反馈 API
// ---------------------------------------------------------------------------

TEST(FacadeAggregateStats, UnknownNameReturnsZeros) {
    Executor executor;
    init_executor(executor, 2);

    const auto stats = executor.get_aggregate_execution_stats("no_such_executor");
    EXPECT_EQ(stats.executed_count, 0u);
    EXPECT_EQ(stats.over_budget_count, 0u);
    EXPECT_EQ(stats.period_count, 0u);
}

TEST(FacadeAggregateStats, ResolvesLockfreeAndRealtimeByName) {
    Executor executor;
    init_executor(executor, 2);

    // lockfree 后端
    const std::string lf_name = "agg_lf";
    auto lockfree = std::make_unique<LockFreeTaskExecutor>(1024, 2, false);
    LockFreeTaskExecutor* lf_raw = lockfree.get();
    ASSERT_TRUE(executor.register_lockfree_executor(lf_name, std::move(lockfree)));
    ASSERT_TRUE(executor.start_lockfree_executor(lf_name));
    std::atomic<int> lf_ran{0};
    for (int i = 0; i < 7; ++i) {
        // 直接向执行器推任务（facade 对 lockfree 无逐任务 submit API），
        // 与被测读路径（facade -> 执行器快照）解耦。
        ASSERT_TRUE(lf_raw->push_task([&lf_ran] { lf_ran.fetch_add(1); }));
    }
    ASSERT_TRUE(wait_until([&lf_ran] { return lf_ran.load() >= 7; }));
    executor.stop_lockfree_executor(lf_name);  // 排空并冻结

    // realtime 后端
    const std::string rt_name = "agg_rt";
    ASSERT_TRUE(executor.register_realtime_task(rt_name,
                                                make_rt_config(rt_name)));
    ASSERT_TRUE(executor.start_realtime_task(rt_name));
    std::atomic<int> rt_ran{0};
    push_and_wait(executor, rt_name, rt_ran, 3);
    executor.stop_realtime_task(rt_name);  // 冻结

    const auto lf_stats = executor.get_aggregate_execution_stats(lf_name);
    EXPECT_EQ(lf_stats.executed_count, 7u);
    EXPECT_EQ(lf_stats.over_budget_count, 0u);
    EXPECT_EQ(lf_stats.period_count, 0u);

    const auto rt_stats = executor.get_aggregate_execution_stats(rt_name);
    EXPECT_GE(rt_stats.executed_count, 3u);
    EXPECT_EQ(rt_stats.over_budget_count,
              static_cast<uint64_t>(
                  executor.get_realtime_executor_status(rt_name).cycle_timeout_count));

    // get_all_aggregate_execution_stats：覆盖两类全部注册执行器，
    // 且与按名查询逐字段一致。
    const auto all = executor.get_all_aggregate_execution_stats();
    ASSERT_EQ(all.size(), 2u)
        << "exactly the registered lockfree + realtime executors";
    ASSERT_NE(all.find(lf_name), all.end());
    ASSERT_NE(all.find(rt_name), all.end());
    EXPECT_EQ(all.at(lf_name).executed_count, lf_stats.executed_count);
    EXPECT_EQ(all.at(lf_name).over_budget_count, lf_stats.over_budget_count);
    EXPECT_EQ(all.at(lf_name).period_count, lf_stats.period_count);
    EXPECT_EQ(all.at(rt_name).executed_count, rt_stats.executed_count);
    EXPECT_EQ(all.at(rt_name).over_budget_count, rt_stats.over_budget_count);
    EXPECT_EQ(all.at(rt_name).period_count, rt_stats.period_count);
}

// ---------------------------------------------------------------------------
// A4. 接口默认实现
// ---------------------------------------------------------------------------

TEST(InterfaceDefaults, RealtimeExecutorDefaultStatsAreZero) {
    MinimalRealtimeExecutor executor;
    const auto stats = executor.get_aggregate_execution_stats();
    EXPECT_EQ(stats.executed_count, 0u);
    EXPECT_EQ(stats.over_budget_count, 0u);
    EXPECT_EQ(stats.period_count, 0u);
}

TEST(InterfaceDefaults, GpuExecutorDefaultHasNoTimingSupport) {
    MinimalGpuExecutor executor("minimal_gpu");

    EXPECT_FALSE(executor.supports_completion_timing())
        << "base IGpuExecutor must report no completion-timing support";

    // 默认 submit_kernel_with_timing：照常提交（submit_kernel_impl 被调
    // 用一次、任务执行）、绝不调用 sink。
    bool kernel_ran = false;
    bool sink_called = false;
    gpu::GpuTaskConfig config;
    config.stream_id = 0;
    auto future = executor.submit_kernel_with_timing(
        [&kernel_ran](void*) { kernel_ran = true; }, config,
        [&sink_called](const gpu::GpuKernelTimingRecord&) { sink_called = true; });
    EXPECT_NO_THROW(future.get());
    EXPECT_TRUE(kernel_ran);
    EXPECT_FALSE(sink_called)
        << "default submit_kernel_with_timing must not invoke the sink";
    EXPECT_EQ(executor.submit_kernel_impl_calls(), 1);
}
