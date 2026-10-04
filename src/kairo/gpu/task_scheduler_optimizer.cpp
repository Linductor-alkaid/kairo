#include "kairo/gpu/task_scheduler_optimizer.hpp"
#include <algorithm>

namespace kairo {
namespace gpu {

TaskSchedulerOptimizer::TaskSchedulerOptimizer() : config_() {}

TaskSchedulerOptimizer::TaskSchedulerOptimizer(const Config& config) : config_(config) {}

// --- 任务提交 ---

bool TaskSchedulerOptimizer::add_task(const GpuTaskNode& task) {
    std::unique_lock lock(graph_mutex_);
    if (task_graph_.size() >= config_.max_pending_tasks) {
        return false;
    }
    if (task_graph_.count(task.task_id)) {
        return false;  // 重复 task_id
    }
    task_graph_[task.task_id] = task;

    // CR-062: 维护反向依赖索引。
    for (const auto& dep : task.dependencies) {
        dependents_[dep].push_back(task.task_id);
    }

    if (config_.enable_priority_inheritance) {
        apply_priority_inheritance();
    }
    return true;
}

void TaskSchedulerOptimizer::erase_node_unlocked(const std::string& task_id) {
    // 摘除单个节点：清理其全部依赖的反向边，并回收"已无未决依赖者"的
    // completed 条目（CR-062：completed_tasks_ 此前只增不减）。
    auto node_it = task_graph_.find(task_id);
    if (node_it != task_graph_.end()) {
        for (const auto& dep : node_it->second.dependencies) {
            auto dep_it = dependents_.find(dep);
            if (dep_it == dependents_.end()) {
                continue;
            }
            auto& list = dep_it->second;
            list.erase(std::remove(list.begin(), list.end(), task_id), list.end());
            if (list.empty()) {
                dependents_.erase(dep_it);
                // 依赖已无未决消费者：completed 记录可以回收。迟到的
                // 新依赖者与"依赖未知 id"的既有语义一致（视为就绪）。
                if (completed_tasks_.count(dep) != 0) {
                    completed_tasks_.erase(dep);
                }
            }
        }
        task_graph_.erase(node_it);
    }
}

bool TaskSchedulerOptimizer::remove_task(const std::string& task_id) {
    std::unique_lock lock(graph_mutex_);
    if (task_graph_.find(task_id) == task_graph_.end()) {
        return false;
    }
    // CR-062: 级联移除全部（传递）依赖 task_id 的任务——否则其下游会因
    // "依赖不在图中"被误判为就绪，带着缺失输入执行。
    std::vector<std::string> stack{task_id};
    std::unordered_set<std::string> visited{task_id};
    while (!stack.empty()) {
        const std::string current = stack.back();
        stack.pop_back();
        auto dep_it = dependents_.find(current);
        if (dep_it != dependents_.end()) {
            for (const auto& downstream : dep_it->second) {
                if (visited.insert(downstream).second) {
                    stack.push_back(downstream);
                }
            }
        }
        erase_node_unlocked(current);
    }
    return true;
}

void TaskSchedulerOptimizer::mark_completed(const std::string& task_id) {
    std::unique_lock lock(graph_mutex_);
    // CR-062: 经 erase_node_unlocked 摘除节点并维护反向边；completed 条目
    // 在无未决依赖者时立即回收（下游全就绪/被移除后不再需要保留）。
    erase_node_unlocked(task_id);
    completed_tasks_.insert(task_id);
    if (dependents_.find(task_id) == dependents_.end()) {
        completed_tasks_.erase(task_id);
    }

    {
        std::lock_guard slock(stats_mutex_);
        ++total_scheduled_;
    }
}

size_t TaskSchedulerOptimizer::pending_count() const {
    std::shared_lock lock(graph_mutex_);
    return task_graph_.size();
}

// --- 依赖图优化 ---

std::vector<GpuTaskNode> TaskSchedulerOptimizer::get_ready_tasks() {
    std::unique_lock lock(graph_mutex_);
    std::vector<GpuTaskNode> ready;

    for (const auto& [task_id, task] : task_graph_) {
        bool all_deps_met = true;
        for (const auto& dep : task.dependencies) {
            if (!completed_tasks_.count(dep) && task_graph_.count(dep)) {
                all_deps_met = false;
                {
                    std::lock_guard slock(stats_mutex_);
                    ++dependency_waits_;
                }
                break;
            }
        }
        if (all_deps_met) {
            ready.push_back(task);
        }
    }

    // 按优先级排序（高优先级在前）
    std::sort(ready.begin(), ready.end(), [](const GpuTaskNode& a, const GpuTaskNode& b) {
        return a.priority > b.priority;
    });

    return ready;
}

bool TaskSchedulerOptimizer::has_cycle() const {
    std::shared_lock lock(graph_mutex_);
    std::unordered_set<std::string> visiting;
    std::unordered_set<std::string> visited;

    for (const auto& [task_id, _] : task_graph_) {
        if (!visited.count(task_id)) {
            if (dfs_has_cycle(task_id, visiting, visited)) {
                return true;
            }
        }
    }
    return false;
}

bool TaskSchedulerOptimizer::dfs_has_cycle(const std::string& node,
                                            std::unordered_set<std::string>& visiting,
                                            std::unordered_set<std::string>& visited) const {
    visiting.insert(node);
    auto it = task_graph_.find(node);
    if (it != task_graph_.end()) {
        for (const auto& dep : it->second.dependencies) {
            if (visiting.count(dep)) {
                return true;  // 环
            }
            if (!visited.count(dep) && task_graph_.count(dep)) {
                if (dfs_has_cycle(dep, visiting, visited)) {
                    return true;
                }
            }
        }
    }
    visiting.erase(node);
    visited.insert(node);
    return false;
}

void TaskSchedulerOptimizer::apply_priority_inheritance() {
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& [task_id, task] : task_graph_) {
            for (const auto& dep : task.dependencies) {
                auto dep_it = task_graph_.find(dep);
                if (dep_it != task_graph_.end() && dep_it->second.priority < task.priority) {
                    dep_it->second.priority = task.priority;
                    changed = true;
                    {
                        std::lock_guard slock(stats_mutex_);
                        ++priority_promotions_;
                    }
                }
            }
        }
    }
}

