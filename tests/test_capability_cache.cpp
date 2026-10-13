// 0.7.1 §3.0.1 能力快照 TTL 缓存 + submit_fast 契约测试
// (docs/design/roadmap_v0.7.md §3.0.1；设计语义见
// docs/design/scheduling_runtime.md §9)。
//
// 覆盖：
//  A. 命中/未命中计数与决策来源标记：CpuOrGpu 路由首采后 TTL 内命中
//     （hit 增长、CapabilityCacheHit 诊断位与 capability_from_cache 同置）；
//     纯策略路径（needs_capabilities == false）不计数。
//  B. TTL 语义：过期后未命中重采；ttl = 0 完全关闭缓存（恒未命中）；
//     set_capability_cache_ttl 立即丢弃旧快照。
//  C. 主动失效（state_epoch）：注册/启动/停止 lockfree 后端后，TTL 内的
//     下一次 dispatch_auto 立即看到新状态（BackendUnavailable → Accepted →
//     BackendNotRunning），证明缓存不跨生命周期转换漂移。
//  D. 决策一致性：缓存开/关产生逐项相同的 status/reason/backend/executor
//     （缓存只影响快照新鲜度，不影响决策语义）；公开的
//     get_executor_capabilities() 绕过缓存保持实时。
//  E. submit_fast：NORMAL 优先级直投默认池、返回值/异常经 future 传递、
//     不产出 RoutingDecision（accepted/rejected 计数与 recent decisions
//     不变）、admission 上界生效、空 callable 拒绝语义与 submit 一致。
//  F/G. 并发冒烟 + 计数守恒：多提交线程 CpuOrGpu + dispatch_auto 与
//     注册/停止线程（epoch 推进）并发，无崩溃、无死锁，
//     hit + miss 与能力消费型路由次数精确相等。

#include <gtest/gtest.h>

#include <kairo/executor.hpp>
#include <kairo/scheduler.hpp>
#include <kairo/scheduling.hpp>
#include <kairo/task_options.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

using namespace kairo;

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

namespace {

void init_executor(Executor& executor, size_t threads) {
    ExecutorConfig config;
    config.min_threads = threads;
    config.max_threads = threads;
    ASSERT_TRUE(executor.initialize(config));
}

// 无 GPU 后端时 CpuOrGpu 提交必被结构化拒绝（NoFallback 默认策略），
// future 以 runtime_error 就绪；该路径仍消费能力表，是缓存行为的最小
// 可观测样本。返回 future 供调用方结算（get() 预期抛出拒绝原因）。
std::future<void> submit_cpuorgpu(Executor& executor, const std::string& name) {
    return executor.submit_auto(
        cpu_gpu_task([] {}, [](void*) {}).name(name));
}

void consume_rejection(std::future<void>& future) {
    try {
        future.get();
        ADD_FAILURE() << "CpuOrGpu submit without GPU backend must be rejected";
    } catch (const std::runtime_error&) {
        // 结构化拒绝经 future 传递（NoFallback + 无可用 GPU）。
    }
}

uint64_t total_capability_routes(const SchedulingMetrics& metrics) {
    return metrics.capability_cache_hit_count +
           metrics.capability_cache_miss_count;
}

}  // namespace

// ---------------------------------------------------------------------------
// A. 命中/未命中计数与决策来源标记
// ---------------------------------------------------------------------------

TEST(CapabilityCacheTest, SecondCpuOrGpuRouteHitsCacheAndMarksProvenance) {
    Executor executor;
    init_executor(executor, 2);
    executor.set_recent_routing_capacity(8);

    auto first = submit_cpuorgpu(executor, "first");
    consume_rejection(first);
    auto second = submit_cpuorgpu(executor, "second");
    consume_rejection(second);

    const auto metrics = executor.get_scheduling_metrics();
    EXPECT_EQ(metrics.capability_cache_miss_count, 1u);
    EXPECT_EQ(metrics.capability_cache_hit_count, 1u);

    const auto decisions = executor.get_recent_routing_decisions(2);
    ASSERT_EQ(decisions.size(), 2u);
    EXPECT_FALSE(decisions[0].capability_from_cache);
    EXPECT_EQ(decisions[0].diagnostics & RoutingDiagnostics::CapabilityCacheHit,
              0u);
    EXPECT_TRUE(decisions[1].capability_from_cache);
    EXPECT_NE(decisions[1].diagnostics & RoutingDiagnostics::CapabilityCacheHit,
              0u);
    // 两次决策的调度语义逐项一致（缓存不改变决策结果）。
    EXPECT_EQ(decisions[0].status, decisions[1].status);
    EXPECT_EQ(decisions[0].reason, decisions[1].reason);
    EXPECT_EQ(decisions[0].selected_backend, decisions[1].selected_backend);
}

