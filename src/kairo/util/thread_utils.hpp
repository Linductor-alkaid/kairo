#pragma once

#include <thread>
#include <vector>
#include <cstdint>
#include <string>

namespace kairo {
namespace util {

/**
 * @brief 设置线程优先级
 * 
 * @param handle 线程原生句柄
 * @param priority 优先级
 *                Linux: SCHED_FIFO优先级范围1-99，普通优先级范围-20到19
 *                Windows: THREAD_PRIORITY_* 常量（如THREAD_PRIORITY_NORMAL）
 * @return 成功返回true，失败返回false
 */
bool set_thread_priority(std::thread::native_handle_type handle, int priority);

/**
 * @brief 设置CPU亲和性
 * 
 * 将线程绑定到指定的CPU核心上。
 * 
 * @param handle 线程原生句柄
 * @param cpu_ids CPU核心ID列表（从0开始）
 * @return 成功返回true，失败返回false
 */
bool set_cpu_affinity(std::thread::native_handle_type handle,
                      const std::vector<int>& cpu_ids);

/**
 * @brief 获取当前线程的优先级
 * 
 * @return 当前线程的优先级值，失败返回0
 */
int get_current_thread_priority();

/**
 * @brief 获取当前线程的CPU亲和性
 * 
 * @return CPU核心ID列表，失败返回空列表
 */
std::vector<int> get_current_thread_affinity();

struct ProcessMemoryLockResult {
    bool applied = false;
    int error_code = 0;
};

/**
 * @brief 锁定进程当前和未来映射，防止分页到 swap
 *
 * Linux 上调用 mlockall(MCL_CURRENT|MCL_FUTURE)。这是进程级操作，不是当前
 * 线程操作；它还会锁定后续映射。需要 CAP_IPC_LOCK 或足够的 RLIMIT_MEMLOCK。
 *
 * @return 是否成功及失败时的 errno（Windows 上 error_code 为 ERROR_NOT_SUPPORTED）
 */
ProcessMemoryLockResult try_mlock_process_memory();

/**
 * @brief 进程级 mlockall/munlockall 的系统调用接缝（默认实现转发内核）
 *
 * 测试通过派生注入替身验证租约的引用计数行为，见
 * ProcessMemoryLockLease::set_syscalls_for_test。纯虚接口保证替身不会
 * 意外回落到真实系统调用。
 */
struct ProcessMemoryLockSyscalls {
    virtual ~ProcessMemoryLockSyscalls() = default;

    // 锁定当前与未来映射。error_code 在 Linux 上为 errno；Windows 平台
    // 不支持时为 ERROR_NOT_SUPPORTED。
    virtual ProcessMemoryLockResult mlockall_current_future() = 0;
    // 解除进程级锁定，语义与 munlockall(2) 一致；返回 0 成功。
    virtual int munlockall() = 0;
};

/** @brief ProcessMemoryLockSyscalls 的真实系统调用实现 */
struct RealProcessMemoryLockSyscalls : ProcessMemoryLockSyscalls {
    ProcessMemoryLockResult mlockall_current_future() override;
    int munlockall() override;
};

/**
 * @brief 进程级内存锁定的引用计数租约
 *
 * mlockall/munlockall 是进程级操作，但库内多个执行器（RealtimeThreadExecutor、
 * BlockingIoExecutor）可能同时启用：若各自直接调用，先停机的执行器要么永久
 * 留下锁定，要么 munlockall 解除仍运行执行器的锁定。本租约用互斥保护的引用
 * 计数管理库自身的 mlockall/munlockall：第一个租约获取时锁定，最后一个租约
 * 释放时解锁；中途释放单个租约不影响其他持有者。
 *
 * 共存策略：租约只追踪本库发起的锁定。若进程在库之外调用了 mlockall，最后
 * 一个租约释放时的 munlockall 同样会解除那些外部锁定（munlockall 无法区分
 * 调用者）；需要保留外部锁定的进程应在库的租约全部释放后重新调用 mlockall。
 *
 * 获取失败（权限不足、RLIMIT_MEMLOCK、平台不支持）不产生租约：返回空租约
 * 并通过 error_code() 保留错误码。
 */
class ProcessMemoryLockLease {
public:
    ProcessMemoryLockLease() = default;
    ProcessMemoryLockLease(ProcessMemoryLockLease&& other) noexcept;
    ProcessMemoryLockLease& operator=(ProcessMemoryLockLease&& other) noexcept;
    ProcessMemoryLockLease(const ProcessMemoryLockLease&) = delete;
    ProcessMemoryLockLease& operator=(const ProcessMemoryLockLease&) = delete;
    ~ProcessMemoryLockLease();

