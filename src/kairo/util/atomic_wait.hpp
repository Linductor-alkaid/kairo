// Atomic wait/notify compatibility layer for GCC 9 / libstdc++ 9.
//
// std::atomic<T>::wait/notify_*（P1135）在 libstdc++ 12 才有实现；GCC 9/10
// 编不过依赖它的两处 worker 驻停循环。本头文件把全部 wake_seq_ 驻停/唤醒
// 流量收口到 kairo::util::atomic_wait / atomic_notify_one / atomic_notify_all
// 自由函数，按平台三级分发：
//   1. __cpp_lib_atomic_wait  -> 直发 std（CI 现网工具链全部走这条路径，
//      行为零变化，libstdc++ 对 32 位标量仍是 futex 直达）；
//   2. __linux__              -> 裸 futex(FUTEX_WAIT_PRIVATE/FUTEX_WAKE_PRIVATE)，
//      与 libstdc++ 的 32 位直达路径机制等价：阻塞前复核值、虚假唤醒安全；
//   3. 其余平台               -> 全局 mutex+cv 仿真（notify 先持锁再唤醒，
//      关闭"复核与阻塞之间"的丢唤醒窗口；仅服务无 futex 的兜底环境）。
// 只需要 std::atomic<uint32_t>：两处驻停代次计数器均为 4 字节（见
// lockfree_task_executor.hpp 的 32 位 futex 说明），天然满足 futex 对齐。
#pragma once

#include <atomic>
#include <version>

#if !defined(__cpp_lib_atomic_wait) && defined(__linux__)
#include <cerrno>
#include <climits>
#include <cstdint>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#if !defined(__cpp_lib_atomic_wait) && !defined(__linux__)
#include <condition_variable>
#include <mutex>
#endif

namespace kairo {
namespace util {

#if defined(__cpp_lib_atomic_wait)

inline void atomic_wait(std::atomic<uint32_t>& atomic, uint32_t expected,
                        std::memory_order order) noexcept {
    atomic.wait(expected, order);
}

inline void atomic_notify_one(std::atomic<uint32_t>& atomic) noexcept {
    atomic.notify_one();
}

inline void atomic_notify_all(std::atomic<uint32_t>& atomic) noexcept {
    atomic.notify_all();
}

#elif defined(__linux__)

inline void atomic_wait(std::atomic<uint32_t>& atomic, uint32_t expected,
                        std::memory_order order) noexcept {
    // libstdc++ 平台等待路径同款做法：futex 直接作用在原子对象的存储上
    // （32 位无锁原子与 uint32_t 布局等价）。wait 的约定是"值不再等于
    // expected 才返回"，虚假唤醒允许存在——两处调用方醒来后都会重扫。
    uint32_t* word = reinterpret_cast<uint32_t*>(&atomic);
    for (;;) {
        if (atomic.load(order) != expected) return;
        const long rc = ::syscall(SYS_futex, word,
                                  static_cast<int>(FUTEX_WAIT_PRIVATE),
                                  static_cast<uint32_t>(expected),
                                  static_cast<const void*>(nullptr),
                                  static_cast<void*>(nullptr), 0);
        if (rc == -1 && errno != EAGAIN && errno != EINTR) {
            // 非预期错误（理论上只有 EINVAL/EFAULT）：按虚假唤醒返回。
            // 调用方协议保证醒来重扫，最坏退化为忙轮询，不会挂死。
            return;
        }
        // EAGAIN：值已变（下一轮 load 复核直接返回）；EINTR / 成功唤醒：
        // 回到顶部复核。
    }
}

inline void atomic_notify_one(std::atomic<uint32_t>& atomic) noexcept {
    ::syscall(SYS_futex, reinterpret_cast<uint32_t*>(&atomic),
              static_cast<int>(FUTEX_WAKE_PRIVATE), 1,
              static_cast<const void*>(nullptr), static_cast<void*>(nullptr), 0);
}

inline void atomic_notify_all(std::atomic<uint32_t>& atomic) noexcept {
    ::syscall(SYS_futex, reinterpret_cast<uint32_t*>(&atomic),
              static_cast<int>(FUTEX_WAKE_PRIVATE), INT_MAX,
              static_cast<const void*>(nullptr), static_cast<void*>(nullptr), 0);
}

#else

// 无 futex 平台的兜底：全局门锁 + 条件变量。notify 侧先加锁再唤醒——
// 若 waiter 正处于"复核通过、尚未入眠"的窗口，notify 会在门锁上排队，
// 等 waiter 真正阻塞后才发唤醒，因此不丢。std::atomic 的值修改发生在
// 调用方（fetch_add 等），本层只负责睡眠/唤醒配对。
inline std::mutex& atomic_wait_gate_mutex() noexcept {
    static std::mutex gate;
    return gate;
}

inline std::condition_variable& atomic_wait_gate_cv() noexcept {
    static std::condition_variable cv;
    return cv;
}

inline void atomic_wait(std::atomic<uint32_t>& atomic, uint32_t expected,
                        std::memory_order order) noexcept {
    std::unique_lock<std::mutex> lock(atomic_wait_gate_mutex());
    atomic_wait_gate_cv().wait(lock,
                               [&] { return atomic.load(order) != expected; });
}

inline void atomic_notify_one(std::atomic<uint32_t>&) noexcept {
    { std::lock_guard<std::mutex> lock(atomic_wait_gate_mutex()); }
    atomic_wait_gate_cv().notify_one();
}

inline void atomic_notify_all(std::atomic<uint32_t>&) noexcept {
    { std::lock_guard<std::mutex> lock(atomic_wait_gate_mutex()); }
    atomic_wait_gate_cv().notify_all();
}

#endif

} // namespace util
} // namespace kairo
