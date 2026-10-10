#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <shared_mutex>

namespace kairo {

/**
 * @brief 任务依赖管理器
 *
 * 用于管理任务之间的依赖关系，支持：
 * - 注册任务依赖
 * - 检查任务是否可执行（所有依赖已完成）
 * - 标记任务完成
 * - 检测循环依赖
 *
 * 线程安全：使用 shared_mutex 实现读写锁，支持并发读取
 *
 * @note CR-052（v0.7.0 M0）：环检测改为 Pearce-Kelly 风格的增量拓扑序。
 * 每个节点持有拓扑序号 ord，边 u→v（u 依赖 v）携带约束 ord[v] < ord[u]。
 * add_dependency 快路径（ord[v] < ord[u] 已满足）O(1) 接受，不再做全图
 * DFS——此前每条边一次锁内全图遍历使链式建链退化为 O(n²)（n=4000 约
 * 3.9s）。违序时对受影响闭包做受限重排，环检测并入同一次遍历；全部
 * 遍历为显式栈迭代（Phase 1 已消除的栈溢出面不回归）。反向边表
 * dependents_ 与 dependencies_ 同步维护。已知边界：与插入序完全相反的
 * 建链形态每次触发重排，代价为已建尾部闭包的线性访问（纯内存遍历）；
 * 按依赖就绪序建链的真实形态全部走快路径。
 */
class TaskDependencyManager {
public:
    /**
     * @brief 构造函数
     */
    TaskDependencyManager() = default;

    /**
     * @brief 析构函数
     */
    ~TaskDependencyManager() = default;

    // 禁止拷贝和移动
    TaskDependencyManager(const TaskDependencyManager&) = delete;
    TaskDependencyManager& operator=(const TaskDependencyManager&) = delete;
    TaskDependencyManager(TaskDependencyManager&&) = delete;
    TaskDependencyManager& operator=(TaskDependencyManager&&) = delete;

    /**
     * @brief 注册任务依赖
     * 
     * 将 task_id 标记为依赖于 depends_on。
     * 如果检测到循环依赖，返回 false。
     * 
     * @param task_id 任务ID
     * @param depends_on 依赖的任务ID
     * @return 是否成功添加依赖（如果存在循环依赖则返回 false）
     */
    bool add_dependency(const std::string& task_id, 
                       const std::string& depends_on);

    /**
     * @brief 检查任务是否可执行（所有依赖已完成）
     * 
     * @param task_id 任务ID
     * @return 如果任务没有依赖或所有依赖已完成，返回 true；否则返回 false
     */
    bool is_ready(const std::string& task_id) const;

    /**
     * @brief 标记任务完成
     *
     * @param task_id 任务ID
     */
    void mark_completed(const std::string& task_id);

    /**
     * @brief 清除所有依赖关系和完成状态
     */
    void clear();

    /**
     * @brief 裁剪单个任务的所有状态(从 dependencies_ 与 completed_tasks_ 中移除)
     *
     * P-260623-002: 长生命周期服务(如常驻实时线程池)中,任务完成/失败后
     * dependencies_ 与 completed_tasks_ 默认永不回收,会无限增长。
     * 调用方在确认 task_id 不再被任何其它任务依赖时可调用本接口主动回收。
     *
     * @param task_id 任务ID(空字符串无操作)
     * @return 实际移除了状态的条数
     */
    size_t prune(const std::string& task_id);

    /**
     * @brief 移除 task_id 对 depends_on 的单条依赖边
     *
     * 若不存在此边则 no-op。P-260623-002 配套接口。
     *
     * @param task_id 任务ID
     * @param depends_on 被依赖的任务ID
     * @return 是否实际移除了边
     */
    bool remove_dependency(const std::string& task_id, const std::string& depends_on);

    /**
     * @brief 内部统计信息(用于监控/测试)
     */
    struct Stats {
        size_t task_count;       // dependencies_ 中的 task 数
        size_t completed_count;  // completed_tasks_ 中的 task 数
        size_t edge_count;       // 所有 task 依赖边的总数
    };
    Stats get_stats() const;

    /**
     * @brief 获取任务的依赖列表
     * 
     * @param task_id 任务ID
     * @return 依赖任务ID列表
     */
    std::vector<std::string> get_dependencies(const std::string& task_id) const;

    /**
     * @brief 检查任务是否已完成
     * 
     * @param task_id 任务ID
     * @return 如果任务已完成，返回 true
     */
    bool is_completed(const std::string& task_id) const;

private:
    /// Pearce-Kelly 受限重排：加约束 v→u（v 先于 u）而 ord[v] > ord[u] 时，
    /// 从 u 沿反向边收 ord ≤ ord[v] 的后代（RB，途中遇 v 即成环），从 v 沿
    /// 正向边收 ord ≥ ord[u] 的祖先（RA），RB∪RA 按原序连续重编号。
    /// 返回 false 表示成环（不加边）。
    bool rebuild_order_for(const std::string& u, const std::string& v);

    /// 确保 ord_ 中存在节点序号（首次出现时分配递增序号）。
    void ensure_ord(const std::string& id);

    // 依赖关系图：task_id -> [depends_on_1, depends_on_2, ...]
    std::unordered_map<std::string, std::vector<std::string>> dependencies_;

    // CR-052：反向边表 task_id -> [依赖 task_id 的任务]，供违序重排沿
    // "谁依赖我"方向遍历；与 dependencies_ 在 add/remove/prune/clear 中
    // 同步维护。
    std::unordered_map<std::string, std::vector<std::string>> dependents_;

    // CR-052：增量拓扑序号（被依赖者 < 依赖者）。单调递增分配；违序时
    // 受影响闭包在原序区间内重编号，计数器不回退。
    std::unordered_map<std::string, int64_t> ord_;
    int64_t next_ord_ = 0;

    // 已完成的任务集合
    std::unordered_set<std::string> completed_tasks_;

    // 读写锁：读操作使用 shared_lock，写操作使用 unique_lock
    mutable std::shared_mutex mutex_;
};

} // namespace kairo
