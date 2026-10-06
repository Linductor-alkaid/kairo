// Single-atom admission gate shared by LockFreeTaskExecutor (P-001) and
// RealtimeThreadExecutor (P-002).
//
// Encoding: bit 31 = closed by stop_and_join(); bits 0..30 count producers
// that won admission and are still inside the submission path. Registration
// and the closed check are one RMW, so a producer either counts itself
// before the close — and stop waits for it before joining/draining — or
// rejects without touching the object pool, queue, or any other member.
//
// Storage stays in each class (std::atomic<uint32_t>): LockFreeTaskExecutor
// is a public header and must not depend on internal src/ headers, so these
// are free functions over the caller's atomic rather than a gate object.
#pragma once

#include <atomic>
#include <cstdint>

namespace kairo {
namespace util {

inline constexpr uint32_t kAdmissionClosedBit = uint32_t{1} << 31;
inline constexpr uint32_t kAdmissionActiveMask = kAdmissionClosedBit - 1;

/**
 * @brief 原子准入：关门检查与计数递增是单一 RMW
 *
 * 返回 true 表示调用者已获准，必须恰好调用一次 admission_leave()。
 * 关门后或计数饱和（溢出保护，2^31 并发生产者实际不可达，仅为保证
 * 编码健全）返回 false。
 */
inline bool admission_enter(std::atomic<uint32_t>& gate) noexcept {
    uint32_t state = gate.load(std::memory_order_relaxed);
    while (true) {
        if (state & kAdmissionClosedBit) {
            return false;
        }
        if ((state & kAdmissionActiveMask) == kAdmissionActiveMask) {
            return false;
        }
        if (gate.compare_exchange_weak(
                state, static_cast<uint32_t>(state + 1),
                std::memory_order_acq_rel, std::memory_order_relaxed)) {
            return true;
        }
    }
}

inline void admission_leave(std::atomic<uint32_t>& gate) noexcept {
    gate.fetch_sub(1, std::memory_order_release);
}

inline void admission_close(std::atomic<uint32_t>& gate) noexcept {
    gate.fetch_or(kAdmissionClosedBit, std::memory_order_acq_rel);
}

// RealtimeThreadExecutor 重启时重开门（保留已完成 stop 等到 0 的活动
// 计数）；LockFreeTaskExecutor 一旦关门不再重开（stop 后 start 拒绝）。
inline void admission_reopen(std::atomic<uint32_t>& gate) noexcept {
    gate.fetch_and(kAdmissionActiveMask, std::memory_order_acq_rel);
}

inline bool admission_is_closed(const std::atomic<uint32_t>& gate) noexcept {
    return (gate.load(std::memory_order_acquire) & kAdmissionClosedBit) != 0;
}

inline bool admission_has_active(const std::atomic<uint32_t>& gate) noexcept {
    return (gate.load(std::memory_order_acquire) & kAdmissionActiveMask) != 0;
}

} // namespace util
} // namespace kairo
