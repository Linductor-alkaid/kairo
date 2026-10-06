#pragma once

#include "scheduling.hpp"
#include "task_cancellation.hpp"

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <string>
#include <vector>
#include <atomic>
#include <functional>
#include <future>
#include <map>
#include <optional>
#include <utility>

namespace kairo {

inline constexpr std::chrono::seconds kDefaultWaitForCompletionTimeout{300};

/**
 * @brief Result of a shutdown request.
 *
 * A worker must never wait for or join the pool that is executing it.  Such
 * calls only request shutdown; an external caller must complete finalization.
 */
enum class ShutdownResult {
    Completed,
    RequestedFromWorker
};

class TimedOutException : public std::runtime_error {
public:
    explicit TimedOutException(const std::string& message)
        : std::runtime_error(message) {}
};

class ExecutorStopping : public std::runtime_error {
public:
    explicit ExecutorStopping(const std::string& message)
        : std::runtime_error(message) {}
};

/**
 * @brief 总量有界 admission 拒绝异常
 *
 * `ExecutorConfig::max_in_flight_tasks` 耗尽时，被拒提交的 future 立即以该
 * 异常就绪；同时记录 `FailureKind::CapacityExhausted` failure 事件。
 */
class CapacityExhaustedException : public std::runtime_error {
public:
    explicit CapacityExhaustedException(const std::string& message)
        : std::runtime_error(message) {}
};

/**
 * @brief 任务优先级枚举
 */
enum class TaskPriority {
    LOW = 0,
    NORMAL = 1,
    HIGH = 2,
    CRITICAL = 3
};

/**
 * @brief Executor facade 可诊断 API 的错误码
 */
enum class ExecutorErrorCode {
    Ok,
    AlreadyInitialized,
    AlreadyShutdown,
    InvalidConfig,
    DuplicateName,
    NotFound,
    BackendUnavailable,
    StartFailed,
    PermissionDenied,
    Unknown
};

inline const char* executor_error_code_to_string(ExecutorErrorCode code) {
    switch (code) {
    case ExecutorErrorCode::Ok:
        return "Ok";
    case ExecutorErrorCode::AlreadyInitialized:
        return "AlreadyInitialized";
    case ExecutorErrorCode::AlreadyShutdown:
        return "AlreadyShutdown";
    case ExecutorErrorCode::InvalidConfig:
        return "InvalidConfig";
    case ExecutorErrorCode::DuplicateName:
        return "DuplicateName";
    case ExecutorErrorCode::NotFound:
        return "NotFound";
    case ExecutorErrorCode::BackendUnavailable:
        return "BackendUnavailable";
    case ExecutorErrorCode::StartFailed:
        return "StartFailed";
    case ExecutorErrorCode::PermissionDenied:
        return "PermissionDenied";
    case ExecutorErrorCode::Unknown:
        return "Unknown";
    default:
        return "Unknown";
    }
}

/**
 * @brief Executor facade 轻量结果类型
 */
struct ExecutorResult {
    bool ok = true;
    ExecutorErrorCode error_code = ExecutorErrorCode::Ok;
    std::string message;

    explicit operator bool() const noexcept {
        return ok;
    }

    static ExecutorResult success(std::string msg = {}) {
        ExecutorResult result;
        result.message = std::move(msg);
        return result;
    }

    static ExecutorResult failure(ExecutorErrorCode code, std::string msg) {
        ExecutorResult result;
        result.ok = false;
        result.error_code = code;
        result.message = std::move(msg);
        return result;
    }
};

/**
 * @brief Facade task graph handle.
 */
class TaskHandle {
public:
    TaskHandle() = default;

    explicit TaskHandle(std::string id) noexcept
        : id_(std::move(id)) {}

    const std::string& id() const noexcept {
        return id_;
    }

    bool valid() const noexcept {
        return !id_.empty();
    }

