#include "task_dependency_manager.hpp"
#include <algorithm>
#include <mutex>

namespace kairo {

bool TaskDependencyManager::add_dependency(const std::string& task_id, 
                                           const std::string& depends_on) {
    // 空任务ID检查
    if (task_id.empty() || depends_on.empty()) {
        return false;
    }

    // 自依赖检查
    if (task_id == depends_on) {
        return false;
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);

    // 检查是否已存在该依赖
    auto it = dependencies_.find(task_id);
    if (it != dependencies_.end()) {
        const auto& deps = it->second;
        if (std::find(deps.begin(), deps.end(), depends_on) != deps.end()) {
            // 依赖已存在，返回成功
            return true;
        }
    }

    // 检测循环依赖（需要在持有锁的情况下检查，因为需要访问 dependencies_）
    if (has_cycle(task_id, depends_on)) {
        return false;
    }

    // 添加依赖
    dependencies_[task_id].push_back(depends_on);
    return true;
}

bool TaskDependencyManager::is_ready(const std::string& task_id) const {
    if (task_id.empty()) {
        return false;
    }

    std::shared_lock<std::shared_mutex> lock(mutex_);

    // 如果任务没有依赖，则可以直接执行
    auto it = dependencies_.find(task_id);
    if (it == dependencies_.end() || it->second.empty()) {
        return true;
    }

    // 检查所有依赖是否都已完成
    for (const auto& dep : it->second) {
        if (completed_tasks_.find(dep) == completed_tasks_.end()) {
            return false;
        }
    }

    return true;
}

void TaskDependencyManager::mark_completed(const std::string& task_id) {
    if (task_id.empty()) {
        return;
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);
    completed_tasks_.insert(task_id);
}

size_t TaskDependencyManager::prune(const std::string& task_id) {
    if (task_id.empty()) {
        return 0;
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);
    size_t removed = 0;
    auto it = dependencies_.find(task_id);
    if (it != dependencies_.end()) {
        removed += it->second.size();
        dependencies_.erase(it);
    }
    auto cit = completed_tasks_.find(task_id);
    if (cit != completed_tasks_.end()) {
        completed_tasks_.erase(cit);
        ++removed;
    }
    return removed;
}

bool TaskDependencyManager::remove_dependency(const std::string& task_id,
                                               const std::string& depends_on) {
    if (task_id.empty() || depends_on.empty()) {
        return false;
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto it = dependencies_.find(task_id);
    if (it == dependencies_.end()) {
        return false;
    }
    auto& deps = it->second;
    auto dit = std::find(deps.begin(), deps.end(), depends_on);
    if (dit == deps.end()) {
        return false;
    }
    deps.erase(dit);
    if (deps.empty()) {
        dependencies_.erase(it);
    }
    return true;
}

TaskDependencyManager::Stats TaskDependencyManager::get_stats() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    Stats s{};
    s.task_count = dependencies_.size();
    s.completed_count = completed_tasks_.size();
    s.edge_count = 0;
    for (const auto& [_, deps] : dependencies_) {
        s.edge_count += deps.size();
    }
    return s;
}

void TaskDependencyManager::clear() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    dependencies_.clear();
    completed_tasks_.clear();
}

std::vector<std::string> TaskDependencyManager::get_dependencies(
    const std::string& task_id) const {
    
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    auto it = dependencies_.find(task_id);
    if (it == dependencies_.end()) {
        return {};
    }
    
    return it->second;
}

bool TaskDependencyManager::is_completed(const std::string& task_id) const {
    if (task_id.empty()) {
        return false;
    }

    std::shared_lock<std::shared_mutex> lock(mutex_);
    return completed_tasks_.find(task_id) != completed_tasks_.end();
}

bool TaskDependencyManager::has_cycle(const std::string& task_id, 
                                      const std::string& depends_on) const {
    // 如果 depends_on 就是 task_id，则存在循环（但这种情况已在 add_dependency 中检查）
    if (task_id == depends_on) {
        return true;
    }

    // 使用 DFS 检查从 depends_on 到 task_id 是否存在路径
    // 如果存在，则添加依赖后会形成循环：task_id -> depends_on -> ... -> task_id
    std::unordered_set<std::string> visited;
    return dfs_path_exists(depends_on, task_id, visited);
}

bool TaskDependencyManager::dfs_path_exists(
    const std::string& start,
    const std::string& target,
    std::unordered_set<std::string>& visited) const {

    // CR-052: 递归实现的深度随依赖链长度无界增长（256KB 栈上 n≈4000 即
    // SIGSEGV，8MB 栈约 9.6 万），且整段 DFS 在写锁内执行——改为显式栈
    // 迭代，语义不变：沿依赖边从 start 是否可达 target。栈内存的是
    // dependencies_ 内部字符串的指针，调用方持锁期间无变异，稳定。
    if (start == target) {
        return true;
    }

    std::vector<const std::string*> stack;
    visited.insert(start);
    stack.push_back(&start);

    while (!stack.empty()) {
        const std::string* current = stack.back();
        stack.pop_back();

        auto it = dependencies_.find(*current);
        if (it == dependencies_.end()) {
            continue;
        }
        for (const auto& dep : it->second) {
            if (dep == target) {
                return true;
            }
            if (visited.insert(dep).second) {
                stack.push_back(&dep);
            }
        }
    }

    return false;
}

} // namespace kairo
