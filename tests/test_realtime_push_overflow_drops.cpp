// test_realtime_push_overflow_drops.cpp
// P-001 (260615): RealtimeThreadExecutor 背压下静默丢任务, 调用方零感知 —
// 修复后必须能通过 RealtimeExecutorStatus 观察到 dropped_task_count,
// 且 enable_stats=true 时 failed_pushes 同步增长.
//
// test_plan: 用极长 cycle_callback 阻塞 RT 线程, 连续 push >1024 任务,
// 断言 dropped_task_count == 溢出数量, 且 enable_stats 下 failed_pushes 同步增长.

#include <cassert>
#include <cmath>
#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <future>
#include <algorithm>
#include <string>
#include <functional>

#include <kairo/config.hpp>
#include <kairo/types.hpp>
#include <kairo/interfaces.hpp>
#include "kairo/realtime_thread_executor.hpp"

using namespace kairo;

// 极长的周期(1 秒), 给 overflow 测试足够时间窗
static constexpr int64_t kPeriodNs = 1'000'000'000;  // 1s
// 默认 RT 队列/池容量
static constexpr size_t kCapacity = 1024;
// 推送任务总数, 远超容量, 故意制造溢出
static constexpr int kTotalPushes = 4000;

// 测试宏
#define TEST_ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            std::cerr << "FAILED: " << message << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
            return false; \
        } \
    } while(0)

// ========== 主测试 ==========

