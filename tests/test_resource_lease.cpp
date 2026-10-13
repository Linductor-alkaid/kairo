// 0.7.1 §3.1 ResourceLease / 资源预留契约测试
// (docs/design/roadmap_v0.7.md §3.1；设计语义见
// docs/design/scheduling_runtime.md §12)。
//
// 覆盖：
//  A. opt-in 边界：未启用预留时 try_reserve 返回 NotConfigured，提交
//     路径行为与 0.7.0 一致（声明 ResourceRequirements 不触发预留）。
//  B. 手动预留：配额内授予/超量拒绝（全或无）、RAII 归还、提前
//     release 幂等、GPU 各维度（显存/槽位/未配置 device/缺 device）。
//  C. 任务绑定预留（submit_auto TaskBuilder）：admission 扣留 → 任务
//     终态归还；并发超卖以结构化拒绝结算（CapacityPressure +
//     ResourceInfeasible，决策与计数一致）。
//  D. 终态释放矩阵——lease 在所有终态路径归还，无一泄漏：
//     成功、任务异常、queued 软超时、DropIfLate 丢弃、
//     max_in_flight admission 拒绝。
//  E. tracked 路径（submit_after TaskBuilder）：预留 + 终态归还 +
//     配额不足结构化拒绝。
//  F. 多维全或无：CPU 核 + GPU 显存联合需求，任一不足不产生部分授予。

#include <gtest/gtest.h>

#include <kairo/executor.hpp>
#include <kairo/resource_lease.hpp>
#include <kairo/scheduler.hpp>

#include <atomic>
#include <chrono>
#include <thread>

using namespace kairo;

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

namespace {

ExecutorConfig reservation_config(size_t threads = 2) {
    ExecutorConfig config;
    config.min_threads = threads;
    config.max_threads = threads;
    config.enable_resource_reservation = true;
    config.resource_quota.cpu_exclusive_cores = 2;
    config.resource_quota.gpu_memory_bytes[0] = 1024;
    config.resource_quota.gpu_slots[0] = 2;
    return config;
}

ResourceRequirements cpu_req(size_t cores) {
    ResourceRequirements req;
    req.cpu_exclusive_cores = cores;
    return req;
}

ResourceRequirements gpu_mem_req(size_t bytes, int device = 0) {
    ResourceRequirements req;
    req.memory_bytes = bytes;
    req.gpu_device = device;
    return req;
}

// 等待 worker 侧释放器完成归还（释放与 future 结算同点，但状态快照
// 读取可能晚于任务完成一个调度步）。
void wait_for_active_leases(Executor& executor, size_t expected) {
    for (int i = 0; i < 2000; ++i) {
        if (executor.get_resource_reservation_status().active_leases == expected) {
            return;
        }
        std::this_thread::sleep_for(milliseconds(1));
    }
    EXPECT_EQ(executor.get_resource_reservation_status().active_leases,
              expected);
}

}  // namespace

// ---------------------------------------------------------------------------
// A. opt-in 边界
// ---------------------------------------------------------------------------

TEST(ResourceLeaseTest, DisabledByDefaultMatchesLegacyBehavior) {
    Executor executor;
    ExecutorConfig config;
    config.min_threads = 2;
    config.max_threads = 2;
    ASSERT_TRUE(executor.initialize(config));

    auto result = executor.try_reserve(cpu_req(1));
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, ReserveError::NotConfigured);

    // 声明资源需求的提交不受影响（无预留、无拒绝）。
    auto future = executor.submit_auto(
        kairo::task([] { return 1; }).resources(cpu_req(2)));
    EXPECT_EQ(future.get(), 1);
    const auto status = executor.get_resource_reservation_status();
    EXPECT_EQ(status.granted_count, 0u);
    EXPECT_EQ(status.active_leases, 0u);
}

// ---------------------------------------------------------------------------
// B. 手动预留
// ---------------------------------------------------------------------------