    explicit operator bool() const noexcept {
        return valid();
    }

private:
    std::string id_;
};

template <class T>
struct TaskSubmission {
    TaskHandle handle;
    std::future<T> future;
};

/**
 * @brief Executor facade 统一失败事件类型
 *
 * 自动调优类失败允许安全回退；任务运行、提交拒绝、超时等状态必须通过事件、
 * 状态计数或 future 保持可观察。
 */
enum class FailureKind {
    TaskException,     // 用户任务抛异常
    SubmitRejected,    // 任务提交被拒绝
    TaskTimeout,       // 任务执行超时
    RealtimeDrop,      // 实时队列丢任务/推送失败
    GpuFailure,        // GPU 执行失败
    WaitTimeout,       // 等待完成超时
    TuningFallback,    // 平台调优失败并安全回退
    CapacityExhausted, // 总量有界 admission 拒绝（max_in_flight_tasks 耗尽）
    DeadlineMissed,    // 任务开始执行时已超过声明的 deadline（仍会执行）
    None               // 0.6.1：无失败（SchedulingFeedback::success 的哨兵）
};

/**
 * @brief Executor facade 失败事件
 */
struct ExecutorFailureEvent {
    FailureKind kind = FailureKind::TaskException;
    std::string executor_name;
    std::string task_id;
    std::string message;
    std::exception_ptr exception;
    std::chrono::steady_clock::time_point timestamp =
        std::chrono::steady_clock::now();
};

/**
 * @brief Executor facade 累计失败状态
 */
struct ExecutorFailureStatus {
    uint64_t task_exception_count = 0;
    uint64_t submit_rejected_count = 0;
    uint64_t timeout_count = 0;
    uint64_t realtime_drop_count = 0;
    uint64_t gpu_failure_count = 0;
    uint64_t wait_timeout_count = 0;
    uint64_t tuning_fallback_count = 0;
    uint64_t capacity_exhausted_count = 0;  // max_in_flight_tasks 拒绝数
    uint64_t deadline_missed_count = 0;     // 开始执行时已错过声明 deadline 的任务数
    uint64_t total_count = 0;
};

/**
 * @brief Scheduling Runtime 累计调度指标（0.6.1）
 *
 * `Executor::get_scheduling_metrics()` 的快照类型。计数对象是**路由
 * 决策**而非任务：一次提交可能产生多条决策（如 GPU 提交异常后回退会
 * 追加一条 AcceptedDegraded 决策）。计数从 Executor 构造起单调累计，
 * 无需预先配置任何观测开关。
 *
 * 字段语义与 `RoutingDecision::status / reason / diagnostics` 一一对应：
 * - `accepted*`/`rejected*` 按 status 分类；
 * - `deadline_rejected` / `backend_unavailable_rejected` /
 *   `capacity_rejected` 按 reason 细分拒绝原因（resource_rejected 与
 *   backend_unavailable/capacity 存在交集：声明 ResourceRequirements
 *   触发的拒绝带 ResourceInfeasible 诊断位）；
 * - `affinity_mismatch` 统计诊断位，无论最终接受或降级；
 * - `deadline_missed` 与 `ExecutorFailureStatus::deadline_missed_count`
 *   同源（执行时错过，任务仍执行）。
 */
struct SchedulingMetrics {
    uint64_t accepted_count = 0;                  // status == Accepted
    uint64_t accepted_degraded_count = 0;         // status == AcceptedDegraded
    uint64_t rejected_count = 0;                  // status == Rejected
    uint64_t deadline_rejected_count = 0;         // reason == DeadlineExpired
    uint64_t backend_unavailable_rejected_count = 0;  // reason == BackendUnavailable
    uint64_t capacity_rejected_count = 0;         // reason == CapacityPressure
    uint64_t resource_rejected_count = 0;         // diagnostics 含 ResourceInfeasible
    uint64_t affinity_mismatch_count = 0;         // diagnostics 含 AffinityMismatch
    uint64_t deadline_missed_count = 0;           // 执行时已错过（不中断执行）
    uint64_t feedback_reported_count = 0;         // on_task_completed 反馈次数
};

/**
 * @brief Executor facade 等待/完成状态快照
 */
struct CompletionStatus {
    std::string executor_name = "default";
    bool is_initialized = false;
    bool is_running = false;
    bool is_idle = true;
    size_t active_tasks = 0;
    size_t queued_tasks = 0;
    size_t pending_tasks = 0;
    size_t completed_tasks = 0;
    size_t failed_tasks = 0;
};

/**
 * @brief Executor facade 周期任务状态
 */
struct PeriodicTaskStatus {
    std::string task_id;
    int64_t period_ms = 0;
    bool is_running = false;
    uint64_t execution_count = 0;
    uint64_t failed_count = 0;
    uint64_t consecutive_failure_count = 0;
    std::string last_error_message;
    std::chrono::steady_clock::time_point next_execute_time{};
    std::chrono::steady_clock::time_point last_failure_time{};
};

using ExecutorFailureCallback = std::function<void(const ExecutorFailureEvent&)>;

/**
 * @brief 生成全局唯一任务 ID（原子计数器实现）。
 *
 * facade 句柄、定时器 id 与线程池内部任务 id 共用同一计数器空间。
 */
std::string generate_task_id();

/**
 * @brief 任务结构体
 */
struct Task {
    std::string task_id;                          // 任务ID
    TaskPriority priority = TaskPriority::NORMAL; // 任务优先级
    std::function<void()> function;              // 任务函数
    std::function<void(std::exception_ptr)> on_timeout; // 软超时时显式满足 future
    int64_t submit_time_ns = 0;                   // 提交时间（纳秒）
    int64_t timeout_ms = 0;                       // 超时时间（毫秒），0表示不超时
    std::vector<std::string> dependencies;       // 依赖任务ID
    std::atomic<bool> cancelled{false};           // 取消标志

