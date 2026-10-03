#pragma once

#include "interfaces.hpp"
#include "blocking_io.hpp"
#include "config.hpp"
#include "types.hpp"
#include "task_options.hpp"
#include "lockfree_task_executor.hpp"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <map>
#include <condition_variable>
#include <shared_mutex>
#include <mutex>
#include <atomic>

namespace kairo {
namespace monitor { class StatisticsCollector; }

/**
 * @brief 执行器管理器
 * 
 * 统一管理线程池执行器和专用实时线程执行器
 * 支持单例模式和实例化模式
 * 
 * 生命周期管理：
 * - 所有执行器的生命周期由 ExecutorManager 统一管理
 * - 使用 RAII 模式，ExecutorManager 析构时自动释放所有执行器
 * - 单例模式：ExecutorManager 单例的生命周期与程序相同
 * - 实例化模式：每个 ExecutorManager 实例拥有独立的执行器，析构时自动释放
 */
class ExecutorManager {
public:
    /**
     * @brief 获取单例实例
     * @return ExecutorManager 单例引用
     */
    static ExecutorManager& instance();

    /**
     * @brief 构造函数（实例化模式）
     * 
     * 创建独立的 ExecutorManager 实例，用于资源隔离
     */
    ExecutorManager();

    /**
     * @brief 析构函数（RAII）
     * 
     * 自动关闭并释放所有执行器
     */
    ~ExecutorManager();

    // 禁止拷贝和赋值
    ExecutorManager(const ExecutorManager&) = delete;
    ExecutorManager& operator=(const ExecutorManager&) = delete;

    /**
     * @brief 初始化默认异步执行器（线程池）
     * 
     * @param config 执行器配置
     * @return 是否初始化成功
     */
    bool initialize_async_executor(const ExecutorConfig& config);

    /**
     * @brief 默认异步执行器是否已经初始化
     *
     * 不触发懒初始化，仅报告当前状态。
     */
    bool has_default_async_executor() const;

    /**
     * @brief 默认异步执行器是否已经 shutdown
     */
    bool is_default_async_shutdown() const;

    /**
     * @brief 获取 Manager 状态 epoch。
     *
     * epoch 只反映注册表和 Manager 生命周期边界变化，不因任务计数变化
     * 递增；用于低频 snapshot 的一致性校验，不是提交 reservation。
     */
    uint64_t get_state_epoch() const noexcept;

    /**
     * @brief 获取默认异步执行器（线程池）的非持有裸指针
     *
     * 仅供能自行与 manager shutdown 串行化的高级调用方使用。
     * 不会延长执行器生命周期；需要跨并发 shutdown 安全使用时，调用
     * get_default_async_executor_snapshot()。
     *
     * @return 异步执行器指针，如果未初始化则返回 nullptr
     */
    IAsyncExecutor* get_default_async_executor();

    /**
     * @brief 获取默认异步执行器的生命周期持有快照
     *
     * 返回的 shared_ptr 会在注册表移除后继续保持对象存活，但不会阻止
     * shutdown 对执行器请求停止。未初始化时会懒创建默认池（提交路径
     * 语义，见 README "submit_auto"）。
     */
    std::shared_ptr<IAsyncExecutor> get_default_async_executor_snapshot();

    /**
     * @brief 同上，但绝不创建默认池（CR-013）
     *
     * 供只读诊断（get_async_executor_status/get_snapshot）与 shutdown
     * 查询路径使用：未初始化时直接返回 nullptr，不产生建池副作用；
     * 懒创建失败后也不重试（失败被闩存，用户显式 initialize() 可重建）。
     */
    std::shared_ptr<IAsyncExecutor> get_default_async_executor_snapshot_no_create();

    /**
     * @brief 注册实时执行器
     * 
     * @param name 执行器名称
     * @param executor 执行器指针（所有权转移）
     * @return 是否注册成功（如果名称已存在则返回 false）
     */
    bool register_realtime_executor(const std::string& name,
                                   std::unique_ptr<IRealtimeExecutor> executor);

    /**
     * @brief 获取已注册实时执行器的非持有裸指针
     *
     * 不会延长执行器生命周期，不能与并发 shutdown 一起使用。需要安全
     * 持有时调用 get_realtime_executor_snapshot()。
     *
     * @param name 执行器名称
     * @return 实时执行器指针，如果不存在则返回 nullptr
     */
    IRealtimeExecutor* get_realtime_executor(const std::string& name);

    /**
     * @brief 获取实时执行器的生命周期持有快照
     *
     * 快照可安全跨注册表移除持有；它不改变 stop/shutdown 的状态语义。
     */
    std::shared_ptr<IRealtimeExecutor> get_realtime_executor_snapshot(const std::string& name) const;

