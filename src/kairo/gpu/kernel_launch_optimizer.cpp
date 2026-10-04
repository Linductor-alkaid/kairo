#include "kairo/gpu/kernel_launch_optimizer.hpp"
#include <list>
#include <algorithm>

namespace kairo {
namespace gpu {

KernelLaunchOptimizer::KernelLaunchOptimizer()
    : config_snapshot_(std::make_shared<const Config>()) {}

KernelLaunchOptimizer::KernelLaunchOptimizer(const Config& config)
    : config_snapshot_(std::make_shared<const Config>(config)) {}

// --- 参数缓存 ---

bool KernelLaunchOptimizer::lookup_params(const std::string& kernel_name,
                                           KernelParamCacheEntry& out) {
    // CR-063: 一次原子加载取得一致配置快照（下同）。
    const Config config = *std::atomic_load_explicit(&config_snapshot_,
                                                     std::memory_order_acquire);
    if (!config.enable_param_cache) {
        return false;
    }

    std::unique_lock lock(cache_mutex_);
    auto it = param_cache_.find(kernel_name);
    if (it == param_cache_.end()) {
        std::lock_guard slock(stats_mutex_);
        ++cache_misses_;
        return false;
    }

    it->second->second.last_access_ns =
        std::chrono::steady_clock::now().time_since_epoch().count();
    ++it->second->second.hit_count;
    out = it->second->second;

    // CR-151: 命中提升到队头（O(1) splice，迭代器保持有效）
    param_lru_.splice(param_lru_.begin(), param_lru_, it->second);

    {
        std::lock_guard slock(stats_mutex_);
        ++cache_hits_;
    }
    return true;
}

void KernelLaunchOptimizer::store_params(const std::string& kernel_name,
                                          const KernelParamCacheEntry& entry) {
    const Config config = *std::atomic_load_explicit(&config_snapshot_,
                                                     std::memory_order_acquire);
    if (!config.enable_param_cache) {
        return;
    }

    std::unique_lock lock(cache_mutex_);
    const auto now_ns = std::chrono::steady_clock::now().time_since_epoch().count();

    // CR-151: 已存在的 key 原地更新 + 提队头，不再触发淘汰（旧实现"先淘汰
    // 后插入"会把热点 kernel 的更新误判为新插入而逐掉别的热点）。
    auto existing = param_cache_.find(kernel_name);
    if (existing != param_cache_.end()) {
        existing->second->second = entry;
        existing->second->second.last_access_ns = now_ns;
        param_lru_.splice(param_lru_.begin(), param_lru_, existing->second);
        return;
    }

    evict_lru_if_needed();
    param_lru_.emplace_front(kernel_name, entry);
    param_lru_.front().second.last_access_ns = now_ns;
    param_cache_[kernel_name] = param_lru_.begin();
}

void KernelLaunchOptimizer::invalidate_params(const std::string& kernel_name) {
    std::unique_lock lock(cache_mutex_);
    auto it = param_cache_.find(kernel_name);
    if (it != param_cache_.end()) {
        param_lru_.erase(it->second);
        param_cache_.erase(it);
    }
}

void KernelLaunchOptimizer::clear_cache() {
    std::unique_lock lock(cache_mutex_);
    param_cache_.clear();
    param_lru_.clear();
}

size_t KernelLaunchOptimizer::cache_size() const {
    std::shared_lock lock(cache_mutex_);
    return param_cache_.size();
}

void KernelLaunchOptimizer::evict_lru_if_needed() {
    const Config config = *std::atomic_load_explicit(&config_snapshot_,
                                                     std::memory_order_acquire);
    if (param_cache_.size() < config.max_cache_entries) {
        return;
    }

    // CR-151: 队尾即最久未使用（splice 维持的访问序），O(1) 淘汰
    auto oldest = std::prev(param_lru_.end());
    param_cache_.erase(oldest->first);
    param_lru_.erase(oldest);
}

// --- 批量化 ---

size_t KernelLaunchOptimizer::enqueue(BatchedKernelRequest request) {
    const Config config = *std::atomic_load_explicit(&config_snapshot_,
                                                     std::memory_order_acquire);
    if (!config.enable_batching) {
        return 0;
    }

    std::lock_guard lock(batch_mutex_);
    if (batch_queue_.empty()) {
        batch_window_start_ns_ =
            std::chrono::steady_clock::now().time_since_epoch().count();
    }
    batch_queue_.push_back(std::move(request));
    return batch_queue_.size();
}

std::vector<BatchedKernelRequest> KernelLaunchOptimizer::flush_if_ready() {
    const Config config = *std::atomic_load_explicit(&config_snapshot_,
                                                     std::memory_order_acquire);
    if (!config.enable_batching) {
        return {};
    }

    std::lock_guard lock(batch_mutex_);
    if (batch_queue_.empty()) {
        return {};
    }

    bool should_flush = false;
    if (batch_queue_.size() >= config.batch_threshold) {
        should_flush = true;
    } else {
        auto now_ns = std::chrono::steady_clock::now().time_since_epoch().count();
        auto elapsed_us = (now_ns - batch_window_start_ns_) / 1000;
        if (elapsed_us >= config.batch_window_us) {
            should_flush = true;
        }
    }

    if (!should_flush) {
        return {};
    }

    std::vector<BatchedKernelRequest> result(batch_queue_.begin(), batch_queue_.end());
    batch_queue_.clear();

    {
        std::lock_guard slock(stats_mutex_);
        ++batched_launches_;
    }

    return result;
}

std::vector<BatchedKernelRequest> KernelLaunchOptimizer::flush_all() {
    const Config config = *std::atomic_load_explicit(&config_snapshot_,
                                                     std::memory_order_acquire);
    std::lock_guard lock(batch_mutex_);
    if (batch_queue_.empty()) {
        return {};
    }

    std::vector<BatchedKernelRequest> result(batch_queue_.begin(), batch_queue_.end());
    batch_queue_.clear();

    if (config.enable_batching) {
        std::lock_guard slock(stats_mutex_);
        ++batched_launches_;
    }

    return result;
}

size_t KernelLaunchOptimizer::pending_count() const {
    std::lock_guard lock(batch_mutex_);
    return batch_queue_.size();
}

// --- 延迟跟踪 ---

void KernelLaunchOptimizer::record_launch_latency(double latency_us) {
    if (!std::atomic_load_explicit(&config_snapshot_, std::memory_order_acquire)
             ->track_latency) {
        return;
    }

    std::lock_guard lock(stats_mutex_);
    total_latency_us_ += latency_us;
    ++total_launches_;
    min_latency_us_ = std::min(min_latency_us_, latency_us);
    max_latency_us_ = std::max(max_latency_us_, latency_us);
}

KernelLaunchStats KernelLaunchOptimizer::get_stats() const {
    std::lock_guard lock(stats_mutex_);
    KernelLaunchStats stats;
    stats.total_launches = total_launches_;
    stats.cache_hits = cache_hits_;
    stats.cache_misses = cache_misses_;
    stats.batched_launches = batched_launches_;
    stats.min_launch_latency_us = (total_launches_ > 0) ? min_latency_us_ : 0.0;
    stats.max_launch_latency_us = max_latency_us_;
    stats.avg_launch_latency_us = (total_launches_ > 0)
        ? total_latency_us_ / static_cast<double>(total_launches_)
        : 0.0;
    return stats;
}

void KernelLaunchOptimizer::reset_stats() {
    std::lock_guard lock(stats_mutex_);
    total_latency_us_ = 0.0;
    min_latency_us_ = 1e18;
    max_latency_us_ = 0.0;
    total_launches_ = 0;
    cache_hits_ = 0;
    cache_misses_ = 0;
    batched_launches_ = 0;
}

KernelLaunchOptimizer::Config KernelLaunchOptimizer::get_config() const {
    return *std::atomic_load_explicit(&config_snapshot_, std::memory_order_acquire);
}

void KernelLaunchOptimizer::update_config(const Config& config) {
    std::atomic_store_explicit(&config_snapshot_,
                               std::make_shared<const Config>(config),
                               std::memory_order_release);
}

} // namespace gpu
} // namespace kairo
