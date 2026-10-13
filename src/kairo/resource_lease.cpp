#include "kairo/resource_lease.hpp"

// 0.7.1 §3.1 资源预留注册表实现：静态配额下的全或无授予与恰好一次
// 归还。设计语义见 docs/design/scheduling_runtime.md §12。

namespace kairo {

const char* reserve_error_to_string(ReserveError error) noexcept {
    switch (error) {
    case ReserveError::None:
        return "None";
    case ReserveError::NotConfigured:
        return "NotConfigured";
    case ReserveError::InvalidRequirement:
        return "InvalidRequirement";
    case ReserveError::InsufficientGpuMemory:
        return "InsufficientGpuMemory";
    case ReserveError::NoFreeGpuSlot:
        return "NoFreeGpuSlot";
    case ReserveError::NoFreeCpuCore:
        return "NoFreeCpuCore";
    default:
        return "Unknown";
    }
}

namespace detail {

ResourceReservationRegistry::ResourceReservationRegistry(ResourceQuota quota)
    : quota_(std::move(quota)) {}

bool ResourceReservationRegistry::requires_reservation(
    const ResourceRequirements& req) noexcept {
    return req.memory_bytes > 0 || req.gpu_concurrent_slots > 0 ||
           req.cpu_exclusive_cores > 0;
}

ReserveError ResourceReservationRegistry::try_acquire(
    const ResourceRequirements& requirements,
    std::shared_ptr<TerminalReleaser>& out) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 校验在前：任何需求形状问题直接拒绝，不做部分授予。
    const bool wants_gpu_memory = requirements.memory_bytes > 0;
    const bool wants_gpu_slots = requirements.gpu_concurrent_slots > 0;
    const bool wants_cpu = requirements.cpu_exclusive_cores > 0;
    if ((wants_gpu_memory || wants_gpu_slots) && requirements.gpu_device < 0) {
        ++rejected_count_;
        return ReserveError::InvalidRequirement;
    }

    const auto quota_bucket = quota_.gpu_memory_bytes.find(requirements.gpu_device);
    const auto slot_bucket = quota_.gpu_slots.find(requirements.gpu_device);
    if ((wants_gpu_memory &&
         (quota_bucket == quota_.gpu_memory_bytes.end() ||
          quota_bucket->second == 0)) ||
        (wants_gpu_slots &&
         (slot_bucket == quota_.gpu_slots.end() || slot_bucket->second == 0))) {
        ++rejected_count_;
        return ReserveError::NotConfigured;
    }
    if (wants_cpu && quota_.cpu_exclusive_cores == 0) {
        ++rejected_count_;
        return ReserveError::NotConfigured;
    }

    // 可行性检查（全或无：任一维度不足则不授予）。
    GpuBucketReserved& reserved = gpu_reserved_[requirements.gpu_device];
    if (wants_gpu_memory &&
        quota_bucket->second - reserved.memory_bytes < requirements.memory_bytes) {
        ++rejected_count_;
        return ReserveError::InsufficientGpuMemory;
    }
    if (wants_gpu_slots &&
        slot_bucket->second - reserved.slots < requirements.gpu_concurrent_slots) {
        ++rejected_count_;
        return ReserveError::NoFreeGpuSlot;
    }
    if (wants_cpu &&
        quota_.cpu_exclusive_cores - cpu_reserved_ <
            requirements.cpu_exclusive_cores) {
        ++rejected_count_;
        return ReserveError::NoFreeCpuCore;
    }

    // 提交授予。
    if (wants_gpu_memory) {
        reserved.memory_bytes += requirements.memory_bytes;
    }
    if (wants_gpu_slots) {
        reserved.slots += requirements.gpu_concurrent_slots;
    }
    if (wants_cpu) {
        cpu_reserved_ += requirements.cpu_exclusive_cores;
    }
    ++granted_count_;
    ++active_leases_;

    // 恰好一次归还：显式 release 或析构兜底（任务终态 / shutdown 排空
    // 丢弃排队任务 / 手动租约析构）。捕获授予量，而非整个需求——调用方
    // 可能复用/修改需求对象。
    const size_t granted_memory =
        wants_gpu_memory ? requirements.memory_bytes : 0;
    const size_t granted_slots =
        wants_gpu_slots ? requirements.gpu_concurrent_slots : 0;
    const size_t granted_cores =
        wants_cpu ? requirements.cpu_exclusive_cores : 0;
    const int device = requirements.gpu_device;
    out = std::make_shared<TerminalReleaser>(
        [this, device, granted_memory, granted_slots, granted_cores]() noexcept {
            std::lock_guard<std::mutex> release_lock(mutex_);
            if (granted_memory > 0 || granted_slots > 0) {
                gpu_reserved_[device].memory_bytes -= granted_memory;
                gpu_reserved_[device].slots -= granted_slots;
            }
            cpu_reserved_ -= granted_cores;
            --active_leases_;
        });
    return ReserveError::None;
}

ResourceReservationStatus ResourceReservationRegistry::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    ResourceReservationStatus snapshot;
    snapshot.granted_count = granted_count_;
    snapshot.rejected_count = rejected_count_;
    snapshot.active_leases = active_leases_;
    snapshot.reserved_cpu_cores = cpu_reserved_;
    for (const auto& [device, bucket] : gpu_reserved_) {
        (void)device;
        snapshot.reserved_gpu_memory_bytes += bucket.memory_bytes;
        snapshot.reserved_gpu_slots += bucket.slots;
    }
    return snapshot;
}

}  // namespace detail

}  // namespace kairo
