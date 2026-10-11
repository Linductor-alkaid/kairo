#pragma once

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <string>
#include <vector>
#include <functional>
#include "types.hpp"

namespace kairo {

// 前向声明
class ICycleManager;

/**
 * @brief 线程池配置结构
 */
struct ThreadPoolConfig {
    size_t min_threads = 0;              // 0 = 自适应 sentinel, ExecutorManager::initialize 时按 hw_concurrency 计算 (min 2)
    size_t max_threads = 0;              // 0 = 自适应 sentinel, ExecutorManager::initialize 时按 hw_concurrency 计算 (默认 hw)
    size_t queue_capacity = 1000;        // 任务队列容量
    int thread_priority = 0;             // 线程优先级（-20到19，Linux；Windows使用SetThreadPriority；Android best-effort）
    std::vector<int> cpu_affinity;       // CPU亲和性（绑定到特定核心）, 空 = auto-allocate [0..hw-1]；Android 使用 cgroup 允许 cpuset
    int64_t task_timeout_ms = 0;         // 任务超时时间（毫秒），0表示不超时
    bool enable_work_stealing = true;    // 默认开, 无锁工作窃取 -10.7% 退化; max_threads==1 时自动关
    // CR-024（v0.7.0 M0）：可选防饿死 aging（默认关闭，语义与 0.6.x 一致）。
    // 开启后堆顶任务每等待 priority_aging_interval_ns 提升一级有效优先级
    // （至多 CRITICAL），dequeue 时跨级竞争。见 PriorityScheduler 注释。
    bool enable_priority_aging = false;
    int64_t priority_aging_interval_ns = 100'000'000;  // 每提升一级的最短等待，默认 100ms
};

/**
 * @brief 实时线程配置结构
 */
struct RealtimeThreadConfig {
    std::string thread_name;                              // 线程名称
    int64_t cycle_period_ns = 0;                          // 周期（纳秒），如2000000表示2ms
    int thread_priority = 0;                              // 线程优先级（SCHED_FIFO: 1-99，Linux；Android 默认不自动提升，显式设值仍 best-effort）
    std::vector<int> cpu_affinity;                        // CPU亲和性 (空 = 自适应 sentinel: 在允许 cpuset 内 round-robin; 显式设值尊重覆盖; Android 可能受 cgroup/SELinux 限制)
    std::function<void()> cycle_callback;                 // 周期回调函数
    ICycleManager* cycle_manager = nullptr;               // 可选的周期管理器接口（用于更精确的周期控制）
    bool enable_process_memory_lock = false;              // 默认关闭；Linux mlockall 是进程级操作；Android 普通 App 通常无权限，失败仅记录状态
    uint64_t timer_slack_ns = 1;                          // 默认 1ns, 几乎消除 50us 内核 timer slack; 显式设 0 表示保留内核默认
    // P-260618-002: 单周期任务预算. process_tasks() 每周期最多处理这么多个任务,
    // 防止生产速率短暂超过消费速率时单周期一口气耗尽整条队列, 打破"周期确定性"契约
    // (cycle_time 爆涨 / cycle_timeout_count 尖刺). 剩余任务自然滚到后续周期处理
    // (MPSC 无锁队列, 无需额外锁). 0 = 不限 (保留旧行为, 向后兼容); 默认 64.
    uint64_t max_tasks_per_cycle = 64;
    // Linux Debug allocation diagnostics. This is opt-in and only takes effect
    // when KAIRO_ENABLE_REALTIME_ALLOCATION_GUARD is enabled at build time.
    bool enable_allocation_guard = false;
};

/**
 * @brief 专属阻塞 I/O worker 配置
 *
 * 用于 LCM、socket、串口或 CAN 等可中断等待循环。该 worker 默认使用普通
 * OS 调度，不会自动申请 SCHED_FIFO；阻塞等待必须由 worker::wakeup() 解除。
 */
struct BlockingIoConfig {
    std::string thread_name;
    std::vector<int> cpu_affinity;                // 空 = 由 OS 自由调度
    bool enable_memory_lock = false;              // 默认关闭，避免 I/O worker 误占锁定内存
    std::chrono::milliseconds startup_timeout{1000}; // 0 = 不等待 ready，正值为启动上限
};

/**
 * @brief 统一执行器配置结构
 * 
 * 用于 Executor::initialize() 和 ExecutorManager::initialize_async_executor()
 * 可内嵌或映射为 ThreadPoolConfig
 */
struct ExecutorConfig {
    size_t min_threads = 0;              // 0 = 自适应 sentinel, ExecutorManager::initialize 时按 hw_concurrency 计算 (min 2)
    size_t max_threads = 0;              // 0 = 自适应 sentinel, ExecutorManager::initialize 时按 hw_concurrency 计算 (默认 hw)
    size_t queue_capacity = 1000;        // 任务队列容量
    int thread_priority = 0;              // 线程优先级
    std::vector<int> cpu_affinity;       // CPU亲和性, 空 = auto-allocate [0..hw-1]
    int64_t task_timeout_ms = 0;         // 任务超时时间（毫秒）
    bool enable_work_stealing = true;    // 默认开, 无锁工作窃取 -10.7% 退化; max_threads==1 时自动关
    bool enable_monitoring = true;       // 启用任务监控（默认开启）
    // 已完成 TaskHandle 的有界保留数；0 表示终态 handle 不保留。
    size_t task_graph_retention_capacity = 1024;
    // 总量有界 admission：facade 已接纳未结算的默认异步提交上限（scheduler
    // 全局队列 + worker 本地队列 + 执行中）。0 = 不启用（默认，零热路径开销）。
    // 注意与 queue_capacity（每 worker 本地队列构造参数）语义无关。
    size_t max_in_flight_tasks = 0;
    // CR-024（v0.7.0 M0）：可选防饿死 aging，透传给默认池的
    // PriorityScheduler（默认关闭）。语义见 ThreadPoolConfig 同名字段。
    bool enable_priority_aging = false;
    int64_t priority_aging_interval_ns = 100'000'000;
};

/**
 * @brief 线程池状态结构（用于监控）
 */
struct ThreadPoolStatus {
    size_t total_threads = 0;            // 总线程数
    size_t active_threads = 0;            // 活跃线程数
    size_t idle_threads = 0;             // 空闲线程数
    size_t queue_size = 0;               // 队列中任务数
    size_t total_tasks = 0;               // 总任务数
    size_t completed_tasks = 0;          // 已完成任务数
    size_t failed_tasks = 0;             // 失败任务数
    double avg_task_time_ms = 0.0;       // 平均任务执行时间（毫秒）
    double cpu_usage_percent = 0.0;       // CPU使用率
};

/**
 * @brief GPU 相关配置
 */
namespace gpu {

/**
 * @brief GPU 执行器配置
 */
struct GpuExecutorConfig {
    std::string name;                             // 执行器名称
    GpuBackend backend = GpuBackend::CUDA;        // GPU后端类型
    int device_id = 0;                            // GPU设备ID
    size_t max_queue_size = 1000;                 // 最大任务队列大小
    size_t memory_pool_size = 0;                 // 内存池大小（0表示不使用内存池）
    int default_stream_count = 1;                // 默认流数量
    bool enable_unified_memory = false;          // 是否启用统一内存（Unified Memory）
    bool enable_monitoring = true;                // 启用监控（默认开启）
};

/**
 * @brief 验证 GPU 执行器配置
 * 
 * @param config GPU 执行器配置
 * @return 是否有效
 */
inline std::string gpu_config_validation_error(const GpuExecutorConfig& config) {
    if (config.name.empty()) {
        return "GPU executor name must not be empty";
    }
    if (config.max_queue_size == 0) {
        return "GPU executor max_queue_size must be > 0";
    }
    if (config.device_id < 0) {
        return "GPU executor device_id must be >= 0";
    }
    if (config.default_stream_count < 1) {
        return "GPU executor default_stream_count must be >= 1";
    }
    return {};
}

inline bool validate_gpu_config(const GpuExecutorConfig& config) {
    return gpu_config_validation_error(config).empty();
}

} // namespace gpu

} // namespace kairo