    /**
     * @brief 创建实时执行器（便捷方法）
     * 
     * 注意：此方法仅创建执行器对象，不会自动注册
     * 需要调用 register_realtime_executor() 进行注册
     * 
     * @param name 执行器名称
     * @param config 实时线程配置
     * @return 执行器指针，如果创建失败则返回 nullptr
     */
    std::unique_ptr<IRealtimeExecutor> create_realtime_executor(
        const std::string& name,
        const RealtimeThreadConfig& config);

    /**
     * @brief 获取所有实时执行器名称
     * 
     * @return 实时执行器名称列表
     */
    std::vector<std::string> get_realtime_executor_names() const;

    /**
     * @brief 获取全部实时执行器的状态值拷贝。
     *
     * 注册表在读取期间以 shared_ptr 快照保护对象生命周期；调用方不会获得
     * 裸指针，也不会阻止并发 stop 改变后端状态。
     */
    std::map<std::string, RealtimeExecutorStatus>
    get_all_realtime_executor_statuses() const;

    bool register_lockfree_executor(const std::string& name,
                                    std::unique_ptr<LockFreeTaskExecutor> executor);
    /** 非持有裸指针；并发 shutdown 路径请使用 snapshot API。 */
    LockFreeTaskExecutor* get_lockfree_executor(const std::string& name);
    /** 生命周期持有快照；不阻止执行器被停止。 */
    std::shared_ptr<LockFreeTaskExecutor> get_lockfree_executor_snapshot(const std::string& name) const;
    std::vector<std::string> get_lockfree_executor_names() const;
    bool start_lockfree_executor(const std::string& name);
    void stop_lockfree_executor(const std::string& name);
    bool try_push_lockfree_task(const std::string& name, std::function<void()> task);

    bool register_blocking_io_executor(
        const std::string& name,
        std::unique_ptr<IBlockingIoExecutor> executor);

    /** 非持有裸指针；并发 shutdown 路径请使用 snapshot API。 */
    IBlockingIoExecutor* get_blocking_io_executor(const std::string& name);
    /** 生命周期持有快照；不阻止执行器被停止。 */
    std::shared_ptr<IBlockingIoExecutor> get_blocking_io_executor_snapshot(const std::string& name) const;

    void request_stop_blocking_io_executor(const std::string& name) noexcept;
    void stop_blocking_io_executor(const std::string& name);
    BlockingIoExecutorStatus get_blocking_io_executor_status(const std::string& name) const;

    std::unique_ptr<IBlockingIoExecutor> create_blocking_io_executor(
        const std::string& name,
        const BlockingIoConfig& config,
        std::unique_ptr<IBlockingIoWorker> worker);

    std::vector<std::string> get_blocking_io_executor_names() const;

    /** 获取全部 Blocking I/O 执行器的状态值拷贝。 */
    std::map<std::string, BlockingIoExecutorStatus>
    get_all_blocking_io_executor_statuses() const;

    /**
     * @brief 注册 GPU 执行器
     * 
     * @param name 执行器名称
     * @param executor 执行器指针（所有权转移）
     * @return 是否注册成功（如果名称已存在则返回 false）
     */
    bool register_gpu_executor(const std::string& name,
                               std::unique_ptr<IGpuExecutor> executor);

    /**
     * @brief 获取已注册 GPU 执行器的非持有裸指针
     *
     * 不会延长执行器生命周期，不能与并发 shutdown 一起使用。需要安全
     * 持有时调用 get_gpu_executor_snapshot()。
     *
     * @param name 执行器名称
     * @return GPU 执行器指针，如果不存在则返回 nullptr
     */
    IGpuExecutor* get_gpu_executor(const std::string& name);

    /**
     * @brief 获取 GPU 执行器的生命周期持有快照
     *
     * 快照可安全跨注册表移除持有；它不阻止 stop/shutdown 改变运行状态。
     */
    std::shared_ptr<IGpuExecutor> get_gpu_executor_snapshot(const std::string& name) const;

    /** Atomically checks the cross-backend name registry. */
    bool is_executor_name_registered(const std::string& name) const;

    /**
     * @brief 创建 GPU 执行器（便捷方法）
     * 
     * 注意：此方法仅创建执行器对象，不会自动注册
     * 需要调用 register_gpu_executor() 进行注册
     * 
     * @param config GPU 执行器配置
     * @return 执行器指针，如果创建失败则返回 nullptr
     */
    std::unique_ptr<IGpuExecutor> create_gpu_executor(
        const gpu::GpuExecutorConfig& config);

    /**
     * @brief 获取所有 GPU 执行器名称
     * 
     * @return GPU 执行器名称列表
     */
    std::vector<std::string> get_gpu_executor_names() const;

    /**
     * @brief 获取所有 GPU 执行器状态（用于监控查询）
     * 
     * @return 执行器名称到状态的映射
     */
    std::map<std::string, gpu::GpuExecutorStatus> get_all_gpu_executor_statuses() const;

    /**
     * @brief Advisory snapshots used for routing and diagnostics.
     *
     * These values are not a submission reservation; callers must still handle
     * concurrent shutdown and capacity changes in the concrete backend.
     */
    std::vector<ExecutorCapability> get_executor_capabilities() const;

