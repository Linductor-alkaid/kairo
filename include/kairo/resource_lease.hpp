#pragma once

#include "scheduling.hpp"

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace kairo {

/**
 * @brief 预留失败原因（0.7.1 §3.1，稳定名称供诊断输出）。
 */
enum class ReserveError : uint8_t {
    None = 0,                // 成功
    NotConfigured,           // 预留未启用，或需求指向的配额桶不存在
    InvalidRequirement,      // 需求自相矛盾（如 GPU 维度未指定 device）
    InsufficientGpuMemory,   // 显存配额余量不足
    NoFreeGpuSlot,           // GPU 并发槽余量不足
    NoFreeCpuCore            // CPU 独占核配额余量不足
};

/** @brief ReserveError 的稳定名称（诊断与测试输出）。 */
const char* reserve_error_to_string(ReserveError error) noexcept;

/**
 * @brief 资源预留配额（静态声明式，0.7.1 §3.1）。
 *
 * 配额由集成方在 ExecutorConfig 中显式给出（与能力快照的 advisory
 * 可行性核对正交）：预留是 facade 级的确定性簿记，闭合的是"多个声明
 * 相同资源的提交在 admission 与执行之间超卖"的 TOCTOU 窗口；硬件层
 * 的弱一致语义（0.6.1 契约）不变。缺省桶 = 该维度未配置（预留请求
 * 返回 NotConfigured）。
 */
struct ResourceQuota {
    /** GPU 显存配额：device id -> 可预留字节数（0 值视为未配置）。 */
    std::unordered_map<int, size_t> gpu_memory_bytes;
    /** GPU 并发槽配额：device id -> 可同时预留的槽位数。 */
    std::unordered_map<int, size_t> gpu_slots;
    /** CPU 独占核配额（进程级总量）。 */
    size_t cpu_exclusive_cores = 0;
};

/**
 * @brief 资源预留注册表状态快照（诊断用）。
 */
struct ResourceReservationStatus {
    uint64_t granted_count = 0;       // 累计授予次数（含任务绑定与手动）
    uint64_t rejected_count = 0;      // 累计全或无拒绝次数
    uint64_t active_leases = 0;       // 当前在租约数
    size_t reserved_gpu_memory_bytes = 0;  // 各 device 在租显存合计
    size_t reserved_gpu_slots = 0;         // 各 device 在租槽位合计
    size_t reserved_cpu_cores = 0;         // 在租独占核合计
};

namespace detail {

/**
 * @brief 恰好一次的终态释放器（0.7.1 §3.1 lease 释放核心）。
 *
 * 语义与 Executor::AdmissionReleaser 同族：release() 以 CAS 保证只
 * 执行一次释放动作（先于 future 结算的观测不变式由调用方保证）；
 * 析构兜底无人显式释放的路径（shutdown 排空丢弃排队任务、提交被拒
 * 后闭包即弃等）。经 shared_ptr 被所有可能结算任务的闭包共享。
 */
struct TerminalReleaser {
    std::function<void()> on_release;   // 必须不抛
    std::atomic_bool released{false};

    explicit TerminalReleaser(std::function<void()> fn)
        : on_release(std::move(fn)) {}

    void release() noexcept {
        bool expected = false;
        if (released.compare_exchange_strong(expected, true,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
            on_release();
        }
    }

    ~TerminalReleaser() { release(); }

    TerminalReleaser(const TerminalReleaser&) = delete;
    TerminalReleaser& operator=(const TerminalReleaser&) = delete;
};

/**
 * @brief 资源预留注册表（0.7.1 §3.1）。
 *
 * 静态配额下的全或无授予：需求涉及多个维度时任一不足则整体回滚。
 * 互斥锁保护——预留只在声明资源且启用预留的提交路径发生，不在
 * 提交热路径的未声明快车道上。线程安全。
 */
class ResourceReservationRegistry {
public:
    explicit ResourceReservationRegistry(ResourceQuota quota);

    /**
     * @brief 尝试按需求授予租约（全或无）。
     *
     * 成功时 out 持有恰好一次释放器（析构兜底）；失败时不做部分授予，
     * 返回错误码。计数（granted/rejected/active）随之更新。
     */
    ReserveError try_acquire(
        const ResourceRequirements& requirements,
        std::shared_ptr<TerminalReleaser>& out);

    /** @brief 需求是否含任何预留维度（启用预留时才值得走注册表）。 */
    static bool requires_reservation(const ResourceRequirements& req) noexcept;

    ResourceReservationStatus status() const;

private:
    struct GpuBucketReserved {
        size_t memory_bytes = 0;
        size_t slots = 0;
    };

    mutable std::mutex mutex_;
    ResourceQuota quota_;
    std::unordered_map<int, GpuBucketReserved> gpu_reserved_;
    size_t cpu_reserved_ = 0;
    uint64_t granted_count_ = 0;
    uint64_t rejected_count_ = 0;
    uint64_t active_leases_ = 0;
};

}  // namespace detail

/**
 * @brief 资源租约句柄（RAII，0.7.1 §3.1）。
 *
 * 手动预留（Executor::try_reserve）的返回载体：析构自动归还配额，
 * release() 提前归还（幂等）。move-only。任务绑定的预留不经此类型——
 * facade 在提交闭包间共享恰好一次释放器，于任务终态释放。
 */
class ResourceLease {
public:
    ResourceLease() noexcept = default;

    explicit ResourceLease(
        std::shared_ptr<detail::TerminalReleaser> releaser) noexcept
        : releaser_(std::move(releaser)) {}

    ResourceLease(ResourceLease&& other) noexcept
        : releaser_(std::move(other.releaser_)) {}

    ResourceLease& operator=(ResourceLease&& other) noexcept {
        if (this != &other) {
            release();
            releaser_ = std::move(other.releaser_);
        }
        return *this;
    }

    ResourceLease(const ResourceLease&) = delete;
    ResourceLease& operator=(const ResourceLease&) = delete;

    ~ResourceLease() { release(); }

    /** @brief 是否持有在租约。 */
    bool valid() const noexcept { return releaser_ != nullptr; }

    /** @brief 提前归还（幂等）；归还后 valid() == false。 */
    void release() noexcept {
        if (releaser_) {
            releaser_->release();
            releaser_.reset();
        }
    }

private:
    std::shared_ptr<detail::TerminalReleaser> releaser_;
};

/**
 * @brief 手动预留结果（try_reserve 的返回载体）。
 *
 * error == None 时 lease 持有有效租约；否则 error 说明原因。
 */
struct ReservationResult {
    ResourceLease lease;
    ReserveError error = ReserveError::None;

    explicit operator bool() const noexcept {
        return error == ReserveError::None;
    }
};

}  // namespace kairo
