// test_realtime_tasks_per_cycle_budget.cpp
// P-260618-002: 验证 RealtimeThreadExecutor 的单周期任务预算契约 —
// max_tasks_per_cycle = N 时, 每周期 process_tasks() 最多处理 N 个任务,
// 即使队列里堆了更多 (4*N). 通过 cycle_callback 采样相邻周期间的任务增量
// 来直接测量单周期处理量: simple_cycle_loop 每轮恰好调用一次 callback + 一次
// process_tasks, 故两次 callback 之间的任务增量 == 上一周期 process_tasks 的处理量.
//
// test_plan:
//   - max_tasks_per_cycle = 16, 启动后等首个空周期完成再推 64 个计数任务 (burst).
//   - 跑 ~300ms (50ms 周期 → ~6 周期).
//   - 断言: 单周期处理量 <= 16 (无预算旧行为下首周期会一口气处理全部 64 → 失败),
//           cycle_count >= 2, max_cycle_time_ns <= period*1.2, 全部 64 任务最终执行
//           (队列清空, 无积压).

#include <iostream>
#include <thread>
#include <atomic>
#include <chrono>
#include <string>
#include <functional>
#include <cstdint>

#include <kairo/config.hpp>
#include <kairo/types.hpp>
#include <kairo/interfaces.hpp>
#include "kairo/realtime_thread_executor.hpp"

using namespace kairo;

// 测试宏
#define TEST_ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            std::cerr << "FAILED: " << message << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
            return false; \
        } \
    } while(0)

// 全局任务计数: 仅 RT 线程在 process_tasks 中执行任务时递增, 在 cycle_callback 中采样.
// 同一 RT 线程顺序访问, 用 relaxed atomic 仅作跨线程可见性 (main 线程 stop 后读取).
static std::atomic<int> g_tasks_executed{0};
// 上一周期 callback 时的计数快照.
static std::atomic<int> g_last_sample{0};
// 单周期处理任务数的最大值 (相邻 callback 之间的增量).
static std::atomic<int> g_max_per_cycle{0};

bool test_realtime_tasks_per_cycle_budget() {
    std::cout << "Testing RealtimeThreadExecutor max_tasks_per_cycle budget contract (P-260618-002)...\n";

    // 重置全局计数器
    g_tasks_executed.store(0, std::memory_order_relaxed);
    g_last_sample.store(0, std::memory_order_relaxed);
    g_max_per_cycle.store(0, std::memory_order_relaxed);

    constexpr int N = 16;                        // max_tasks_per_cycle
    constexpr int kTotalTasks = 4 * N;           // 64, 远超单周期预算
    constexpr int64_t kPeriodNs = 50'000'000;    // 50ms, leaves room to burst during sleep
    constexpr size_t kCapacity = 1024;

    RealtimeThreadConfig config;
    config.thread_name = "test_p002_budget_contract";
    config.cycle_period_ns = kPeriodNs;
    config.thread_priority = 0;
    config.max_tasks_per_cycle = N;
    // cycle_callback 在 process_tasks 之前运行; 采样相邻周期间的任务增量,
    // 该增量恰等于上一周期 process_tasks 实际处理的任务数 (每轮一次 callback + 一次 process_tasks).
    config.cycle_callback = []() {
        const int now = g_tasks_executed.load(std::memory_order_relaxed);
        const int prev = g_last_sample.exchange(now, std::memory_order_relaxed);
        const int delta = now - prev;  // 上一周期 process_tasks 处理的任务数
        int cur_max = g_max_per_cycle.load(std::memory_order_relaxed);
        while (delta > cur_max) {
            if (g_max_per_cycle.compare_exchange_weak(cur_max, delta,
                    std::memory_order_relaxed, std::memory_order_relaxed)) {
                break;
            }
        }
    };

    RealtimeThreadExecutor executor("p002_budget_contract", config,
                                    /*enable_stats=*/false,
                                    /*queue_capacity=*/kCapacity);

    TEST_ASSERT(executor.start(), "Executor should start successfully");
    for (int i = 0; i < 200; ++i) {
        if (executor.get_status().cycle_count > 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    TEST_ASSERT(executor.get_status().cycle_count > 0,
                "Executor should complete the first cycle before burst push");

    // 启动后趁 RT 线程 sleep 到下一周期前, 一次性 burst 灌入 kTotalTasks 个计数任务
    // (容量 1024 >> 64, 全部入队). 这避免阻塞 cycle_callback 污染 cycle_time 统计.
    // 每个任务仅自增全局计数 (轻量), 真正要测的是预算对"每周期处理量"的约束.
    for (int i = 0; i < kTotalTasks; ++i) {
        auto ok = executor.push_task([]() {
            g_tasks_executed.fetch_add(1, std::memory_order_relaxed);
        });
        TEST_ASSERT(ok, "Burst push while running should succeed");
    }

    // 50ms 周期, 64 任务, 预算 16/周期 → 4 周期 (~200ms) 即可全部清空.
    // 等 ~300ms 保证完全消费.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    auto status = executor.get_status();

    // 关键断言 1: 至少跑过 2 个周期 (测量有意义).
    TEST_ASSERT(status.cycle_count >= 2, "at least 2 cycles must have run");

    // 关键断言 2: 单周期处理任务数 <= N (预算硬上限).
    //   无预算 (旧行为) 时第一个周期会一口气处理全部 64 个 → g_max_per_cycle == 64 > N → 失败.
    //   有预算时 g_max_per_cycle <= N.
    const int measured_max = g_max_per_cycle.load(std::memory_order_relaxed);
    std::cout << "  max_tasks_per_cycle_measured=" << measured_max << " (budget N=" << N << ")\n";
    TEST_ASSERT(measured_max <= N,
                "tasks processed in a single cycle must not exceed max_tasks_per_cycle");

    // 关键断言 3: 周期执行时间不超预算 (20% 抖动余量).
    std::cout << "  max_cycle_time_ns=" << static_cast<int64_t>(status.max_cycle_time_ns)
              << " period=" << kPeriodNs
              << " cycle_count=" << status.cycle_count << "\n";
    TEST_ASSERT(static_cast<int64_t>(status.max_cycle_time_ns) <= kPeriodNs * 1.2,
                "max_cycle_time_ns must stay within period (20% margin)");

    // 关键断言 4: 队列最终被消费空 (无积压) — 全部 64 个任务都执行了.
    const int executed = g_tasks_executed.load(std::memory_order_relaxed);
    std::cout << "  tasks_executed=" << executed << "/" << kTotalTasks << "\n";
    TEST_ASSERT(executed == kTotalTasks,
                "all pushed tasks must eventually execute (queue drains, no infinite backlog)");

    executor.stop();
    std::cout << "  test_realtime_tasks_per_cycle_budget: PASSED\n";
    return true;
}

int main() {
    std::cout << "========== P-260618-002 单周期任务预算契约测试 ==========\n\n";
    bool ok = test_realtime_tasks_per_cycle_budget();
    std::cout << "\n";
    if (ok) {
        std::cout << "========== 所有测试通过 ==========\n";
        return 0;
    }
    std::cout << "========== 测试失败 ==========\n";
    return 1;
}
