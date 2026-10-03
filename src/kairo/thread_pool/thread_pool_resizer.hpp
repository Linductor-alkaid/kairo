#pragma once

#include "kairo/config.hpp"
#include <atomic>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <vector>

namespace kairo {

// 前向声明
class ThreadPool;

/**
 * @brief 线程池动态扩缩容控制器
 * 
 * 监控线程池状态，根据负载情况自动调整线程数量。
 * 支持扩容和缩容，确保线程池在最佳状态下运行。
 */
class ThreadPoolResizer {
public:
    /**
     * @brief 构造函数
     * 
     * @param pool 线程池引用
     * @param config 线程池配置
     */
    ThreadPoolResizer(ThreadPool& pool, const ThreadPoolConfig& config);

    /**
     * @brief 析构函数
     */
    ~ThreadPoolResizer() = default;

    // 禁止拷贝和移动
    ThreadPoolResizer(const ThreadPoolResizer&) = delete;
    ThreadPoolResizer& operator=(const ThreadPoolResizer&) = delete;
    ThreadPoolResizer(ThreadPoolResizer&&) = delete;
    ThreadPoolResizer& operator=(ThreadPoolResizer&&) = delete;

    /**
     * @brief 检查并执行扩缩容
     * 
     * 根据当前线程池状态，决定是否需要扩容或缩容。
     * 这个方法应该由监控线程定期调用。
     */
    void check_and_resize();

    /**
     * @brief 手动触发扩容
     * 
     * @param num_threads 要增加的线程数（默认1）
     * @return 成功返回 true
     */
    bool expand(size_t num_threads = 1);

    /**
     * @brief 手动触发缩容
     * 
     * @param num_threads 要减少的线程数（默认1）
     * @return 成功返回 true
     */
    bool shrink(size_t num_threads = 1);

    /**
     * @brief 启用/禁用动态扩缩容
     * 
     * @param enabled 是否启用
     */
    void set_enabled(bool enabled);

    /**
     * @brief 检查是否启用
     * 
     * @return 启用返回 true
     */
    bool is_enabled() const;

    /**
     * @brief 更新线程池状态信息（由 ThreadPool 调用）
     * 
     * @param queue_size 队列大小
     * @param active_threads 活跃线程数
     * @param total_threads 总线程数
     * @param avg_wait_time_ms 平均等待时间（毫秒）
     */
    void update_status(size_t queue_size,
                       size_t active_threads,
                       size_t total_threads,
                       double avg_wait_time_ms);

private:
    /**
     * @brief 评估是否需要扩容
     * 
     * @return 需要扩容返回 true
     */
    bool should_expand() const;

    /**
     * @brief 评估是否需要缩容
     * 
     * @return 需要缩容返回 true
     */
    bool should_shrink();

    /**
     * @brief 标记线程为待退出（优雅退出）
     * 
     * @param thread_id 线程ID
     */
    void mark_thread_for_exit(size_t thread_id);

    ThreadPool& pool_;                              // 线程池引用
    ThreadPoolConfig config_;                       // 配置信息
    
    // 状态信息（由 ThreadPool 更新）
    std::atomic<size_t> queue_size_{0};
    std::atomic<size_t> active_threads_{0};
    std::atomic<size_t> total_threads_{0};
    std::atomic<double> avg_wait_time_ms_{0.0};
    
    // 扩缩容控制
    std::atomic<bool> enabled_{true};
    std::chrono::steady_clock::time_point last_resize_time_;
    std::chrono::steady_clock::time_point last_idle_start_time_;
    std::atomic<bool> is_idle_{false};
    
    // 待退出的线程ID列表（用于缩容）
    mutable std::mutex exit_threads_mutex_;
    std::vector<size_t> exit_threads_;
};

} // namespace kairo