TEST(CapabilityCacheTest, PolicyOnlyRoutesDoNotTouchCacheCounters) {
    Executor executor;
    init_executor(executor, 2);
    // 预热：先做一次能力消费型路由，建立非零基线。
    auto warm = submit_cpuorgpu(executor, "warm");
    consume_rejection(warm);
    const auto baseline = executor.get_scheduling_metrics();

    for (int i = 0; i < 8; ++i) {
        (void)executor.submit_auto(kairo::task([] {}));
    }

    const auto after = executor.get_scheduling_metrics();
    EXPECT_EQ(total_capability_routes(after), total_capability_routes(baseline));
}

// ---------------------------------------------------------------------------
// B. TTL 语义
// ---------------------------------------------------------------------------

TEST(CapabilityCacheTest, ExpiredTtlForcesRecollection) {
    Executor executor;
    init_executor(executor, 2);
    executor.set_capability_cache_ttl(milliseconds(5));

    auto first = submit_cpuorgpu(executor, "a");
    consume_rejection(first);
    std::this_thread::sleep_for(milliseconds(20));
    auto second = submit_cpuorgpu(executor, "b");
    consume_rejection(second);

    const auto metrics = executor.get_scheduling_metrics();
    EXPECT_EQ(metrics.capability_cache_miss_count, 2u);
    EXPECT_EQ(metrics.capability_cache_hit_count, 0u);
}

TEST(CapabilityCacheTest, ZeroTtlDisablesCacheEntirely) {
    Executor executor;
    init_executor(executor, 2);
    EXPECT_EQ(executor.capability_cache_ttl(), milliseconds(100));
    executor.set_capability_cache_ttl(std::chrono::nanoseconds(0));
    EXPECT_EQ(executor.capability_cache_ttl(), std::chrono::nanoseconds(0));

    for (int i = 0; i < 4; ++i) {
        auto future = submit_cpuorgpu(executor, "t" + std::to_string(i));
        consume_rejection(future);
    }

    const auto metrics = executor.get_scheduling_metrics();
    EXPECT_EQ(metrics.capability_cache_miss_count, 4u);
    EXPECT_EQ(metrics.capability_cache_hit_count, 0u);
}

TEST(CapabilityCacheTest, SetterDropsStaleSnapshotImmediately) {
    Executor executor;
    init_executor(executor, 2);
    auto warm = submit_cpuorgpu(executor, "warm");
    consume_rejection(warm);
    EXPECT_EQ(executor.get_scheduling_metrics().capability_cache_hit_count, 0u);

    // 重新设置 TTL 同时丢弃旧快照：下一次路由必须重采（旧快照不得因
    // "名义上仍在 TTL 内" 而命中）。
    executor.set_capability_cache_ttl(milliseconds(100));
    auto after_set = submit_cpuorgpu(executor, "after-set");
    consume_rejection(after_set);

    const auto metrics = executor.get_scheduling_metrics();
    EXPECT_EQ(metrics.capability_cache_miss_count, 2u);
    EXPECT_EQ(metrics.capability_cache_hit_count, 0u);
}

// ---------------------------------------------------------------------------
// C. 主动失效（manager state_epoch）
// ---------------------------------------------------------------------------

TEST(CapabilityCacheTest, RegistrationAndStopInvalidateCacheWithinTtl) {
    Executor executor;
    init_executor(executor, 2);
    executor.set_capability_cache_ttl(
        std::chrono::seconds(10));  // 长 TTL：失效只能来自 epoch

    TaskOptions options;
    options.name = "lf-dispatch";
    options.intent = ExecutionIntent::LowLatency;
    options.preferred_executor = "cache-lf";

    // 1) 未注册：结构化拒绝 + 首次采集。
    auto rejected = executor.dispatch_auto(options, [] {});
    EXPECT_FALSE(rejected.accepted);
    EXPECT_EQ(rejected.decision.reason, RoutingReason::BackendUnavailable);

    // 2) 注册但未启动：TTL 内的下一次路由必须立即看到（epoch 失效）。
    ASSERT_TRUE(executor.register_lockfree_executor(
        "cache-lf", std::make_unique<LockFreeTaskExecutor>(8)));
    auto stopped = executor.dispatch_auto(options, [] {});
    EXPECT_FALSE(stopped.accepted);
    EXPECT_EQ(stopped.decision.reason, RoutingReason::BackendNotRunning);

    // 3) 启动：同一 TTL 内必须变为可接受。
    ASSERT_TRUE(executor.start_lockfree_executor("cache-lf"));
    std::atomic<int> executions{0};
    auto accepted = executor.dispatch_auto(options, [&] { ++executions; });
    EXPECT_TRUE(accepted.accepted);
    EXPECT_EQ(accepted.backend, ExecutionBackend::LockFree);

    // 4) 停止：再次立即反映为未运行。
    executor.stop_lockfree_executor("cache-lf");
    auto stopped_again = executor.dispatch_auto(options, [] {});
    EXPECT_FALSE(stopped_again.accepted);
    EXPECT_EQ(stopped_again.decision.reason, RoutingReason::BackendNotRunning);

    EXPECT_EQ(executions.load(), 1);
}