TEST(ResourceLeaseTest, ManualReserveGrantDenyAndRaiiRelease) {
    Executor executor;
    ASSERT_TRUE(executor.initialize(reservation_config()));

    {
        auto first = executor.try_reserve(cpu_req(1));
        ASSERT_TRUE(first);
        EXPECT_EQ(executor.get_resource_reservation_status().active_leases, 1u);
        EXPECT_EQ(executor.get_resource_reservation_status().reserved_cpu_cores,
                  1u);

        // 余量不足：全或无拒绝。
        auto overflow = executor.try_reserve(cpu_req(2));
        EXPECT_FALSE(overflow);
        EXPECT_EQ(overflow.error, ReserveError::NoFreeCpuCore);
        EXPECT_EQ(executor.get_resource_reservation_status().rejected_count, 1u);
    }  // RAII 归还

    EXPECT_EQ(executor.get_resource_reservation_status().active_leases, 0u);
    EXPECT_EQ(executor.get_resource_reservation_status().reserved_cpu_cores, 0u);

    // 归还后可再次授予；提前 release 幂等。
    auto again = executor.try_reserve(cpu_req(2));
    ASSERT_TRUE(again);
    again.lease.release();
    EXPECT_FALSE(again.lease.valid());
    EXPECT_EQ(executor.get_resource_reservation_status().active_leases, 0u);
    again.lease.release();
    EXPECT_EQ(executor.get_resource_reservation_status().active_leases, 0u);
    EXPECT_EQ(executor.get_resource_reservation_status().granted_count, 2u);
}

TEST(ResourceLeaseTest, GpuQuotaDimensions) {
    Executor executor;
    ASSERT_TRUE(executor.initialize(reservation_config()));

    auto memory = executor.try_reserve(gpu_mem_req(768));
    ASSERT_TRUE(memory);
    EXPECT_EQ(executor.get_resource_reservation_status()
                  .reserved_gpu_memory_bytes,
              768u);

    // 显存余量不足。
    auto memory_overflow = executor.try_reserve(gpu_mem_req(512));
    EXPECT_FALSE(memory_overflow);
    EXPECT_EQ(memory_overflow.error, ReserveError::InsufficientGpuMemory);

    // 未配置的 device → NotConfigured。
    auto unknown_device = executor.try_reserve(gpu_mem_req(1, 3));
    EXPECT_FALSE(unknown_device);
    EXPECT_EQ(unknown_device.error, ReserveError::NotConfigured);

    // GPU 维度缺 device → InvalidRequirement。
    ResourceRequirements no_device;
    no_device.memory_bytes = 1;
    auto invalid = executor.try_reserve(no_device);
    EXPECT_FALSE(invalid);
    EXPECT_EQ(invalid.error, ReserveError::InvalidRequirement);

    // 槽位维度与显存独立：显存在租不占用槽位配额。
    ResourceRequirements slots_req;
    slots_req.gpu_device = 0;
    slots_req.gpu_concurrent_slots = 2;
    auto slots = executor.try_reserve(slots_req);
    ASSERT_TRUE(slots);
    EXPECT_EQ(executor.get_resource_reservation_status().reserved_gpu_slots, 2u);

    ResourceRequirements slot_overflow = slots_req;
    slot_overflow.gpu_concurrent_slots = 1;
    auto over = executor.try_reserve(slot_overflow);
    EXPECT_FALSE(over);
    EXPECT_EQ(over.error, ReserveError::NoFreeGpuSlot);
}

// ---------------------------------------------------------------------------
// C. 任务绑定预留（submit_auto TaskBuilder）
// ---------------------------------------------------------------------------

TEST(ResourceLeaseTest, TaskBoundReservationReleasesAtTerminalState) {
    Executor executor;
    ASSERT_TRUE(executor.initialize(reservation_config()));

    auto future = executor.submit_auto(
        kairo::task([] { return 11; }).resources(cpu_req(2)));

    EXPECT_EQ(future.get(), 11);
    wait_for_active_leases(executor, 0);
    const auto status = executor.get_resource_reservation_status();
    EXPECT_EQ(status.granted_count, 1u);
    EXPECT_EQ(status.reserved_cpu_cores, 0u);
}