    // ---- 0.6.0 Scheduling Runtime：per-task 调度元数据 ----
    // （affinity/resource 消费在路由层，保留于 TaskOptions，不进 Task）
    QosClass qos = QosClass::Standard;            // 服务质量类别
    int64_t deadline_ns = 0;                      // 绝对 deadline（steady 纳秒）；0 = 无
};

/**
 * @brief 异步执行器状态结构
 */
struct AsyncExecutorStatus {
    std::string name;                             // 执行器名称
    bool is_running = false;                      // 是否运行中
    size_t active_tasks = 0;                      // 活跃任务数
    size_t completed_tasks = 0;                   // 已完成任务数
    size_t failed_tasks = 0;                      // 失败任务数
    size_t queue_size = 0;                        // 队列大小
    double avg_task_time_ms = 0.0;                // 平均任务执行时间（毫秒）
};

/**
 * @brief 实时执行器状态结构
 *
 * P-001 (260615): 新增 dropped_task_count / failed_pushes / peak_queue_size /
 * queue_capacity 字段，承载背压下静默丢任务的可见性。
 * - dropped_task_count:  push_task() 因 未运行、空任务、队列满 或 对象池耗尽
 *                       而被丢弃的累计数
 *                       (即使 enable_stats=false 也会累计, 是背压可见性的核心指标)
 * - failed_pushes:       仅 enable_stats=true 时由 LockFreeQueue 统计的所有底层入队失败数
 *                       （队列满、CAS 竞争或 reservation 取消；不等同于 dropped_task_count）
 * - peak_queue_size:     仅 enable_stats=true 时由 LockFreeQueue 统计的峰值队列长度
 * - queue_capacity:      RT 无锁队列的固定容量 (>= dropped 阈值), 用于比率分析
 */
struct RealtimeExecutorStatus {
    std::string name;                             // 执行器名称
    bool is_running = false;                      // 是否运行中
    int64_t cycle_period_ns = 0;                  // 周期（纳秒）
    int64_t cycle_count = 0;                      // 周期计数
    int64_t cycle_timeout_count = 0;              // 周期超时计数
    double avg_cycle_time_ns = 0.0;                // 平均周期执行时间（纳秒）
    double max_cycle_time_ns = 0.0;               // 最大周期执行时间（纳秒）
    bool priority_applied = false;                // 请求的实时调度/优先级是否成功应用；Android 通常为 false
    bool cpu_affinity_applied = false;            // 请求的 CPU 亲和性是否成功应用；Android 受 cgroup/SELinux 限制
    bool process_memory_lock_applied = false;     // 请求的进程级 mlockall 是否成功应用；Android 普通 App 通常为 false
    int process_memory_lock_errno = 0;            // mlockall 失败时的 errno；未请求或成功时为 0
    bool timer_slack_applied = false;             // 请求的 timer slack 是否成功应用；Android 厂商内核可能忽略
    // P-001 (260615): 背压可见性字段
    uint64_t dropped_task_count = 0;              // 累计丢任务数 (未运行+空任务+队列满+池耗尽, 始终累计)
    uint64_t failed_pushes = 0;                   // LockFreeQueue 所有底层失败入队数 (仅 enable_stats=true)
    uint64_t peak_queue_size = 0;                 // 队列峰值 (仅 enable_stats=true)
    uint64_t queue_capacity = 0;                  // 队列固定容量
    uint64_t rejected_not_running_count = 0;      // 未运行/已停止时拒绝的累计数
    uint64_t rejected_empty_task_count = 0;       // 空任务拒绝累计数
    uint64_t pool_exhausted_count = 0;            // 对象池耗尽拒绝累计数
    uint64_t queue_full_count = 0;                // 队列满拒绝累计数
    uint64_t cycle_manager_error_count = 0;       // 外部周期管理器调用异常累计数
};

enum class BlockingIoStopReason {
    None,
    Requested,
    WorkerReturned,
    WorkerException,
    StartFailed
};

/**
 * @brief 专属阻塞 I/O worker 的生命周期状态。
 *
 * transport 的收包、解码、背压与延迟统计仍属于对应 adapter 和
 * kairo::comm::CommStats；本结构只描述 executor 能可靠拥有的线程状态。
 */
struct BlockingIoExecutorStatus {
    std::string name;
    bool is_running = false;
    bool stop_requested = false;
    bool ready = false;
    bool cpu_affinity_applied = false;
    bool memory_locked = false;
    uint64_t wakeup_count = 0;
    BlockingIoStopReason stop_reason = BlockingIoStopReason::None;
    std::string last_error_message;
};

/**
 * @brief 任务统计信息（用于监控）
 */
struct TaskStatistics {
    int64_t total_count = 0;                     // 总任务数
    int64_t success_count = 0;                    // 成功任务数
    int64_t fail_count = 0;                       // 失败任务数
    int64_t timeout_count = 0;                    // 超时任务数
    int64_t total_execution_time_ns = 0;          // 总执行时间（纳秒）
    int64_t max_execution_time_ns = 0;            // 最大执行时间（纳秒）
    int64_t min_execution_time_ns = 0;            // 最小执行时间（纳秒）
};

/**
 * @brief A sampled task's lifecycle state for bounded diagnostics.
 *
 * This is diagnostic state, not a future state machine. In particular, a
 * soft timeout is observed when the worker skips an expired queued task.
 */
enum class TaskLifecycleState {
    Pending,
    Queued,
    Running,
    Succeeded,
    Failed,
    TimedOut,
    Rejected,
    Cancelled,
    DependencyBlocked
};

/**
 * @brief A value copy of one sampled in-flight task.
 *
 * No callable, payload, exception object, or dependency list is retained.
 */
struct TaskLifecycleSnapshot {
    std::string task_id;
    std::string task_type;
    std::string executor_name;
    TaskLifecycleState state = TaskLifecycleState::Pending;
    std::chrono::steady_clock::time_point submitted_at{};
    std::chrono::steady_clock::time_point state_changed_at{};
};

/** @brief Bounded in-flight diagnostic data collected by TaskMonitor. */
struct InFlightTaskDiagnostics {
    size_t count = 0;
    std::map<TaskLifecycleState, size_t> state_counts;
    std::chrono::nanoseconds oldest_age{0};
    size_t dropped_count = 0;
    bool incomplete = false;
    std::vector<TaskLifecycleSnapshot> tasks;
};

/**
 * @brief Executor 的整体生命周期摘要。
 *
 * 该状态只用于诊断，不作为任务提交的 reservation，也不替代各后端的
 * is_running、stop_requested 或 stop_reason 状态。
 */
enum class ExecutorLifecycleState {
    Created,      // 已创建 / Created: 默认异步后端尚未初始化
    Initializing, // 初始化中 / Initializing: 一个或多个后端正在创建或启动
    Running,      // 运行中 / Running: 至少一个可用后端正在运行
    Draining,     // 排空中 / Draining: 已请求停止，正在完成已接受的工作
    Stopped,      // 已停止 / Stopped: Manager 拥有的全部后端均已停止
    Failed        // 失败 / Failed: 初始化或关键生命周期操作失败
};

/**
 * @brief 周期统计信息（用于ICycleManager）
 */
struct CycleStatistics {
    std::string name;                             // 周期任务名称
    int64_t period_ns = 0;                        // 周期（纳秒）
    int64_t cycle_count = 0;                      // 周期计数
    int64_t timeout_count = 0;                    // 超时计数
    double avg_cycle_time_ns = 0.0;               // 平均周期执行时间（纳秒）
    double max_cycle_time_ns = 0.0;               // 最大周期执行时间（纳秒）
    bool is_running = false;                      // 是否运行中
};

/**
 * @brief GPU 相关类型定义
 */
namespace gpu {

/**
 * @brief GPU 后端类型
 */
enum class GpuBackend {
    CUDA,      // NVIDIA CUDA
    OPENCL,    // OpenCL (跨平台)
    SYCL,      // SYCL (Intel/AMD)
    HIP        // AMD ROCm
};

/**
 * @brief GPU 设备信息
 */
struct GpuDeviceInfo {
    std::string name;                             // 设备名称
    GpuBackend backend;                            // 后端类型
    int device_id = 0;                             // 设备ID
    std::string vendor;                            // 厂商（NVIDIA/AMD/Intel）
    size_t total_memory_bytes = 0;                // 总内存（字节）
    size_t free_memory_bytes = 0;                 // 空闲内存（字节）
    int compute_capability_major = 0;              // 计算能力主版本（CUDA）
    int compute_capability_minor = 0;              // 计算能力次版本（CUDA）
    size_t max_threads_per_block = 0;              // 每个Block最大线程数
    size_t max_blocks_per_grid[3] = {0, 0, 0};    // Grid最大维度
};

/**
 * @brief GPU 执行器状态
 */
struct GpuExecutorStatus {
    std::string name;                             // 执行器名称
    bool is_running = false;                      // 是否运行中
    GpuBackend backend;                            // 后端类型
    int device_id = 0;                             // 设备ID
    size_t active_kernels = 0;                    // 活跃kernel数
    size_t completed_kernels = 0;                 // 已完成kernel数
    size_t failed_kernels = 0;                    // 失败kernel数
    size_t queue_size = 0;                        // 任务队列大小
    size_t queue_capacity = 0;                    // 队列硬容量，0表示未知/不适用
    double avg_kernel_time_ms = 0.0;              // 平均kernel执行时间（毫秒）
    size_t memory_used_bytes = 0;                  // 已使用内存（字节）
    size_t memory_total_bytes = 0;                 // 总内存（字节）
    double memory_usage_percent = 0.0;            // 内存使用率
    std::string last_error_message;                // 最近一次启动/运行失败原因（空表示无错误）
};

/**
 * @brief GPU 任务配置
 */
struct GpuTaskConfig {
    size_t grid_size[3] = {1, 1, 1};              // Grid维度
    size_t block_size[3] = {1, 1, 1};            // Block维度
    size_t shared_memory_bytes = 0;               // 共享内存大小（字节）
    int stream_id = 0;                            // 流ID（0表示默认流）
    bool async = true;                            // 是否异步执行
    int priority = 1;                             // 优先级（0=LOW, 1=NORMAL, 2=HIGH, 3=CRITICAL），与 CPU submit_priority 对齐
};

} // namespace gpu

/**
 * @brief Executor facade 定时任务计数快照（独立于 failure 体系）。
 *
 * pending_count 为当前 Scheduled 的 delayed/periodic timer 数；
 * executed_count 为已派发的一次性 timer 与已发出的周期 tick 数；
 * cancelled_count 为显式取消与 shutdown 清理的 timer 数。
 */
struct TimerStatusSummary {
    uint64_t pending_count = 0;
    uint64_t executed_count = 0;
    uint64_t cancelled_count = 0;
};

/**
 * @brief Executor 的低频诊断快照。
 *
 * 每个字段由其 provider 在各自的同步域内复制；跨 provider 的组合为
 * best-effort，不承诺事务级一致性。captured_at 固定为采集开始时间。
 * 此接口不包含在途任务明细、任务 callable、业务 payload 或通信 payload，
 * 也不应在实时周期或任务热路径中调用。
 */
struct ExecutorSnapshot {
    uint32_t schema_version = 3;                  // 快照 schema 版本
    uint64_t snapshot_sequence = 0;               // 同一 Monitor 内严格单调递增
    uint64_t state_epoch = 0;                     // 最后观测到的 Manager 状态 epoch
    std::chrono::steady_clock::time_point captured_at{}; // 采集开始时间
    std::chrono::nanoseconds collection_duration{0}; // 采集耗时（纳秒）
    ExecutorLifecycleState lifecycle = ExecutorLifecycleState::Created;
    bool partial = false;                         // 任一 provider 不可用、移除或采集失败
    std::string consistency_note;                 // partial 的原因或一致性说明