    bool try_push_realtime_task(const std::string& name, std::function<void()> task);

    /**
     * @brief 关闭所有执行器
     * 
     * @param wait_for_tasks 是否等待任务完成（默认：true）
     */
    ShutdownResult shutdown(bool wait_for_tasks = true);

    /**
     * @brief 启用或禁用任务监控
     */
    void enable_monitoring(bool enable);

    /**
     * @brief 设置监控采样率
     * @param rate 采样率 (0.0-1.0)
     */
    void set_monitoring_sampling_rate(double rate);

    void set_in_flight_task_capacity(size_t capacity);
    void set_in_flight_task_sampling_rate(double rate);

    /**
     * @brief 按 task_type 获取任务统计
     */
    TaskStatistics get_task_statistics(const std::string& task_type) const;

    /**
     * @brief 获取全部 task_type 的任务统计
     */
    std::map<std::string, TaskStatistics> get_all_task_statistics() const;

    InFlightTaskDiagnostics get_in_flight_task_diagnostics() const;

    void record_in_flight_task_pending(const std::string& task_id,
                                        const std::string& task_type,
                                        const std::string& executor_name);
    void record_in_flight_task_state(const std::string& task_id,
                                     TaskLifecycleState state);
    void record_in_flight_task_terminal(const std::string& task_id);

private:
    void bump_state_epoch() noexcept;
    bool is_executor_name_registered_locked(const std::string& name) const;
    void finish_default_async_shutdown_drain();

    // Protects default_async_executor_, default_async_shutdown_ and
    // default_async_shutdown_in_progress_.
    mutable std::mutex default_async_mutex_;
    // 默认异步执行器（线程池）
    std::shared_ptr<IAsyncExecutor> default_async_executor_;

    // stop(false)/shutdown(false) 退休的默认执行器。句柄从
    // default_async_executor_ 撤下后池可能仍在 detached 终结线程上排空，
    // 而 worker 闭包捕获裸 facade 指针：后续 shutdown(true)（含
    // ~Executor → 本管理器析构）必须先对这些退休执行器做 stop(true)，
    // 等 worker 全部 join 后才允许析构 facade/manager 状态。
    // 由 default_async_mutex_ 保护。
    std::vector<std::shared_ptr<IAsyncExecutor>> retired_async_executors_;

    // 已关闭标记：shutdown 后不再懒初始化，get_default_async_executor() 直接返回 nullptr
    bool default_async_shutdown_ = false;
    // CR-013: 懒创建失败闩存——失败的 call_once 若抛出会允许无限重试，
    // 每次提交/诊断查询都重新走一遍建池并把 system_error 抛给调用方。
    bool default_init_failed_ = false;

    // P-260816-001: 默认执行器的排空（stop/wait_for_completion 含 worker join，
    // 可阻塞数秒）在 default_async_mutex_ 之外执行，避免池内任务再入
    // submit()/状态读路径时与 shutdown 互相等待形成自死锁。并发 shutdown
    // 调用者在 default_async_shutdown_cv_ 上等待排空完成，保持旧实现
    // "第二个调用者等第一个排空结束后才返回"的语义。
    bool default_async_shutdown_in_progress_ = false;
    std::condition_variable default_async_shutdown_cv_;

    // 懒初始化用：保证多线程首次调用 get_default_async_executor() 时只初始化一次
    std::once_flag default_init_once_;

    // 实时执行器注册表
    std::unordered_map<std::string, std::shared_ptr<IRealtimeExecutor>> realtime_executors_;

    // 读写锁（保护实时执行器注册表）
    mutable std::shared_mutex mutex_;

    std::unordered_map<std::string, std::shared_ptr<LockFreeTaskExecutor>> lockfree_executors_;
    mutable std::shared_mutex lockfree_mutex_;

    std::unordered_map<std::string, std::shared_ptr<IBlockingIoExecutor>> blocking_io_executors_;
    mutable std::shared_mutex blocking_io_mutex_;

    // GPU 执行器注册表
    std::unordered_map<std::string, std::shared_ptr<IGpuExecutor>> gpu_executors_;

    // 读写锁（保护 GPU 执行器注册表）
    mutable std::shared_mutex gpu_mutex_;

    // Serializes cross-backend name uniqueness checks and registrations.
    mutable std::mutex registration_mutex_;

    // Once shutdown starts, named registries are sealed against new entries.
    bool registries_shutdown_ = false;
    std::atomic<uint64_t> state_epoch_{0};

    // 统计收集器（任务监控）
    std::unique_ptr<monitor::StatisticsCollector> statistics_collector_;

    // 退出时自动关闭：atexit 回调（仅单例创建时注册，无参无返回、不抛异常）
    static void atexit_shutdown();

    // 单例实例（线程安全初始化）
    static ExecutorManager* instance_;
    static std::once_flag once_flag_;
};

} // namespace kairo