TEST(ResourceLeaseTest, ConcurrentOversubscriptionRejectsStructuredly) {
    Executor executor;
    ASSERT_TRUE(executor.initialize(reservation_config()));

    // 占满独占核配额的长期任务。
    std::atomic<bool> gate{false};
    auto holder = executor.submit_auto(
        kairo::task([&gate]() -> int {
            while (!gate.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(milliseconds(1));
            }
            return 0;
        }).resources(cpu_req(2)));
    // 等 holder 的预留生效。
    for (int i = 0; i < 2000; ++i) {
        if (executor.get_resource_reservation_status().active_leases == 1) break;
        std::this_thread::sleep_for(milliseconds(1));
    }

    // 配额已满：第二个声明同样需求的提交被结构化拒绝。
    auto rejected = executor.submit_auto(
        kairo::task([] { return 2; }).resources(cpu_req(1)));
    EXPECT_THROW(rejected.get(), std::runtime_error);

    const auto decisions = executor.get_recent_routing_decisions(2);
    ASSERT_FALSE(decisions.empty());
    const auto& last = decisions.back();
    EXPECT_EQ(last.status, RoutingStatus::Rejected);
    EXPECT_EQ(last.reason, RoutingReason::CapacityPressure);
    EXPECT_NE(last.diagnostics & RoutingDiagnostics::ResourceInfeasible, 0u);

    const auto metrics = executor.get_scheduling_metrics();
    EXPECT_GE(metrics.resource_rejected_count, 1u);

    gate.store(true, std::memory_order_release);
    (void)holder.get();
    wait_for_active_leases(executor, 0);
    // 释放后同需求可再授予。
    auto retried = executor.submit_auto(
        kairo::task([] { return 3; }).resources(cpu_req(2)));
    EXPECT_EQ(retried.get(), 3);
}

// ---------------------------------------------------------------------------
// D. 终态释放矩阵（CR-010/011 类泄漏不可复现）
// ---------------------------------------------------------------------------

TEST(ResourceLeaseTerminalStatesTest, ExceptionStillReleasesLease) {
    Executor executor;
    ASSERT_TRUE(executor.initialize(reservation_config()));
    auto future = executor.submit_auto(
        kairo::task([]() -> int { throw std::runtime_error("boom"); })
            .resources(cpu_req(1)));
    EXPECT_THROW(future.get(), std::runtime_error);
    wait_for_active_leases(executor, 0);
}

TEST(ResourceLeaseTerminalStatesTest, QueuedSoftTimeoutReleasesLease) {
    Executor executor;
    auto config = reservation_config(1);
    config.task_timeout_ms = 30;
    ASSERT_TRUE(executor.initialize(config));

    // 门闩占住唯一 worker（带更早 deadline 防 EDF 插队，见
    // test_deadline_policy.cpp 的构造说明），目标任务排队熬过软超时。
    std::atomic<bool> gate{false};
    auto blocker = executor.submit_auto(
        kairo::task([&gate] {
            while (!gate.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(milliseconds(1));
            }
        }).deadline(Clock::now() + milliseconds(5)));

    auto victim = executor.submit_auto(
        kairo::task([] { return 1; }).resources(cpu_req(1)));
    std::this_thread::sleep_for(milliseconds(100));
    gate.store(true, std::memory_order_release);
    EXPECT_THROW(victim.get(), TimedOutException);
    blocker.get();
    wait_for_active_leases(executor, 0);
}

TEST(ResourceLeaseTerminalStatesTest, DeadlineDropReleasesLease) {
    Executor executor;
    ASSERT_TRUE(executor.initialize(reservation_config(1)));

    std::atomic<bool> gate{false};
    auto blocker = executor.submit_auto(
        kairo::task([&gate] {
            while (!gate.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(milliseconds(1));
            }
        }).deadline(Clock::now() + milliseconds(5)));

    auto victim = executor.submit_auto(
        kairo::task([] { return 1; })
            .deadline(Clock::now() + milliseconds(30))
            .deadline_policy(DeadlinePolicy::DropIfLate)
            .resources(cpu_req(1)));
    std::this_thread::sleep_for(milliseconds(100));
    gate.store(true, std::memory_order_release);
    bool dropped = false;
    try {
        victim.get();
    } catch (const TaskCancelled& cancelled) {
        dropped = cancelled.reason() == TaskCancellationReason::DeadlineMissed;
    }
    EXPECT_TRUE(dropped);
    blocker.get();
    wait_for_active_leases(executor, 0);
}