TEST(CapabilityCacheTest, PublicCapabilityQueryBypassesCache) {
    Executor executor;
    init_executor(executor, 2);
    executor.set_capability_cache_ttl(std::chrono::seconds(10));
    auto warm = submit_cpuorgpu(executor, "warm");
    consume_rejection(warm);

    // 公开查询 API 保持实时语义（每次全量采集，不读缓存）。
    const size_t before = executor.get_executor_capabilities().size();
    ASSERT_TRUE(executor.register_lockfree_executor(
        "cache-lf", std::make_unique<LockFreeTaskExecutor>(8)));
    const auto after = executor.get_executor_capabilities();

    EXPECT_EQ(after.size(), before + 1);
    bool found = false;
    for (const auto& capability : after) {
        if (capability.backend == ExecutionBackend::LockFree &&
            capability.name == "cache-lf") {
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

// ---------------------------------------------------------------------------
// D. 决策一致性（缓存开/关逐项相同）
// ---------------------------------------------------------------------------

TEST(CapabilityCacheTest, CachedAndFreshRoutingDecisionsAreIdentical) {
    // 同一组输入序列分别在"缓存关闭"与"缓存开启（已预热）"的两个
    // Executor 上路由；决策流除缓存位外必须逐项相同。
    Executor fresh_executor;
    init_executor(fresh_executor, 2);
    fresh_executor.set_capability_cache_ttl(std::chrono::nanoseconds(0));

    Executor cached_executor;
    init_executor(cached_executor, 2);
    auto warm = submit_cpuorgpu(cached_executor, "warm");  // 预热缓存
    consume_rejection(warm);

    for (Executor* target : {&fresh_executor, &cached_executor}) {
        auto future = submit_cpuorgpu(*target, "matrix-cpuorgpu");
        consume_rejection(future);
        TaskOptions options;
        options.name = "matrix-lf";
        options.intent = ExecutionIntent::LowLatency;
        options.preferred_executor = "missing";
        (void)target->dispatch_auto(options, [] {});
    }

    // 各取末尾 2 条（cached 侧头部多一条预热决策，对齐后比较）。
    const auto fresh_decisions =
        fresh_executor.get_recent_routing_decisions(2);
    const auto cached_decisions =
        cached_executor.get_recent_routing_decisions(2);
    ASSERT_EQ(fresh_decisions.size(), cached_decisions.size());
    ASSERT_EQ(fresh_decisions.size(), 2u);
    for (size_t i = 0; i < fresh_decisions.size(); ++i) {
        EXPECT_EQ(fresh_decisions[i].status, cached_decisions[i].status)
            << "decision " << i;
        EXPECT_EQ(fresh_decisions[i].reason, cached_decisions[i].reason)
            << "decision " << i;
        EXPECT_EQ(fresh_decisions[i].selected_backend,
                  cached_decisions[i].selected_backend)
            << "decision " << i;
        EXPECT_EQ(fresh_decisions[i].selected_executor_name,
                  cached_decisions[i].selected_executor_name)
            << "decision " << i;
        EXPECT_EQ(fresh_decisions[i].fell_back, cached_decisions[i].fell_back)
            << "decision " << i;
        EXPECT_EQ(fresh_decisions[i].detail, cached_decisions[i].detail)
            << "decision " << i;
    }
}

// ---------------------------------------------------------------------------
// E. submit_fast
// ---------------------------------------------------------------------------

TEST(SubmitFastTest, ExecutesOnDefaultPoolAndReturnsValue) {
    Executor executor;
    init_executor(executor, 2);

    auto value_future = executor.submit_fast([] { return 42; });
    EXPECT_EQ(value_future.get(), 42);

    std::atomic<int> side_effect{0};
    auto void_future = executor.submit_fast([&] { ++side_effect; });
    void_future.get();
    EXPECT_EQ(side_effect.load(), 1);
}

TEST(SubmitFastTest, ExceptionPropagatesThroughFuture) {
    Executor executor;
    init_executor(executor, 2);

    auto future = executor.submit_fast(
        []() -> int { throw std::runtime_error("submit_fast boom"); });
    EXPECT_THROW(future.get(), std::runtime_error);
}

TEST(SubmitFastTest, ProducesNoRoutingDecisionsOrMetrics) {
    Executor executor;
    init_executor(executor, 2);
    executor.set_recent_routing_capacity(8);

    const auto before = executor.get_scheduling_metrics();
    (void)executor.submit_fast([] {});
    const auto after = executor.get_scheduling_metrics();

    EXPECT_EQ(after.accepted_count, before.accepted_count);
    EXPECT_EQ(after.rejected_count, before.rejected_count);
    EXPECT_EQ(after.accepted_degraded_count, before.accepted_degraded_count);
    EXPECT_TRUE(executor.get_recent_routing_decisions(8).empty());

    // 与 submit_auto 对照：裸 builder 提交会计 Accepted（DefaultPolicy）。
    (void)executor.submit_auto(kairo::task([] {}));
    const auto with_auto = executor.get_scheduling_metrics();
    EXPECT_EQ(with_auto.accepted_count, before.accepted_count + 1);
}

TEST(SubmitFastTest, RespectsBoundedAdmission) {
    Executor executor;
    init_executor(executor, 1);
    executor.set_max_in_flight_tasks(1);

    // 占满唯一 admission 名额并阻塞 worker：后续 submit_fast 立即以
    // CapacityExhausted 就绪（不阻塞提交线程）。
    std::atomic<bool> gate{false};
    auto blocker = executor.submit_fast([&] {
        while (!gate.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(milliseconds(1));
        }
    });
    auto rejected = executor.submit_fast([] {});
    EXPECT_THROW(rejected.get(), CapacityExhaustedException);
    gate.store(true, std::memory_order_release);
    blocker.get();
}

TEST(SubmitFastTest, EmptyCallableRejectionMatchesSubmitSemantics) {
    Executor executor;
    init_executor(executor, 2);

    std::function<void()> empty;
    auto future = executor.submit_fast(empty);
    EXPECT_THROW(future.get(), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// F/G. 并发冒烟 + 计数守恒
// ---------------------------------------------------------------------------

TEST(CapabilityCacheTest, ConcurrentSubmissionsWithEpochBumpsStayConsistent) {
    Executor executor;
    init_executor(executor, 4);
    executor.set_capability_cache_ttl(milliseconds(20));
    executor.set_recent_routing_capacity(0);  // 关闭观测：隔离计数变量

    ASSERT_TRUE(executor.register_lockfree_executor(
        "churn-lf", std::make_unique<LockFreeTaskExecutor>(8)));
    ASSERT_TRUE(executor.start_lockfree_executor("churn-lf"));

    constexpr int kThreads = 4;
    constexpr int kSubmitsPerThread = 120;
    std::atomic<int> capability_routes{0};
    std::atomic<bool> stop_churn{false};
    std::vector<std::thread> workers;
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t] {
            for (int i = 0; i < kSubmitsPerThread; ++i) {
                auto future = submit_cpuorgpu(
                    executor, "concurrent-" + std::to_string(t) + "-" +
                                  std::to_string(i));
                consume_rejection(future);  // 恰一次能力消费型 route_task

                TaskOptions options;
                options.name = "concurrent-dispatch";
                options.intent = ExecutionIntent::LowLatency;
                options.preferred_executor = "churn-lf";
                (void)executor.dispatch_auto(options, [] {});  // 恰一次 route_dispatch
                capability_routes.fetch_add(2, std::memory_order_relaxed);
            }
        });
    }

    // epoch 扰动线程：反复注册/停止第二个后端，推进 manager state_epoch
    //（强制未命中路径与命中路径并发交错）。
    std::thread churn([&] {
        for (int i = 0; i < 40 && !stop_churn.load(std::memory_order_acquire);
             ++i) {
            if (executor.register_lockfree_executor(
                    "churn-tmp", std::make_unique<LockFreeTaskExecutor>(8))) {
                executor.stop_lockfree_executor("churn-tmp");
            }
            std::this_thread::sleep_for(milliseconds(2));
        }
    });

    for (auto& worker : workers) {
        worker.join();
    }
    stop_churn.store(true, std::memory_order_release);
    churn.join();

    const auto metrics = executor.get_scheduling_metrics();
    // 守恒：每个能力消费型路由恰计一次 hit 或 miss。
    EXPECT_EQ(total_capability_routes(metrics),
              static_cast<uint64_t>(capability_routes.load()));
    EXPECT_GT(metrics.capability_cache_hit_count, 0u);
    EXPECT_GT(metrics.capability_cache_miss_count, 0u);
}
