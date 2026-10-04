#include <kairo/comm/realtime_memory.hpp>

#include <atomic>
#include <cstdlib>
#include <new>

namespace {

struct GuardState {
    bool active = false;
    kairo::comm::RealtimeAllocationStats stats;
    kairo::comm::RealtimeAllocationViolationPolicy policy =
        kairo::comm::RealtimeAllocationViolationPolicy::RecordOnly;
};

thread_local GuardState guard_state;

} // namespace

namespace kairo::comm {

RealtimeAllocationGuard::RealtimeAllocationGuard(std::string_view component,
                                                 std::string_view phase,
                                                 RealtimeAllocationViolationPolicy policy,
                                                 bool enabled) noexcept {
#ifdef KAIRO_ENABLE_REALTIME_ALLOCATION_GUARD
    if (!enabled) {
        return;
    }
    previous_active_ = guard_state.active;
    previous_stats_ = guard_state.stats;
    previous_policy_ = guard_state.policy;
    guard_state.active = true;
    guard_state.stats.component = component;
    guard_state.stats.phase = phase;
    guard_state.policy = policy;
    active_ = true;
#else
    (void)component;
    (void)phase;
    (void)policy;
    (void)enabled;
#endif
}

RealtimeAllocationGuard::~RealtimeAllocationGuard() noexcept {
    if (active_) {
        const RealtimeAllocationStats completed_stats = guard_state.stats;
        guard_state.active = previous_active_;
        guard_state.policy = previous_policy_;
        if (previous_active_) {
            guard_state.stats = completed_stats;
            guard_state.stats.component = previous_stats_.component;
            guard_state.stats.phase = previous_stats_.phase;
        }
    }
}

bool RealtimeAllocationGuard::is_enabled() noexcept {
#ifdef KAIRO_ENABLE_REALTIME_ALLOCATION_GUARD
    return true;
#else
    return false;
#endif
}

RealtimeAllocationStats RealtimeAllocationGuard::current_thread_stats() noexcept {
    return guard_state.stats;
}

void RealtimeAllocationGuard::reset_current_thread_stats() noexcept {
    guard_state.stats = {};
}

} // namespace kairo::comm

#ifdef KAIRO_ENABLE_REALTIME_ALLOCATION_GUARD
namespace {
// CR-044: 对齐内存的平台分配/释放（MSVC 无 C11 aligned_alloc）。
// 必须位于 guard_alloc/guard_aligned_alloc 之前（复验曾抓到定义后置导致
// guard 宏开启时编译失败——本地默认构建不开宏，勿依赖其编译检查）。
#if defined(_MSC_VER)
inline void* platform_aligned_alloc(std::size_t alignment, std::size_t size) {
    return _aligned_malloc(size, alignment);
}
inline void platform_aligned_free(void* pointer) noexcept {
    _aligned_free(pointer);
}
#else
inline void* platform_aligned_alloc(std::size_t alignment, std::size_t size) {
    return std::aligned_alloc(alignment, size);
}
inline void platform_aligned_free(void* pointer) noexcept {
    std::free(pointer);
}
#endif

// CR-044: 统一记账入口。此前只替换了普通 operator new——库内大量
// alignas(64) 成员（BoundedQueue 节点、PhaseGate::Core 等）经对齐变体
// 分配时完全绕过 guard 计数，"实时路径无分配"的校验存在盲区。
inline void* guard_alloc(std::size_t size) {
    if (guard_state.active) {
        ++guard_state.stats.allocation_count;
        guard_state.stats.allocated_bytes += size;
        if (guard_state.policy == kairo::comm::RealtimeAllocationViolationPolicy::Abort) {
            std::abort();
        }
    }
    if (void* pointer = std::malloc(size)) {
        return pointer;
    }
    throw std::bad_alloc();
}

// 对齐分配：malloc 已保证 max_align_t 对齐，超过它的对齐请求走
// aligned_alloc（size 向上取整到 alignment，POSIX 要求）。
inline void* guard_aligned_alloc(std::size_t size, std::size_t alignment) {
    if (alignment <= alignof(std::max_align_t)) {
        return guard_alloc(size);
    }
    if (guard_state.active) {
        ++guard_state.stats.allocation_count;
        guard_state.stats.allocated_bytes += size;
        if (guard_state.policy == kairo::comm::RealtimeAllocationViolationPolicy::Abort) {
            std::abort();
        }
    }
    const std::size_t padded = (size + alignment - 1) / alignment * alignment;
    if (void* pointer = platform_aligned_alloc(alignment, padded ? padded : alignment)) {
        return pointer;
    }
    throw std::bad_alloc();
}

}  // namespace

void* operator new(std::size_t size) {
    return guard_alloc(size);
}

void* operator new[](std::size_t size) {
    return ::operator new(size);
}

// CR-044: C++17 对齐变体（alignas 超过 max_align_t 的类型经此分配）。
void* operator new(std::size_t size, std::align_val_t alignment) {
    return guard_aligned_alloc(size, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
    return ::operator new(size, alignment);
}

// nothrow 变体：与普通变体同一路径，失败返回 nullptr 而非抛出。
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    if (guard_state.active) {
        ++guard_state.stats.allocation_count;
        guard_state.stats.allocated_bytes += size;
        if (guard_state.policy == kairo::comm::RealtimeAllocationViolationPolicy::Abort) {
            std::abort();
        }
    }
    return std::malloc(size);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return ::operator new(size, std::nothrow);
}

void* operator new(std::size_t size, std::align_val_t alignment,
                   const std::nothrow_t&) noexcept {
    try {
        return ::operator new(size, alignment);
    } catch (...) {
        return nullptr;
    }
}

void* operator new[](std::size_t size, std::align_val_t alignment,
                     const std::nothrow_t&) noexcept {
    return ::operator new(size, alignment, std::nothrow);
}

void operator delete(void* pointer) noexcept {
    std::free(pointer);
}

void operator delete[](void* pointer) noexcept {
    std::free(pointer);
}

void operator delete(void* pointer, std::size_t) noexcept {
    std::free(pointer);
}

void operator delete[](void* pointer, std::size_t) noexcept {
    std::free(pointer);
}

// CR-044: 对齐删除变体——必须与对齐 new 配对，否则 aligned_alloc 的内存
// 经 free 释放是 UB（POSIX 要求 aligned_alloc 用 free 释放倒是允许的，
// 但 C++17 标准要求配对调用对齐 delete）。
void operator delete(void* pointer, std::align_val_t) noexcept {
    platform_aligned_free(pointer);
}

void operator delete[](void* pointer, std::align_val_t) noexcept {
    platform_aligned_free(pointer);
}

void operator delete(void* pointer, std::align_val_t, std::size_t) noexcept {
    platform_aligned_free(pointer);
}

void operator delete[](void* pointer, std::align_val_t, std::size_t) noexcept {
    platform_aligned_free(pointer);
}
#endif