// --- 负载均衡 ---

void TaskSchedulerOptimizer::update_device_load(const DeviceLoad& load) {
    std::lock_guard lock(device_mutex_);
    device_loads_[load.device_id] = load;
}

int TaskSchedulerOptimizer::select_best_device(const GpuTaskNode& task) const {
    if (!config_.enable_load_balancing) {
        return 0;
    }

    std::lock_guard lock(device_mutex_);
    if (device_loads_.empty()) {
        return 0;
    }

    // CR-061: 初值与循环内同一公式（含任务自身开销）。此前初值漏加
    // task.estimated_cost，begin() 落在高负载设备时低负载设备永远无法胜出。
    int best_device = device_loads_.begin()->first;
    size_t min_cost = device_loads_.begin()->second.estimated_total_cost +
                      task.estimated_cost;

    for (const auto& [device_id, load] : device_loads_) {
        size_t total_cost = load.estimated_total_cost + task.estimated_cost;
        if (total_cost < min_cost) {
            min_cost = total_cost;
            best_device = device_id;
        }
    }

    {
        std::lock_guard slock(stats_mutex_);
        ++load_balance_moves_;
    }

    return best_device;
}

std::vector<DeviceLoad> TaskSchedulerOptimizer::get_device_loads() const {
    std::lock_guard lock(device_mutex_);
    std::vector<DeviceLoad> loads;
    loads.reserve(device_loads_.size());
    for (const auto& [_, load] : device_loads_) {
        loads.push_back(load);
    }
    return loads;
}

// --- 统计 ---

TaskSchedulingStats TaskSchedulerOptimizer::get_stats() const {
    std::lock_guard lock(stats_mutex_);
    TaskSchedulingStats stats;
    stats.total_tasks_scheduled = total_scheduled_;
    stats.priority_promotions = priority_promotions_;
    stats.dependency_waits = dependency_waits_;
    stats.load_balance_moves = load_balance_moves_;
    stats.avg_wait_time_us = (total_scheduled_ > 0)
        ? total_wait_us_ / static_cast<double>(total_scheduled_)
        : 0.0;
    return stats;
}

void TaskSchedulerOptimizer::reset_stats() {
    std::lock_guard lock(stats_mutex_);
    total_scheduled_ = 0;
    priority_promotions_ = 0;
    dependency_waits_ = 0;
    load_balance_moves_ = 0;
    total_wait_us_ = 0.0;
}

TaskSchedulerOptimizer::Config TaskSchedulerOptimizer::get_config() const {
    std::shared_lock lock(graph_mutex_);
    return config_;
}

void TaskSchedulerOptimizer::update_config(const Config& config) {
    std::unique_lock lock(graph_mutex_);
    config_ = config;
}

} // namespace gpu
} // namespace kairo
