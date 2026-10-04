#include "thread_pool_resizer.hpp"
#include "thread_pool.hpp"
// CR-117: kDefaultCapacitySlots 定义在 worker_local_queue.hpp。该头不在
// thread_pool.hpp 的 lockfree 构建分支（KAIRO_USE_LOCKFREE_WORKER_QUEUE）
// 的包含链里，必须显式包含，否则该 CI 变体编译失败。
#include "worker_local_queue.hpp"
#include <algorithm>
#include <chrono>

namespace kairo {

namespace {
// CR-117: queue_capacity=0 是"使用默认槽位"的哨兵（WorkerLocalQueue 实际
// 回退为 kDefaultCapacitySlots）。resizer 若直接拿原始 0 参与阈值运算，
// 会得到"任意积压即扩、永不缩"的退化行为（queue_high 恒真、queue_low 恒假）。
// 所有阈值计算一律使用该有效容量，与队列层的实际行为保持一致。
inline size_t effective_queue_capacity(size_t configured) {
    return configured == 0 ? WorkerLocalQueue::kDefaultCapacitySlots : configured;
}
}  // namespace

ThreadPoolResizer::ThreadPoolResizer(ThreadPool& pool, const ThreadPoolConfig& config)
    : pool_(pool)
    , config_(config)
    , last_resize_time_(std::chrono::steady_clock::now())
    , last_idle_start_time_(std::chrono::steady_clock::now()) {
}

void ThreadPoolResizer::check_and_resize() {
    if (!enabled_.load()) {
        return;
    }
    
    // 防止频繁扩缩容，至少间隔1秒
    auto now = std::chrono::steady_clock::now();
    auto time_since_last_resize = std::chrono::duration_cast<std::chrono::seconds>(
        now - last_resize_time_
    ).count();
    
    if (time_since_last_resize < 1) {
        return;
    }
    
    size_t current_threads = total_threads_.load();
    
    // 检查是否需要扩容
    if (should_expand()) {
        size_t max_threads = config_.max_threads;
        if (current_threads < max_threads) {
            size_t threads_to_add = std::min(size_t(1), max_threads - current_threads);
            if (expand(threads_to_add)) {
                last_resize_time_ = now;
            }
            return;
        }
    }
    
    // 检查是否需要缩容
    if (should_shrink()) {
        size_t min_threads = config_.min_threads;
        if (current_threads > min_threads) {
            size_t threads_to_remove = std::min(size_t(1), current_threads - min_threads);
            if (shrink(threads_to_remove)) {
                last_resize_time_ = now;
            }
        }
    }
}

bool ThreadPoolResizer::should_expand() const {
    size_t queue_size = queue_size_.load();
    double avg_wait_time = avg_wait_time_ms_.load();
    size_t current_threads = total_threads_.load();
    size_t max_threads = config_.max_threads;
    
    // 扩容条件（需要同时满足）：
    // 1. 队列中任务数 > 队列容量的 80%
    // 2. 平均任务等待时间 > 100ms
    // 3. 当前线程数 < max_threads
    
    const size_t capacity = effective_queue_capacity(config_.queue_capacity);
    const size_t queue_capacity_fifth = capacity / 5;
    const size_t queue_capacity_fifth_ceiling = queue_capacity_fifth
        + static_cast<size_t>(capacity % 5 != 0);
    bool queue_high = queue_size > capacity - queue_capacity_fifth_ceiling;
    bool wait_time_high = (avg_wait_time > 100.0);
    bool can_expand = (current_threads < max_threads);
    
    return queue_high && wait_time_high && can_expand;
}

bool ThreadPoolResizer::should_shrink() {
    size_t queue_size = queue_size_.load();
    size_t active_threads = active_threads_.load();
    size_t total_threads = total_threads_.load();
    size_t min_threads = config_.min_threads;
    
    // 缩容条件（需要同时满足）：
    // 1. 空闲线程数 > 总线程数的 50%
    // 2. 队列中任务数 < 队列容量的 20%
    // 3. 持续空闲时间 > 60秒
    // 4. 当前线程数 > min_threads
    
    // Guard against size_t underflow (same fix as ThreadPool::get_status()):
    // active_threads_ is a relaxed atomic that may briefly exceed
    // total_threads_ during resize transitions. Saturate to 0.
    size_t idle_threads = (active_threads <= total_threads)
                              ? (total_threads - active_threads)
                              : 0;
    bool idle_high = idle_threads > total_threads / 2;
    bool queue_low = queue_size
        < effective_queue_capacity(config_.queue_capacity) / 5;
    bool can_shrink = (total_threads > min_threads);
    
    // 检查持续空闲时间
    auto now = std::chrono::steady_clock::now();
    auto idle_duration = std::chrono::duration_cast<std::chrono::seconds>(
        now - last_idle_start_time_
    ).count();
    bool idle_long_enough = (idle_duration > 60);
    
    // 更新空闲状态
    if (idle_high && queue_low) {
        if (!is_idle_.load()) {
            is_idle_.store(true);
            last_idle_start_time_ = now;
        }
    } else {
        is_idle_.store(false);
    }
    
    return idle_high && queue_low && idle_long_enough && can_shrink;
}

bool ThreadPoolResizer::expand(size_t num_threads) {
    if (num_threads == 0) {
        return false;
    }

    const size_t current_threads = pool_.get_status().total_threads;
    if (current_threads > config_.max_threads ||
        num_threads > config_.max_threads - current_threads) {
        return false;
    }
    return pool_.resize(current_threads + num_threads);
}

bool ThreadPoolResizer::shrink(size_t num_threads) {
    if (num_threads == 0) {
        return false;
    }

    const size_t current_threads = pool_.get_status().total_threads;
    if (current_threads < config_.min_threads ||
        num_threads > current_threads - config_.min_threads) {
        return false;
    }
    return pool_.resize(current_threads - num_threads);
}

void ThreadPoolResizer::set_enabled(bool enabled) {
    enabled_.store(enabled);
}

bool ThreadPoolResizer::is_enabled() const {
    return enabled_.load();
}

void ThreadPoolResizer::update_status(size_t queue_size,
                                      size_t active_threads,
                                      size_t total_threads,
                                      double avg_wait_time_ms) {
    queue_size_.store(queue_size);
    active_threads_.store(active_threads);
    total_threads_.store(total_threads);
    avg_wait_time_ms_.store(avg_wait_time_ms);
}

void ThreadPoolResizer::mark_thread_for_exit(size_t thread_id) {
    std::lock_guard<std::mutex> lock(exit_threads_mutex_);
    exit_threads_.push_back(thread_id);
}

} // namespace kairo