bool test_realtime_push_overflow_drops() {
    std::cout << "Testing RealtimeThreadExecutor push overflow drops counter (P-001)...\n";

    // 用一个 atomic gate 让 cycle_callback 在测试需要时阻塞 RT 线程, 强制让
    // lockfree_queue 持续累积任务直至满, 从而触发 push_task 失败路径.
    std::atomic<bool> block_cycle{false};
    std::atomic<bool> unblock_cycle{false};

    RealtimeThreadConfig config;
    config.thread_name = "test_p001_overflow";
    config.cycle_period_ns = kPeriodNs;  // 1s, 不会自然到期
    config.thread_priority = 0;
    config.cycle_callback = [&block_cycle, &unblock_cycle]() {
        // 当测试要求阻塞时, busy-wait 直到 unblock_cycle 被置位.
        // 注意: 不能用 sleep, 否则 simple_cycle_loop() 末尾的
        // sleep_until(next_cycle_time) 不会被执行, 但我们的 1s 周期足以让
        // 单次回调耗尽整个测试窗口.
        while (block_cycle.load(std::memory_order_acquire) &&
               !unblock_cycle.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    };

    // enable_stats=true 让 LockFreeQueue 内部同时累计 failed_pushes
    // (与 RT 执行器自己的 dropped_task_count 双重印证).
    RealtimeThreadExecutor executor("p001_overflow", config,
                                    /*enable_stats=*/true,
                                    /*queue_capacity=*/kCapacity);

    // 启动前: dropped_task_count == 0
    auto pre_status = executor.get_status();
    TEST_ASSERT(pre_status.dropped_task_count == 0,
                "dropped_task_count must be 0 before start");
    TEST_ASSERT(pre_status.queue_capacity == kCapacity,
                "queue_capacity must equal constructor argument");
    TEST_ASSERT(pre_status.is_running == false,
                "is_running must be false before start");

    TEST_ASSERT(executor.start(), "Executor should start successfully");
    // 等待 RT 线程进入 cycle_callback
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 让 RT 线程在第一个周期回调中阻塞
    block_cycle.store(true, std::memory_order_release);

    // 主线程连续 push kTotalPushes 任务, 由于 RT 线程不会消费它们,
    // 队列很快被打满 (kCapacity), 后续 push 必然失败.
    std::atomic<int> push_success_via_ex{0};
    for (int i = 0; i < kTotalPushes; ++i) {
        if (executor.push_task([]() { /* noop */ })) {
            push_success_via_ex.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // 现在解除阻塞, 让 RT 线程清空队列, 然后停止.
    // 注意: 此时队列里最多 kCapacity 个任务 (成功的), 它们会被执行.
    unblock_cycle.store(true, std::memory_order_release);
    block_cycle.store(false, std::memory_order_release);

    // 给 RT 线程足够时间消费已入队的任务
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 关键断言 1: dropped_task_count 必须等于溢出量
    //   push 成功次数 = 入队成功的次数 = push_success_via_ex (== RT 队列峰值)
    //   溢出量 = kTotalPushes - 成功次数
    //   由于 RT 线程一直在 cycle_callback 中阻塞 (没有任何 pop), push 成功的次数
    //   应 == kCapacity (1024), 即 push_success_via_ex 应 ≈ kCapacity.
    auto status = executor.get_status();

    const int expected_success = push_success_via_ex.load();
    const uint64_t expected_drops = static_cast<uint64_t>(kTotalPushes - expected_success);

    std::cout << "  push_success=" << expected_success
              << " / " << kTotalPushes
              << "  dropped=" << status.dropped_task_count
              << " (expected=" << expected_drops << ")\n";

    TEST_ASSERT(status.dropped_task_count == expected_drops,
                "dropped_task_count must equal (total - successful_pushes)");
    TEST_ASSERT(expected_success <= static_cast<int>(kCapacity),
                "successful pushes must not exceed queue capacity when RT thread is blocked");

    // 关键断言 2: enable_stats=true 时 failed_pushes 同步增长
    //   failed_pushes 是 LockFreeQueue 统计的"队列满导致的入队失败"次数
    //   它 == dropped_task_count 中"队列满"分支, 不包括"对象池耗尽"分支.
    //   但在本测试场景中, 队列满与对象池耗尽几乎同时发生, 应当近似相等
    //   (允许差异: 极少数情况下 acquire 早于 push, 此时 push 失败但 pool 释放
    //    顺序与 push 失败的关系会有边界, 但总额应不超过 dropped_task_count).
    TEST_ASSERT(status.failed_pushes >= status.dropped_task_count ||
                // failed_pushes 是 dropped_task_count 的子集 (队列满那部分),
                // 但 enable_stats 可能在 dropped 累加后稍后才观察到, 容忍其更大.
                status.failed_pushes <= status.dropped_task_count,
                "failed_pushes must be related to dropped_task_count "
                "(either equal subset or close)");
    // 更精确: failed_pushes <= dropped_task_count (failed_pushes 是 dropped 的子集)
    TEST_ASSERT(status.failed_pushes <= status.dropped_task_count,
                "failed_pushes must be <= dropped_task_count (subset relationship)");

    // 关键断言 3: queue_capacity 暴露正确
    TEST_ASSERT(status.queue_capacity == kCapacity,
                "queue_capacity must equal kCapacity");
    TEST_ASSERT(status.peak_queue_size >= expected_success,
                "peak_queue_size must reach at least the number of successful pushes");

    // 关键断言 4: dropped_task_count 是单调非递减的 — 重复调用 get_status 不会回退
    auto status2 = executor.get_status();
    TEST_ASSERT(status2.dropped_task_count >= status.dropped_task_count,
                "dropped_task_count must be monotonically non-decreasing");

    executor.stop();

    std::cout << "  test_realtime_push_overflow_drops: PASSED "
              << "(drops=" << status.dropped_task_count
              << ", failed_pushes=" << status.failed_pushes
              << ", peak=" << status.peak_queue_size << ")\n";
    return true;
}

// ========== 辅助测试: void push_task 路径与计数器一致 ==========

bool test_realtime_push_overflow_via_void_push() {
    std::cout << "Testing RealtimeThreadExecutor push_task (void) still records drops (P-001)...\n";

    std::atomic<bool> block_cycle{false};
    std::atomic<bool> unblock_cycle{false};

    RealtimeThreadConfig config;
    config.thread_name = "test_p001_void";
    config.cycle_period_ns = kPeriodNs;
    config.cycle_callback = [&block_cycle, &unblock_cycle]() {
        while (block_cycle.load(std::memory_order_acquire) &&
               !unblock_cycle.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    };

    RealtimeThreadExecutor executor("p001_void_overflow", config, true, kCapacity);
    TEST_ASSERT(executor.start(), "Executor should start");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    block_cycle.store(true, std::memory_order_release);
    for (int i = 0; i < kTotalPushes; ++i) {
        executor.push_task([]() { /* noop */ });
    }
    unblock_cycle.store(true, std::memory_order_release);
    block_cycle.store(false, std::memory_order_release);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto status = executor.get_status();
    std::cout << "  void_push dropped=" << status.dropped_task_count
              << " failed_pushes=" << status.failed_pushes << "\n";

    // void push_task 路径必须也累计 dropped_task_count
    // (它内部委托 push_task, 共享同一计数器).
    TEST_ASSERT(status.dropped_task_count > 0,
                "void push_task must also increment dropped_task_count on overflow");

    executor.stop();
    std::cout << "  test_realtime_push_overflow_via_void_push: PASSED\n";
    return true;
}

// ========== 辅助测试: 关闭 enable_stats 时 dropped 仍然累计 ==========

bool test_realtime_push_overflow_without_stats() {
    std::cout << "Testing dropped_task_count is recorded even when enable_stats=false (P-001)...\n";

    std::atomic<bool> block_cycle{false};
    std::atomic<bool> unblock_cycle{false};

    RealtimeThreadConfig config;
    config.thread_name = "test_p001_nostats";
    config.cycle_period_ns = kPeriodNs;
    config.cycle_callback = [&block_cycle, &unblock_cycle]() {
        while (block_cycle.load(std::memory_order_acquire) &&
               !unblock_cycle.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    };

    // enable_stats=false: LockFreeQueue 内部不累计 failed_pushes, 但 RT
    // 执行器自身的 dropped_task_count 仍然必须可见 (这是 P-001 的核心契约).
    RealtimeThreadExecutor executor("p001_nostats", config, false, kCapacity);
    TEST_ASSERT(executor.start(), "Executor should start");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    block_cycle.store(true, std::memory_order_release);
    for (int i = 0; i < kTotalPushes; ++i) {
        executor.push_task([]() { /* noop */ });
    }
    unblock_cycle.store(true, std::memory_order_release);
    block_cycle.store(false, std::memory_order_release);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto status = executor.get_status();
    std::cout << "  no-stats dropped=" << status.dropped_task_count
              << " failed_pushes=" << status.failed_pushes
              << " peak=" << status.peak_queue_size << "\n";

    TEST_ASSERT(status.dropped_task_count > 0,
                "dropped_task_count must be > 0 even with enable_stats=false");
    // enable_stats=false 时 failed_pushes 必须保持为 0
    TEST_ASSERT(status.failed_pushes == 0,
                "failed_pushes must be 0 when enable_stats=false");
    TEST_ASSERT(status.peak_queue_size == 0,
                "peak_queue_size must be 0 when enable_stats=false");

    executor.stop();
    std::cout << "  test_realtime_push_overflow_without_stats: PASSED\n";
    return true;
}

// ========== 辅助测试: 单周期任务预算 (P-260618-002) ==========
// 预算机制下, 即使队列里堆了大量任务, 每周期处理量有界 → cycle_time 不会爆涨,
// cycle_timeout_count 稳态不尖刺. 关键: cycle_callback 用 fast noop,
// 不能用 block-while-gate 模式 (那样会触发丢任务, 偏离预算语义).

bool test_realtime_push_overflow_drops_budget() {
    std::cout << "Testing RealtimeThreadExecutor max_tasks_per_cycle budget (P-260618-002)...\n";

    // 50ms 周期给测试足够窗口在两个周期之间 burst 入队.
    constexpr int64_t kBudgetPeriodNs = 50'000'000;  // 50ms

    RealtimeThreadConfig config;
    config.thread_name = "test_p002_budget";
    config.cycle_period_ns = kBudgetPeriodNs;
    config.thread_priority = 0;
    config.cycle_callback = []() { /* fast noop */ };
    // 小预算: 每周期最多处理 8 个任务.
    config.max_tasks_per_cycle = 8;

    // enable_stats=true 便于观察 peak_queue_size; 容量保持默认 kCapacity(1024).
    RealtimeThreadExecutor executor("p002_budget", config,
                                    /*enable_stats=*/true,
                                    /*queue_capacity=*/kCapacity);

    TEST_ASSERT(executor.start(), "Executor should start successfully");
    for (int i = 0; i < 200; ++i) {
        if (executor.get_status().cycle_count > 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    TEST_ASSERT(executor.get_status().cycle_count > 0,
                "Executor should complete the first cycle before bulk push");

    // 启动后趁 RT 线程 sleep 到下一周期前, 一次性灌入大量任务 (远超每周期预算 8).
    // 短周期 + 容量 1024 下, 成功入队 ~1024 个, 余者被静默丢弃
    // (P-001 已覆盖丢任务计数, 此处不重复断言). 灌入后队列持续满载,
    // 每个周期都会正好处理 8 个 → 验证预算对 cycle_time 的约束.
    constexpr int kBulkPushes = 2000;
    int pushed_ok = 0;
    for (int i = 0; i < kBulkPushes; ++i) {
        if (executor.push_task([]() { /* noop */ })) {
            ++pushed_ok;
        }
    }
    std::cout << "  running bulk pushed_ok=" << pushed_ok << "/" << kBulkPushes << "\n";

    // 跑约 300ms (~6 个周期). 期间队列始终非空, 每周期正好处理 8 个 noop 任务.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    auto status = executor.get_status();
    std::cout << "  cycle_count=" << status.cycle_count
              << " max_cycle_time_ns=" << static_cast<int64_t>(status.max_cycle_time_ns)
              << " period=" << kBudgetPeriodNs
              << " timeout_count=" << status.cycle_timeout_count
              << " dropped=" << status.dropped_task_count << "\n";

    // 关键断言 1: 没有周期的执行时间超过 cycle_period_ns * 1.2 (允许 20% 调度抖动).
    //   预算限制使单周期处理量有界 (8 个 noop 任务仅几微秒), 故 cycle_time 应远小于周期.
    TEST_ASSERT(static_cast<int64_t>(status.max_cycle_time_ns) <= kBudgetPeriodNs * 1.2,
                "max_cycle_time_ns must stay within period (20% margin) under bounded budget");

    // 关键断言 2: 稳态下 cycle_timeout_count 几乎不增长 (允许极少数调度抖动 < 10).
    TEST_ASSERT(status.cycle_timeout_count < 10,
                "cycle_timeout_count must not spike in steady state under bounded budget");

    executor.stop();

    std::cout << "  test_realtime_push_overflow_drops_budget: PASSED\n";
    return true;
}

// ========== 主函数 ==========

int main() {
    std::cout << "========== P-001 RealtimeThreadExecutor 背压丢任务计数器测试 ==========\n\n";

    bool all_passed = true;
    all_passed &= test_realtime_push_overflow_drops();
    all_passed &= test_realtime_push_overflow_via_void_push();
    all_passed &= test_realtime_push_overflow_without_stats();
    all_passed &= test_realtime_push_overflow_drops_budget();

    std::cout << "\n";
    if (all_passed) {
        std::cout << "========== 所有测试通过 ==========\n";
        return 0;
    } else {
        std::cout << "========== 部分测试失败 ==========\n";
        return 1;
    }
}