    /** @brief 是否持有进程级锁定（参与引用计数） */
    bool holds_lock() const { return held_; }

    /** @brief 获取失败时的错误码；成功持有或未尝试获取时为 0 */
    int error_code() const { return error_code_; }

    /**
     * @brief 获取一个进程级锁定租约
     *
     * 引用计数已大于 0 时直接追加引用（不重复 mlockall）；计数为 0 时调用
     * mlockall，失败则返回空租约。
     */
    static ProcessMemoryLockLease try_acquire();

    /**
     * @brief 注入替身系统调用（测试接缝）；传 nullptr 恢复真实实现
     *
     * 必须在没有存活租约时调用，否则替身调用会与真实租约共享引用计数。
     */
    static void set_syscalls_for_test(ProcessMemoryLockSyscalls* syscalls);

private:
    ProcessMemoryLockLease(bool held, int error_code)
        : held_(held), error_code_(error_code) {}

    void release();

    bool held_ = false;
    int error_code_ = 0;
};

/**
 * @brief 把当前线程名设进内核，便于 top/htop/perf 看到
 *
 * Linux 上线程名最长 15 字符（pthread_setname_np 限制），超出会自动截断。
 *
 * @param name 线程名
 */
void set_current_thread_name(const std::string& name);

/**
 * @brief 设置当前线程的 timer slack（纳秒）
 *
 * Linux 默认 timer slack 为 50us，会给定时唤醒带来额外抖动。设为 1 几乎无 slack。
 * Windows 上为空实现。
 *
 * @param slack_ns timer slack（纳秒）
 */
bool set_current_thread_timer_slack_ns(uint64_t slack_ns);

#ifdef _WIN32

/**
 * @brief Windows 处理器组亲和性的值类型（对应 Win32 GROUP_AFFINITY）
 *
 * 单个线程的亲和性只能用一个 (group, mask) 表达；mask 是组内 64 位掩码。
 */
struct ProcessorGroupAffinity {
    unsigned long long mask;  // KAFFINITY：组内 64 位 CPU 掩码
    unsigned short group;     // 处理器组编号
};

/**
 * @brief Windows 处理器组 Win32 API 的可注入包装（默认实现转发真实 API）
 *
 * 逻辑 CPU 编号按 group * 64 + 组内序号 扩展到 64 以上（多处理器组服务器）；
 * 跨组请求会被 set_cpu_affinity() 拒绝。test_thread_utils_processor_group.cpp
 * 通过派生替身模拟多处理器组。接口类型不依赖 windows.h 以便在头文件声明。
 */
struct ProcessorGroupApi {
    virtual ~ProcessorGroupApi() = default;

    virtual unsigned short get_active_processor_group_count() = 0;
    virtual unsigned long get_active_processor_count(unsigned short group) = 0;
    // 返回非 0 成功；affinity 为出参。
    virtual int get_thread_group_affinity(void* thread, ProcessorGroupAffinity* affinity) = 0;
    // 返回非 0 成功。
    virtual int set_thread_group_affinity(void* thread,
                                          const ProcessorGroupAffinity* affinity) = 0;
};

/** @brief 注入替身处理器组 API（测试接缝）；传 nullptr 恢复真实实现 */
void set_processor_group_api_for_test(ProcessorGroupApi* api);

#endif  // _WIN32

} // namespace util
} // namespace kairo
