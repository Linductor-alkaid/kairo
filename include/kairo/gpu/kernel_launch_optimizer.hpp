#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>
#include <deque>
#include <functional>
#include <string>
#include <chrono>
#include <atomic>
#include <list>

namespace kairo {
namespace gpu {

/**
 * @brief Kernel 参数缓存条目
 *
 * 缓存已验证的 kernel 启动参数，避免重复计算最优 grid/block 配置。
 */
struct KernelParamCacheEntry {
    size_t grid_size[3] = {1, 1, 1};
    size_t block_size[3] = {1, 1, 1};
    size_t shared_memory_bytes = 0;
    int64_t last_access_ns = 0;       ///< 最后访问时间
    uint32_t hit_count = 0;           ///< 命中次数
};

/**
 * @brief 批量 kernel 启动请求
 */
struct BatchedKernelRequest {
    std::function<void(void*)> kernel_func;
    size_t grid_size[3] = {1, 1, 1};
    size_t block_size[3] = {1, 1, 1};
    size_t shared_memory_bytes = 0;
    int stream_id = 0;
    int priority = 1;
};

/**
 * @brief Kernel 启动延迟统计
 */
struct KernelLaunchStats {
    double avg_launch_latency_us = 0.0;   ///< 平均启动延迟（微秒）
    double min_launch_latency_us = 0.0;   ///< 最小启动延迟
    double max_launch_latency_us = 0.0;   ///< 最大启动延迟
    size_t total_launches = 0;            ///< 总启动次数
    size_t cache_hits = 0;                ///< 参数缓存命中次数
    size_t cache_misses = 0;              ///< 参数缓存未命中次数
    size_t batched_launches = 0;          ///< 批量启动次数
};

/**
 * @brief Kernel 启动优化器
 *
 * 提供三项优化：
 * 1. 参数缓存 — 缓存 kernel 名称到最优 grid/block 配置的映射，避免重复计算
 * 2. 启动批量化 — 将短时间内提交的多个 kernel 合并为一次批量提交
 * 3. 启动延迟优化 — 跟踪启动延迟并提供统计，辅助调优
 */
class KernelLaunchOptimizer {
public:
    struct Config {
        size_t max_cache_entries = 256;       ///< 最大缓存条目数
        size_t batch_threshold = 4;           ///< 触发批量提交的最小 kernel 数
        int64_t batch_window_us = 100;        ///< 批量收集窗口（微秒）
        bool enable_param_cache = true;       ///< 启用参数缓存
        bool enable_batching = true;          ///< 启用批量化
        bool track_latency = true;            ///< 跟踪启动延迟
    };

    KernelLaunchOptimizer();
    explicit KernelLaunchOptimizer(const Config& config);

    // --- 参数缓存 ---

    /// 查找缓存的 kernel 参数，命中返回 true 并填充 out
    bool lookup_params(const std::string& kernel_name, KernelParamCacheEntry& out);

    /// 存储 kernel 参数到缓存
    void store_params(const std::string& kernel_name, const KernelParamCacheEntry& entry);

    /// 使指定 kernel 的缓存失效
    void invalidate_params(const std::string& kernel_name);

    /// 清空参数缓存
    void clear_cache();

    /// 缓存条目数
    size_t cache_size() const;

    // --- 批量化 ---

    /// 将 kernel 加入批量队列，返回当前队列深度
    size_t enqueue(BatchedKernelRequest request);

    /// 如果队列达到阈值或窗口超时，取出一批 kernel；否则返回空
    std::vector<BatchedKernelRequest> flush_if_ready();

    /// 强制取出所有排队的 kernel
    std::vector<BatchedKernelRequest> flush_all();

    /// 当前批量队列深度
    size_t pending_count() const;

    // --- 延迟跟踪 ---

    /// 记录一次 kernel 启动延迟（微秒）
    void record_launch_latency(double latency_us);

    /// 获取启动统计
    KernelLaunchStats get_stats() const;

    /// 重置统计
    void reset_stats();

    /// 获取/更新配置
    Config get_config() const;
    void update_config(const Config& config);

private:
    void evict_lru_if_needed();  ///< LRU 淘汰

    // CR-063: config 以不可变快照发布（原子指针替换）。此前 update_config
    // 持 cache_mutex_ 写、多个读路径无锁读，构成数据竞争（TSAN 实证）。
    std::shared_ptr<const Config> config_snapshot_;
    mutable std::shared_mutex cache_mutex_;
    // CR-151: O(1) LRU——list 头部=最近使用，map 存 list 迭代器。旧实现
    // unordered_map + 每次 O(n) 全表扫描找最旧项，且"先淘汰后插入"对已存在
    // key 的更新也会误逐热点。新实现：命中/更新 splice 到队头 O(1)，淘汰
    // 直接取队尾 O(1)，更新已有 key 不再触发淘汰。
    std::list<std::pair<std::string, KernelParamCacheEntry>> param_lru_;
    std::unordered_map<std::string,
                       std::list<std::pair<std::string, KernelParamCacheEntry>>::iterator>
        param_cache_;

    mutable std::mutex batch_mutex_;
    std::deque<BatchedKernelRequest> batch_queue_;
    int64_t batch_window_start_ns_ = 0;

    mutable std::mutex stats_mutex_;
    double total_latency_us_ = 0.0;
    double min_latency_us_ = 1e18;
    double max_latency_us_ = 0.0;
    size_t total_launches_ = 0;
    size_t cache_hits_ = 0;
    size_t cache_misses_ = 0;
    size_t batched_launches_ = 0;
};

} // namespace gpu
} // namespace kairo
