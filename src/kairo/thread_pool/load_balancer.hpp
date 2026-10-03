#pragma once

#include <cstddef>
#include <vector>
#include <tuple>
#include <atomic>
#include <shared_mutex>
#include <chrono>

namespace kairo {

/**
 * @brief 负载均衡器
 * 
 * 跟踪每个工作线程的负载状态，提供线程选择策略。
 * 支持轮询、最少任务优先等负载均衡策略。
 */
class LoadBalancer {
public:
    /**
     * @brief 负载均衡策略
     */
    enum class Strategy {
        ROUND_ROBIN,      // 轮询
        LEAST_TASKS,      // 最少任务优先
        LEAST_LOAD        // 最少负载优先（队列大小 + 活跃任务数）
    };

    /**
     * @brief 工作线程负载信息
     */
    struct WorkerLoad {
        constexpr WorkerLoad() noexcept = default;
        size_t queue_size = 0;           // 本地队列大小
        size_t active_tasks = 0;          // 正在执行的任务数
        std::chrono::steady_clock::time_point last_update;  // 最后更新时间
    };

    /**
     * @brief 构造函数
     * 
     * @param num_workers 工作线程数量
     */
    explicit LoadBalancer(size_t num_workers);

    /**
     * @brief 析构函数
     */
    ~LoadBalancer() = default;

    // 禁止拷贝和移动
    LoadBalancer(const LoadBalancer&) = delete;
    LoadBalancer& operator=(const LoadBalancer&) = delete;
    LoadBalancer(LoadBalancer&&) = delete;
    LoadBalancer& operator=(LoadBalancer&&) = delete;

    /**
     * @brief 选择最佳工作线程
     * 
     * 根据当前策略选择最适合的工作线程。
     * 
     * @return 选中的工作线程ID
     */
    size_t select_worker();

    /**
     * @brief 更新线程负载
     * 
     * @param worker_id 工作线程ID
     * @param queue_size 本地队列大小
     * @param active_tasks 正在执行的任务数
     */
    void update_load(size_t worker_id, size_t queue_size, size_t active_tasks);

    /**
     * @brief 批量更新线程负载（一次写锁）
     * 
     * @param updates (worker_id, queue_size, active_tasks) 列表
     */
    void update_load_batch(const std::vector<std::tuple<size_t, size_t, size_t>>& updates);

    /**
     * @brief 获取线程负载信息
     * 
     * @param worker_id 工作线程ID
     * @return 负载信息
     */
    WorkerLoad get_load(size_t worker_id) const;

    /**
     * @brief 获取所有线程负载（用于工作窃取）
     * 
     * @return 所有线程的负载信息
     */
    std::vector<WorkerLoad> get_all_loads() const;

    /**
     * @brief 无分配地选出总负载最高的工作线程（用于工作窃取选 victim）
     *
     * PA-13：steal 路径原实现经 get_all_loads() 拷贝整个 vector，再构造
     * pair vector 并 sort，每次窃取尝试 2 次堆分配。此方法在内部
     * shared_lock 下做一次 O(n) 扫描，零分配。
     *
     * @param exclude_worker 跳过的工作线程ID（窃取者自身）
     * @param queue_count 调用方观察到的本地队列数；与负载表大小不一致
     *        （resize 竞态窗口）时返回 SIZE_MAX，调用方走随机回退
     * @return 总负载最高且大于 0 的工作线程ID；无候选返回 SIZE_MAX
     */
    size_t highest_load_victim(size_t exclude_worker, size_t queue_count) const;

    /**
     * @brief 设置负载均衡策略
     * 
     * @param strategy 策略类型
     */
    void set_strategy(Strategy strategy);

    /**
     * @brief 获取当前策略
     * 
     * @return 当前策略
     */
    Strategy get_strategy() const;

    /**
     * @brief 调整工作线程数量（动态扩缩容时使用）
     * 
     * @param new_num_workers 新的工作线程数量
     */
    void resize(size_t new_num_workers);

private:
    /**
     * @brief 使用轮询策略选择线程
     * 
     * @return 选中的线程ID
     */
    size_t select_round_robin();

    /**
     * @brief 使用最少任务优先策略选择线程
     * 
     * @return 选中的线程ID
     */
    size_t select_least_tasks();

    /**
     * @brief 使用最少负载优先策略选择线程
     * 
     * @return 选中的线程ID
     */
    size_t select_least_load();

    std::vector<WorkerLoad> worker_loads_;      // 每个线程的负载信息
    std::atomic<size_t> round_robin_index_{0}; // 轮询索引
    // 260610P010: std::atomic<Strategy> 替换裸 Strategy 字段
    // 解决 set_strategy() 与 select_worker() 之间无锁并发读写引发的 data race (UB)
    std::atomic<Strategy> strategy_{Strategy::ROUND_ROBIN}; // 当前策略
    // Protects every access to worker_loads_, including size(). LoadBalancer
    // never acquires ThreadPool::local_queues_mutex_: callers may hold that
    // lock before entering here, so this prevents a reverse lock order.
    mutable std::shared_mutex mutex_;
};

} // namespace kairo