TEST(ResourceLeaseTerminalStatesTest, BoundedAdmissionRejectionReleasesLease) {
    Executor executor;
    auto config = reservation_config(1);
    config.max_in_flight_tasks = 1;
    ASSERT_TRUE(executor.initialize(config));

    std::atomic<bool> gate{false};
    auto holder = executor.submit_auto(
        kairo::task([&gate]() -> int {
            while (!gate.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(milliseconds(1));
            }
            return 0;
        }).resources(cpu_req(1)));
    for (int i = 0; i < 2000; ++i) {
        if (executor.get_resource_reservation_status().active_leases == 1) break;
        std::this_thread::sleep_for(milliseconds(1));
    }

    // admission 满额：预留已授予的后续提交被 admission 拒绝——租约必须
    // 随拒绝即时归还（terminal_release 与 admission 同链）。
    auto rejected = executor.submit_auto(
        kairo::task([] { return 1; }).resources(cpu_req(1)));
    EXPECT_THROW(rejected.get(), CapacityExhaustedException);
    EXPECT_EQ(executor.get_resource_reservation_status().active_leases, 1u);

    gate.store(true, std::memory_order_release);
    (void)holder.get();
    wait_for_active_leases(executor, 0);
}

// ---------------------------------------------------------------------------
// E. tracked 路径
// ---------------------------------------------------------------------------

TEST(ResourceLeaseTrackedTest, TrackedReservationReleasesAtTerminalState) {
    Executor executor;
    ASSERT_TRUE(executor.initialize(reservation_config()));

    auto submission = executor.submit_after_with_handle(
        std::vector<TaskHandle>{},
        kairo::task([] { return 21; }).resources(cpu_req(2)));
    EXPECT_EQ(submission.future.get(), 21);
    wait_for_active_leases(executor, 0);
    EXPECT_EQ(executor.get_resource_reservation_status().granted_count, 1u);
}

TEST(ResourceLeaseTrackedTest, TrackedReservationFailureRejectsStructuredly) {
    Executor executor;
    ASSERT_TRUE(executor.initialize(reservation_config()));

    // 占满配额的长任务（tracked）。
    auto holder = executor.submit_after_with_handle(
        std::vector<TaskHandle>{},
        kairo::task([]() -> int {
            std::this_thread::sleep_for(milliseconds(50));
            return 0;
        }).resources(cpu_req(2)));
    for (int i = 0; i < 2000; ++i) {
        if (executor.get_resource_reservation_status().active_leases == 1) break;
        std::this_thread::sleep_for(milliseconds(1));
    }

    // 配额不足：tracked 提交以结构化拒绝结算（未入队），配额不变。
    auto rejected = executor.submit_after_with_handle(
        std::vector<TaskHandle>{},
        kairo::task([] { return 1; }).resources(cpu_req(1)));
    EXPECT_THROW(rejected.future.get(), std::runtime_error);
    EXPECT_GE(executor.get_resource_reservation_status().rejected_count, 1u);
    EXPECT_EQ(executor.get_resource_reservation_status().active_leases, 1u);

    (void)holder.future.get();
    wait_for_active_leases(executor, 0);
}

// ---------------------------------------------------------------------------
// F. 多维全或无
// ---------------------------------------------------------------------------

TEST(ResourceLeaseTest, MultiDimensionRequirementIsAllOrNothing) {
    Executor executor;
    ASSERT_TRUE(executor.initialize(reservation_config()));

    // 联合需求：CPU 核充足但显存超量 → 整体拒绝，CPU 配额不得被部分扣留。
    ResourceRequirements joint;
    joint.gpu_device = 0;
    joint.memory_bytes = 4096;
    joint.cpu_exclusive_cores = 1;
    auto result = executor.try_reserve(joint);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, ReserveError::InsufficientGpuMemory);
    EXPECT_EQ(executor.get_resource_reservation_status().reserved_cpu_cores, 0u);
    EXPECT_EQ(executor.get_resource_reservation_status().active_leases, 0u);
}
