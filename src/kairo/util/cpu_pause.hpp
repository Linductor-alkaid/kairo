// Shared CPU pause/backoff primitives for lock-free spin loops.
//
// This is the single definition point for the pause instruction. It
// previously existed as three independent macros (PAUSE_INSTRUCTION in
// lockfree_queue.hpp, KAIRO_POOL_PAUSE in object_pool.hpp, KAIRO_PAUSE in
// lockfree_task_executor.cpp) with divergent fallbacks on unlisted
// architectures (no-op vs yield). The unified fallback is a real yield —
// the conservative choice for a bounded spin loop on an unknown core.
#pragma once

#include <cstddef>
#include <thread>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <emmintrin.h>
#endif
#define KAIRO_CPU_PAUSE() _mm_pause()
#elif defined(__aarch64__) || defined(__arm__)
#define KAIRO_CPU_PAUSE() __asm__ volatile("yield" ::: "memory")
#else
#define KAIRO_CPU_PAUSE() std::this_thread::yield()
#endif

namespace kairo {
namespace util {

/**
 * @brief 有界指数退避 pause（失败 CAS 重试专用）
 *
 * pause() 按当前计数执行 pause 指令，随后计数翻倍并封顶于 max_pauses。
 * 构造参数为首次计数与封顶值，例如：
 *  - LockFreeQueue::push 使用 PauseBackoff{multiplier, 16 * multiplier}
 *    （原实现：backoff 从 1 翻倍至 16，每次乘以退避倍数）
 *  - ObjectPool::acquire 使用 PauseBackoff{1, 32}
 */
class PauseBackoff {
public:
    PauseBackoff(size_t first_pauses, size_t max_pauses) noexcept
        : next_(first_pauses == 0 ? 1 : first_pauses)
        , max_(max_pauses < next_ ? next_ : max_pauses) {}

    void pause() noexcept {
        const size_t count = next_;
        for (size_t i = 0; i < count; ++i) {
            KAIRO_CPU_PAUSE();
        }
        // next_ <= max_ 恒成立：next_ > max_ - next_ 时直接封顶，
        // 否则 next_*2 <= max_，翻倍不会回绕。
        next_ = next_ > max_ - next_ ? max_ : next_ * 2;
    }

private:
    size_t next_;
    size_t max_;
};

} // namespace util
} // namespace kairo
