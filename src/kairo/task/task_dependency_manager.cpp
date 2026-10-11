#include "task_dependency_manager.hpp"
#include <algorithm>
#include <mutex>
#include <utility>

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

    // CR-052：增量拓扑序（Pearce-Kelly 风格）。边 u→v 携带约束
    // ord[v] < ord[u]；新节点先给 depends_on(v) 分配、再给 task_id(u)
    // 分配，使新建边默认满足序（正序与逆序建链均落在快路径）。
    ensure_ord(depends_on);
    ensure_ord(task_id);

    bool accepted;
    if (ord_[depends_on] < ord_[task_id]) {
        accepted = true;  // 快路径：约束已满足，O(1)
    } else {
        // 违序：受限重排，环检测并入同一次遍历。
        accepted = rebuild_order_for(task_id, depends_on);
    }
    if (!accepted) {
        return false;
    }

    // 添加依赖（正向边 + 反向边同步）
    dependencies_[task_id].push_back(depends_on);
    dependents_[depends_on].push_back(task_id);
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
    // CR-052：拓扑结构同步回收。契约约定调用时已无其它任务依赖 task_id，
    // 反向表条目按契约应为空——防御性擦除；ord 条目随节点消失（同名
    // 重新注册将获得更大序号，既有约束 ord[被依赖] < ord[依赖者] 仍成立）。
    dependents_.erase(task_id);
    ord_.erase(task_id);
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
    // CR-052：反向边同步回收
    auto rit = dependents_.find(depends_on);
    if (rit != dependents_.end()) {
        auto& ents = rit->second;
        auto eit = std::find(ents.begin(), ents.end(), task_id);
        if (eit != ents.end()) {
            ents.erase(eit);
        }
        if (ents.empty()) {
            dependents_.erase(rit);
        }
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
    dependents_.clear();
    ord_.clear();
    next_ord_ = 0;
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

void TaskDependencyManager::ensure_ord(const std::string& id) {
    if (ord_.find(id) == ord_.end()) {
        ord_.emplace(id, next_ord_++);
    }
}

bool TaskDependencyManager::rebuild_order_for(const std::string& u,
                                              const std::string& v) {
    // 加约束 v→u（v 先于 u）而 ord[v] > ord[u]：两段式受限重排。
    //   A 段 = v 的祖先闭包（沿 dependencies_，收 ord ≥ ord_u，含 v）
    //   D 段 = u 的后代闭包（沿 dependents_，收 ord ≤ ord_v，含 u）
    // 前提（归纳不变式）：当前 ord 满足全部既有边约束，故任何祖先/后代
    // 链上序严格单调——剪枝不会切断通向 v/u 的路径，也不会漏掉交点。
    // 无环（A∩D=∅）时重排为：A 段按原序占区间前部，D 段按原序占后部。
    // 正确性：A∩D≠∅ ⟺ ∃w：v⇝w⇝u ⟺ 成环；无环时 A 与 D 之间既有约束
    // 只能是 A 中节点先于 D 中节点（反向 d⇝a 会经 u⇝d⇝a⇝v 落入 A∩D），
    // 两段式恰好保持段内原相对序并把 v 放到 u 之前；段外节点不受影响
    // （A 段新值 ≤ 原值，其后代约束保持；D 段新值 ≥ 原值，其祖先约束
    // 保持；被剪枝的链尾序值仍落在段界之外）。
    const int64_t ord_u = ord_[u];
    const int64_t ord_v = ord_[v];

    // A 段：从 v 沿正向边（我依赖谁）收集 ord ≥ ord_u 的祖先。
    std::vector<std::pair<std::string, int64_t>> seg_a;
    std::unordered_set<std::string> set_a;
    {
        std::vector<const std::string*> stack{&v};
        set_a.insert(v);
        seg_a.emplace_back(v, ord_v);
        while (!stack.empty()) {
            const std::string* cur = stack.back();
            stack.pop_back();
            auto it = dependencies_.find(*cur);
            if (it == dependencies_.end()) {
                continue;
            }
            for (const auto& dependency : it->second) {
                if (set_a.count(dependency)) {
                    continue;
                }
                const auto ord_it = ord_.find(dependency);
                if (ord_it == ord_.end() || ord_it->second < ord_u) {
                    continue;  // 剪枝：更小序的祖先无需移动
                }
                set_a.insert(dependency);
                seg_a.emplace_back(dependency, ord_it->second);
                stack.push_back(&dependency);
            }
        }
    }

    // 环判定（两者等价，取并增强）：环 ⟺ v 的依赖闭包含 u（u ∈ A）⟺
    // A∩D ≠ ∅。u ∈ A 在此显式检查；其余交点由 D 段遍历命中 set_a 捕获。
    if (set_a.count(u)) {
        return false;  // v ⇝ u 已存在：加边成环
    }

    // D 段：从 u 沿反向边（谁依赖我）收集 ord ≤ ord_v 的后代；途中命中
    // A 段成员（含 v）即 v ⇝ u 可达，加边成环。
    std::vector<std::pair<std::string, int64_t>> seg_d;
    std::unordered_set<std::string> seen_d{u};
    {
        std::vector<const std::string*> stack{&u};
        seg_d.emplace_back(u, ord_u);
        while (!stack.empty()) {
            const std::string* cur = stack.back();
            stack.pop_back();
            auto it = dependents_.find(*cur);
            if (it == dependents_.end()) {
                continue;
            }
            for (const auto& dependent : it->second) {
                if (set_a.count(dependent)) {
                    return false;  // v ⇝ u 存在：加边成环
                }
                if (seen_d.count(dependent)) {
                    continue;
                }
                const auto ord_it = ord_.find(dependent);
                if (ord_it == ord_.end() || ord_it->second > ord_v) {
                    continue;  // 剪枝：更大序的后代无需移动
                }
                seen_d.insert(dependent);
                seg_d.emplace_back(dependent, ord_it->second);
                stack.push_back(&dependent);
            }
        }
    }

    // 三段式重排：区间 [ord_u, ord_v] 内除 A、D 外还可能有第三方节点
    // （序值落在区间内、但与 u/v 无祖先关系的节点）——它们的既有值会被
    // A/D 两段的连续放置撞占，同值又瓦解后续重排依赖的"区间容量"论证。
    // 因此把区间内全部节点纳入：A 段左对齐（base=ord_u 起，成员新值
    // ≤ 原值，其约束后代安全）；第三方节点按原序居中；D 段右对齐（到
    // ord_v 止，成员新值 ≥ 原值，其约束祖先安全）。段间约束方向由闭包性
    // 锁死：others ⇝ a 会经 a ⇝ v 把 o 拉进 A；d ⇝ o 会经 u ⇝ d ⇝ o 把
    // o 拉进 D——均不存在；a ⇝ o 时 a_new ≤ a_old ≤ ord_v-|D| < o_new，
    // o ⇝ d 时 o_new ≤ ord_v-|D| < d_new 恒成立。v ∈ A 段末、u ∈ D 段首，
    // 新约束 v_new < u_new 恰好满足。
    std::sort(seg_a.begin(), seg_a.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.second < rhs.second; });
    std::sort(seg_d.begin(), seg_d.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.second < rhs.second; });

    // 区间内第三方节点（不属于 A∪D）
    std::vector<std::pair<std::string, int64_t>> seg_o;
    for (const auto& [id, value] : ord_) {
        if (value >= ord_u && value <= ord_v && !set_a.count(id) &&
            !seen_d.count(id)) {
            seg_o.emplace_back(id, value);
        }
    }
    std::sort(seg_o.begin(), seg_o.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.second < rhs.second; });

    int64_t assigned = ord_u;
    for (const auto& [id, ord] : seg_a) {
        ord_[id] = assigned++;
    }
    for (const auto& [id, ord] : seg_o) {
        ord_[id] = assigned++;
    }
    int64_t d_pos = ord_v;
    for (auto it = seg_d.rbegin(); it != seg_d.rend(); ++it) {
        ord_[it->first] = d_pos--;
    }
    return true;
}

} // namespace kairo
