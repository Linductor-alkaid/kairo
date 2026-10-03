#include "load_balancer.hpp"
#include <algorithm>
#include <limits>
#include <mutex>

namespace kairo {

LoadBalancer::LoadBalancer(size_t num_workers)
    : worker_loads_(num_workers) {
    auto now = std::chrono::steady_clock::now();
    for (auto& load : worker_loads_) {
        load.last_update = now;
    }
}

size_t LoadBalancer::select_worker() {
    // 260610P010: 读 atomic 用 relaxed(只关心最新值,不依赖与其它内存的顺序)
    switch (strategy_.load(std::memory_order_relaxed)) {
        case Strategy::ROUND_ROBIN:
            return select_round_robin();
        case Strategy::LEAST_TASKS:
            return select_least_tasks();
        case Strategy::LEAST_LOAD:
            return select_least_load();
        default:
            return select_round_robin();
    }
}

size_t LoadBalancer::select_round_robin() {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    size_t num_workers = worker_loads_.size();
    if (num_workers == 0) {
        return 0;
    }
    
    // 轮询索引本身仍使用原子操作；共享锁保护 worker_loads_ 的生命周期。
    size_t index = round_robin_index_.fetch_add(1, std::memory_order_relaxed);
    return index % num_workers;
}

size_t LoadBalancer::select_least_tasks() {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    size_t num_workers = worker_loads_.size();
    if (num_workers == 0) {
        return 0;
    }
    
    size_t best_worker = 0;
    size_t min_tasks = std::numeric_limits<size_t>::max();
    
    for (size_t i = 0; i < num_workers; ++i) {
        size_t total_tasks = worker_loads_[i].queue_size + worker_loads_[i].active_tasks;
        if (total_tasks < min_tasks) {
            min_tasks = total_tasks;
            best_worker = i;
        }
    }
    
    return best_worker;
}

size_t LoadBalancer::select_least_load() {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    size_t num_workers = worker_loads_.size();
    if (num_workers == 0) {
        return 0;
    }
    
    size_t best_worker = 0;
    size_t min_load = std::numeric_limits<size_t>::max();
    
    for (size_t i = 0; i < num_workers; ++i) {
        // 负载 = 队列大小 + 活跃任务数
        size_t load = worker_loads_[i].queue_size + worker_loads_[i].active_tasks;
        if (load < min_load) {
            min_load = load;
            best_worker = i;
        }
    }
    
    return best_worker;
}

void LoadBalancer::update_load(size_t worker_id, size_t queue_size, size_t active_tasks) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (worker_id >= worker_loads_.size()) {
        return;
    }

    worker_loads_[worker_id].queue_size = queue_size;
    worker_loads_[worker_id].active_tasks = active_tasks;
    worker_loads_[worker_id].last_update = std::chrono::steady_clock::now();
}

void LoadBalancer::update_load_batch(const std::vector<std::tuple<size_t, size_t, size_t>>& updates) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto now = std::chrono::steady_clock::now();
    const size_t num_workers = worker_loads_.size();
    for (const auto& u : updates) {
        size_t worker_id = std::get<0>(u);
        if (worker_id >= num_workers) continue;
        worker_loads_[worker_id].queue_size = std::get<1>(u);
        worker_loads_[worker_id].active_tasks = std::get<2>(u);
        worker_loads_[worker_id].last_update = now;
    }
}

LoadBalancer::WorkerLoad LoadBalancer::get_load(size_t worker_id) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    if (worker_id >= worker_loads_.size()) {
        return WorkerLoad{};
    }
    
    return worker_loads_[worker_id];
}

std::vector<LoadBalancer::WorkerLoad> LoadBalancer::get_all_loads() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return worker_loads_;
}

size_t LoadBalancer::highest_load_victim(size_t exclude_worker, size_t queue_count) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);

    const size_t num_workers = worker_loads_.size();
    if (num_workers != queue_count || num_workers <= 1) {
        return static_cast<size_t>(-1);
    }

    size_t victim = static_cast<size_t>(-1);
    size_t best_load = 0;
    for (size_t i = 0; i < num_workers; ++i) {
        if (i == exclude_worker) continue;
        size_t total_load = worker_loads_[i].queue_size + worker_loads_[i].active_tasks;
        if (total_load > best_load) {
            best_load = total_load;
            victim = i;
        }
    }
    return victim;
}

void LoadBalancer::set_strategy(Strategy strategy) {
    // 260610P010: relaxed 写足矣(setter 不会跟其它内存操作组成 happens-before 链)
    strategy_.store(strategy, std::memory_order_relaxed);
}

LoadBalancer::Strategy LoadBalancer::get_strategy() const {
    return strategy_.load(std::memory_order_relaxed);
}

void LoadBalancer::resize(size_t new_num_workers) {
    std::unique_lock<std::shared_mutex> lock(mutex_);

    const size_t old_size = worker_loads_.size();

    if (new_num_workers > old_size) {
        // 扩容：添加新的负载信息
        auto now = std::chrono::steady_clock::now();
        worker_loads_.resize(new_num_workers);
        for (size_t i = old_size; i < new_num_workers; ++i) {
            worker_loads_[i] = WorkerLoad{};
            worker_loads_[i].last_update = now;
        }
    } else if (new_num_workers < old_size) {
        // 缩容：移除多余的负载信息
        worker_loads_.resize(new_num_workers);
    }
}

} // namespace kairo