    CompletionStatus completion;
    AsyncExecutorStatus async;                    // 默认异步后端；未初始化时为默认值
    std::map<std::string, RealtimeExecutorStatus> realtime;
    std::map<std::string, BlockingIoExecutorStatus> blocking_io;
    std::map<std::string, gpu::GpuExecutorStatus> gpu;

    ExecutorFailureStatus failures;
    std::vector<ExecutorFailureEvent> recent_failures;
    std::map<std::string, TaskStatistics> task_statistics;

    // 取消与定时生命周期计数：独立于 failure 体系，不并入
    // ExecutorFailureStatus::total_count（schema 3 新增）。
    CancellationStatus cancellation;
    TimerStatusSummary timers;

    // Bounded, sampled diagnostics. A full table means some sampled tasks
    // were omitted; aggregate statistics remain independent and complete.
    size_t in_flight_count = 0;
    std::map<TaskLifecycleState, size_t> in_flight_state_counts;
    std::chrono::nanoseconds oldest_in_flight_age{0};
    size_t in_flight_dropped_count = 0;
    bool in_flight_diagnostics_incomplete = false;
    std::vector<TaskLifecycleSnapshot> in_flight_tasks;

    size_t running_backend_count = 0;
    size_t stopping_backend_count = 0;
    size_t active_task_count = 0;
    size_t queued_task_count = 0;
    size_t failed_task_count = 0;
    size_t dropped_work_count = 0;
};

/**
 * @brief Executor facade 等待结果。
 *
 * timed_out 为 true 时，diagnostic_snapshot 保存同一次超时路径采集的完整
 * 生命周期现场；正常完成和未初始化等待时为空。
 */
struct WaitResult {
    bool completed = true;
    bool timed_out = false;
    std::chrono::milliseconds timeout{0};
    CompletionStatus status;
    std::string message;
    std::optional<ExecutorSnapshot> diagnostic_snapshot;

    explicit operator bool() const noexcept {
        return completed;
    }
};

/**
 * @brief 低频生命周期诊断快照回调。
 *
 * 回调在触发诊断的 facade 调用线程中执行；实现会隔离回调异常。回调不得
 * 调用实时周期代码，也不应把快照查询或格式化放入任务热路径。
 */
using ExecutorSnapshotCallback = std::function<void(const ExecutorSnapshot&)>;

} // namespace kairo
