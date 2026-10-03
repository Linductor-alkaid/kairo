#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <functional>
#include <queue>
#include <atomic>
#include <chrono>

namespace kairo {
namespace gpu {

/**
 * @brief 任务节点（依赖图中的节点）
 */
struct GpuTaskNode {
    std::string task_id;
    std::function<void(void*)> kernel_func;
    int priority = 1;                          ///< 0=LOW, 1=NORMAL, 2=HIGH, 3=CRITICAL
    int stream_id = 0;
    int target_device = -1;                    ///< 目标设备，-1 表示自动选择
    std::vector<std::string> dependencies;     ///< 依赖的 task_id 列表
    size_t estimated_cost = 0;                 ///< 估计执行开销（用于负载均衡）
};

/**
 * @brief 设备负载信息
 */
struct DeviceLoad {
    int device_id = 0;
    size_t pending_tasks = 0;                  ///< 排队任务数
    size_t estimated_total_cost = 0;           ///< 估计总开销
    double utilization = 0.0;                  ///< 利用率 (0.0-1.0)
};

/**
 * @brief 调度统计
 */
struct TaskSchedulingStats {
    size_t total_tasks_scheduled = 0;
    size_t priority_promotions = 0;            ///< 优先级提升次数
    size_t dependency_waits = 0;               ///< 依赖等待次数
    size_t load_balance_moves = 0;             ///< 负载均衡迁移次数
    double avg_wait_time_us = 0.0;             ///< 平均等待时间
};

/**
 * @brief GPU 任务调度优化器
 *
 * 提供三项优化：
 * 1. 优先级调度 — 高优先级任务优先执行，支持优先级继承（被高优先级依赖的任务提升优先级）
 * 2. 依赖图优化 — 拓扑排序解析任务依赖，最大化并行度
 * 3. 负载均衡 — 多 GPU 场景下根据设备负载自动分配任务
 */
class TaskSchedulerOptimizer {
public:
    struct Config {
        bool enable_priority_inheritance = true;   ///< 启用优先级继承
        bool enable_load_balancing = true;         ///< 启用负载均衡
        size_t max_pending_tasks = 1024;           ///< 最大待调度任务数
    };

    TaskSchedulerOptimizer();
    explicit TaskSchedulerOptimizer(const Config& config);

    // --- 任务提交 ---

    /// 添加任务到调度图
    bool add_task(const GpuTaskNode& task);

    /// 移除任务（仅未调度的）
    bool remove_task(const std::string& task_id);

    /// 标记任务已完成（解除下游依赖）
    void mark_completed(const std::string& task_id);

    /// 当前待调度任务数
    size_t pending_count() const;

    // --- 依赖图优化 ---

    /// 获取当前可执行的任务列表（所有依赖已满足），按优先级排序
    std::vector<GpuTaskNode> get_ready_tasks();

    /// 检测依赖图是否有环
    bool has_cycle() const;

    // --- 负载均衡 ---

    /// 更新设备负载信息
    void update_device_load(const DeviceLoad& load);

    /// 为任务选择最优设备（target_device == -1 时调用）
    int select_best_device(const GpuTaskNode& task) const;

    /// 获取所有设备负载
    std::vector<DeviceLoad> get_device_loads() const;

    // --- 统计 ---

    TaskSchedulingStats get_stats() const;
    void reset_stats();

    Config get_config() const;
    void update_config(const Config& config);

private:
    /// 优先级继承：提升被高优先级任务依赖的低优先级任务
    void apply_priority_inheritance();

    /// 拓扑排序辅助：DFS 检测环
    bool dfs_has_cycle(const std::string& node,
                       std::unordered_set<std::string>& visiting,
                       std::unordered_set<std::string>& visited) const;

    Config config_;

    // CR-062: 摘除单个节点（图 + 反向边 + completed 回收）。要求已持
    // graph_mutex_ 写锁。
    void erase_node_unlocked(const std::string& task_id);

    mutable std::shared_mutex graph_mutex_;
    std::unordered_map<std::string, GpuTaskNode> task_graph_;
    std::unordered_set<std::string> completed_tasks_;
    // CR-062: 反向依赖索引（依赖 -> 依赖它的图内任务）。用于
    // remove_task 的级联移除与 completed_tasks_ 的及时回收（此前
    // completed_tasks_ 只增不减，长运行进程内存无界增长）。
    std::unordered_map<std::string, std::vector<std::string>> dependents_;

    mutable std::mutex device_mutex_;
    std::unordered_map<int, DeviceLoad> device_loads_;

    mutable std::mutex stats_mutex_;
    mutable size_t total_scheduled_ = 0;
    mutable size_t priority_promotions_ = 0;
    mutable size_t dependency_waits_ = 0;
    mutable size_t load_balance_moves_ = 0;
    mutable double total_wait_us_ = 0.0;
};

} // namespace gpu
} // namespace kairo
