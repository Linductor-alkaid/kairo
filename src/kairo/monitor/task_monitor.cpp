#include "kairo/monitor/task_monitor.hpp"
#include <algorithm>
#include <atomic>
#include <functional>

namespace kairo {
namespace monitor {

namespace {
constexpr auto relaxed = std::memory_order_relaxed;
}  // namespace

size_t TaskMonitor::shard_of(const std::string& key, size_t shard_count) {
    return std::hash<std::string>{}(key) % shard_count;
}

void TaskMonitor::record_task_queued(const std::string& task_id,
                                     const std::string& task_type,
                                     const std::string& executor_name) {
    if (!enabled_.load(relaxed) || !should_sample_in_flight()) {
        return;
    }
    const size_t capacity = in_flight_capacity_.load(relaxed);
    if (capacity == 0) {
        return;
    }
    // 分片化后容量是跨分片软上界：原子计数预留槽位，竞争下至多短暂
    // 超限一两个条目（快照本身弱一致，见 types.hpp 的诊断语义）。
    if (in_flight_count_.fetch_add(1, relaxed) >= capacity) {
        in_flight_count_.fetch_sub(1, relaxed);
        in_flight_dropped_count_.fetch_add(1, relaxed);
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    auto& shard = in_flight_shards_[shard_of(task_id, kIdShardCount)];
    std::lock_guard<std::mutex> lock(shard.mutex);
    shard.tasks.emplace(task_id, TaskLifecycleSnapshot{
        task_id, task_type, executor_name, TaskLifecycleState::Queued, now, now});
}

void TaskMonitor::record_task_pending(const std::string& task_id,
                                      const std::string& task_type,
                                      const std::string& executor_name) {
    if (!enabled_.load(relaxed) || !should_sample_in_flight()) {
        return;
    }
    const size_t capacity = in_flight_capacity_.load(relaxed);
    if (capacity == 0) {
        return;
    }
    if (in_flight_count_.fetch_add(1, relaxed) >= capacity) {
        in_flight_count_.fetch_sub(1, relaxed);
        in_flight_dropped_count_.fetch_add(1, relaxed);
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    auto& shard = in_flight_shards_[shard_of(task_id, kIdShardCount)];
    std::lock_guard<std::mutex> lock(shard.mutex);
    shard.tasks.emplace(task_id, TaskLifecycleSnapshot{
        task_id, task_type, executor_name, TaskLifecycleState::Pending, now, now});
}

void TaskMonitor::record_task_state(const std::string& task_id, TaskLifecycleState state) {
    if (!enabled_.load(relaxed) || !in_flight_active()) {
        return;
    }
    auto& shard = in_flight_shards_[shard_of(task_id, kIdShardCount)];
    std::lock_guard<std::mutex> lock(shard.mutex);
    auto it = shard.tasks.find(task_id);
    if (it != shard.tasks.end()) {
        it->second.state = state;
        it->second.state_changed_at = std::chrono::steady_clock::now();
    }
}

void TaskMonitor::record_task_terminal(const std::string& task_id) {
    if (!enabled_.load(relaxed) || !in_flight_active()) {
        return;
    }
    auto& shard = in_flight_shards_[shard_of(task_id, kIdShardCount)];
    std::lock_guard<std::mutex> lock(shard.mutex);
    if (shard.tasks.erase(task_id) != 0) {
        in_flight_count_.fetch_sub(1, relaxed);
    }
}

TaskMonitor::~TaskMonitor() {
    // 与最后一个持分片锁的读者（线程池 worker 的 record_task_*）建立
    // happens-before。析构若不取锁，分片成员的无锁析构与 shutdown/析构
    // 竞态下 worker 尚未结束的持锁访问构成数据竞争（TSAN 报告；若 worker
    // 真在临界区内则是真实 UAF）。逐片取锁后：worker 在任一临界区内则
    // 阻塞等待其退出；已退出则 unlock→lock 边使成员析构 formally 排序在
    // 最后一次访问之后。分片内 mutex 声明先于容器，容器先于锁析构。
    for (auto& shard : id_shards_) {
        std::lock_guard<std::mutex> lock(shard.mutex);
    }
    for (auto& shard : in_flight_shards_) {
        std::lock_guard<std::mutex> lock(shard.mutex);
    }
    for (auto& shard : type_shards_) {
        std::lock_guard<std::mutex> lock(shard.mutex);
    }
}

void TaskMonitor::record_task_start(const std::string& task_id,
                                    const std::string& task_type) {
    if (!enabled_.load(relaxed)) {
        return;
    }
    if (in_flight_active()) {
        auto& in_flight_shard = in_flight_shards_[shard_of(task_id, kIdShardCount)];
        std::lock_guard<std::mutex> lock(in_flight_shard.mutex);
        auto in_flight = in_flight_shard.tasks.find(task_id);
        if (in_flight != in_flight_shard.tasks.end()) {
            in_flight->second.state = TaskLifecycleState::Running;
            in_flight->second.state_changed_at = std::chrono::steady_clock::now();
        }
    }
    // should_sample() 自带采样率 0 的无锁早退；采样率为 0 时 id 映射已被
    // set_sampling_rate 清空，此分支不进入。
    if (should_sample()) {
        auto& shard = id_shards_[shard_of(task_id, kIdShardCount)];
        std::lock_guard<std::mutex> lock(shard.mutex);
        shard.id_to_type[task_id] = task_type;
    }
}

void TaskMonitor::record_task_complete(const std::string& task_id,
                                       bool success,
                                       int64_t execution_time_ns) {
    if (!enabled_.load(relaxed)) {
        return;
    }
    // CR-071 快速门：in-flight 关闭时分片为空，擦除可跳过；统计采样为 0
    // 时 id 映射为空，结算可跳过。两门全关时本函数零取锁。
    if (in_flight_active()) {
        auto& in_flight_shard = in_flight_shards_[shard_of(task_id, kIdShardCount)];
        std::lock_guard<std::mutex> lock(in_flight_shard.mutex);
        if (in_flight_shard.tasks.erase(task_id) != 0) {
            in_flight_count_.fetch_sub(1, relaxed);
        }
    }
    if (sampling_rate_.load(relaxed) == 0) {
        return;
    }
    std::string task_type;
    {
        auto& shard = id_shards_[shard_of(task_id, kIdShardCount)];
        std::lock_guard<std::mutex> lock(shard.mutex);
        auto it = shard.id_to_type.find(task_id);
        if (it == shard.id_to_type.end()) {
            return;  // 未 record_task_start（或未采样），忽略
        }
        task_type = std::move(it->second);
        shard.id_to_type.erase(it);
    }

    auto& shard = type_shards_[shard_of(task_type, kTypeShardCount)];
    std::lock_guard<std::mutex> lock(shard.mutex);
    Stats& s = shard.stats[task_type];
    s.total_count += 1;
    if (success) {
        s.success_count += 1;
    } else {
        s.fail_count += 1;
    }
    s.total_execution_time_ns += execution_time_ns;
    if (execution_time_ns > s.max_execution_time_ns) {
        s.max_execution_time_ns = execution_time_ns;
    }
    if (s.min_execution_time_ns == 0) {
        s.min_execution_time_ns = execution_time_ns;
    } else {
        s.min_execution_time_ns =
            std::min(s.min_execution_time_ns, execution_time_ns);
    }
}

void TaskMonitor::record_task_timeout(const std::string& task_id) {
    if (!enabled_.load(relaxed)) {
        return;
    }
    if (in_flight_active()) {
        auto& in_flight_shard = in_flight_shards_[shard_of(task_id, kIdShardCount)];
        std::lock_guard<std::mutex> lock(in_flight_shard.mutex);
        if (in_flight_shard.tasks.erase(task_id) != 0) {
            in_flight_count_.fetch_sub(1, relaxed);
        }
    }
    if (sampling_rate_.load(relaxed) == 0) {
        return;
    }
    std::string task_type;
    {
        auto& shard = id_shards_[shard_of(task_id, kIdShardCount)];
        std::lock_guard<std::mutex> lock(shard.mutex);
        auto it = shard.id_to_type.find(task_id);
        if (it == shard.id_to_type.end()) {
            return;
        }
        task_type = std::move(it->second);
        shard.id_to_type.erase(it);
    }

    auto& shard = type_shards_[shard_of(task_type, kTypeShardCount)];
    std::lock_guard<std::mutex> lock(shard.mutex);
    Stats& s = shard.stats[task_type];
    s.total_count += 1;
    s.timeout_count += 1;
}

TaskStatistics TaskMonitor::get_statistics(const std::string& task_type) const {
    auto& shard = type_shards_[shard_of(task_type, kTypeShardCount)];
    std::lock_guard<std::mutex> lock(shard.mutex);
    auto it = shard.stats.find(task_type);
    if (it == shard.stats.end()) {
        return {};
    }
    const Stats& s = it->second;
    TaskStatistics out;
    out.total_count = s.total_count;
    out.success_count = s.success_count;
    out.fail_count = s.fail_count;
    out.timeout_count = s.timeout_count;
    out.total_execution_time_ns = s.total_execution_time_ns;
    out.max_execution_time_ns = s.max_execution_time_ns;
    out.min_execution_time_ns = s.min_execution_time_ns;
    return out;
}

std::map<std::string, TaskStatistics> TaskMonitor::get_all_statistics() const {
    std::map<std::string, TaskStatistics> result;
    for (auto& shard : type_shards_) {
        std::lock_guard<std::mutex> lock(shard.mutex);
        for (const auto& [k, s] : shard.stats) {
            TaskStatistics& t = result[k];
            t.total_count += s.total_count;
            t.success_count += s.success_count;
            t.fail_count += s.fail_count;
            t.timeout_count += s.timeout_count;
            t.total_execution_time_ns += s.total_execution_time_ns;
            t.max_execution_time_ns =
                std::max(t.max_execution_time_ns, s.max_execution_time_ns);
            // 0 表示无样本；分片间取两者中较小的非零值。
            if (t.min_execution_time_ns == 0) {
                t.min_execution_time_ns = s.min_execution_time_ns;
            } else if (s.min_execution_time_ns != 0) {
                t.min_execution_time_ns =
                    std::min(t.min_execution_time_ns, s.min_execution_time_ns);
            }
        }
    }
    return result;
}

std::vector<TaskLifecycleSnapshot> TaskMonitor::get_in_flight_tasks() const {
    std::vector<TaskLifecycleSnapshot> result;
    for (auto& shard : in_flight_shards_) {
        std::lock_guard<std::mutex> lock(shard.mutex);
        for (const auto& [_, task] : shard.tasks) {
            result.push_back(task);
        }
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.submitted_at < right.submitted_at;
    });
    return result;
}

InFlightTaskDiagnostics TaskMonitor::get_in_flight_diagnostics() const {
    InFlightTaskDiagnostics result;
    result.dropped_count = in_flight_dropped_count_.load(relaxed);
    result.incomplete = result.dropped_count != 0;
    result.tasks.reserve(in_flight_count_.load(relaxed));
    auto oldest = std::chrono::steady_clock::now();
    for (auto& shard : in_flight_shards_) {
        std::lock_guard<std::mutex> lock(shard.mutex);
        for (const auto& [_, task] : shard.tasks) {
            ++result.state_counts[task.state];
            result.tasks.push_back(task);
            oldest = std::min(oldest, task.submitted_at);
        }
    }
    result.count = result.tasks.size();
    std::sort(result.tasks.begin(), result.tasks.end(), [](const auto& left, const auto& right) {
        return left.submitted_at < right.submitted_at;
    });
    if (!result.tasks.empty()) {
        result.oldest_age = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - oldest);
    }
    return result;
}

std::map<TaskLifecycleState, size_t> TaskMonitor::get_in_flight_state_counts() const {
    std::map<TaskLifecycleState, size_t> result;
    for (auto& shard : in_flight_shards_) {
        std::lock_guard<std::mutex> lock(shard.mutex);
        for (const auto& [_, task] : shard.tasks) {
            ++result[task.state];
        }
    }
    return result;
}

size_t TaskMonitor::get_in_flight_count() const {
    return in_flight_count_.load(std::memory_order_acquire);
}

size_t TaskMonitor::get_in_flight_evicted_count() const {
    return in_flight_evicted_count_.load(relaxed);
}

size_t TaskMonitor::get_in_flight_dropped_count() const {
    return in_flight_dropped_count_.load(relaxed);
}

std::chrono::nanoseconds TaskMonitor::get_oldest_in_flight_age() const {
    auto oldest = std::chrono::steady_clock::now();
    bool any = false;
    for (auto& shard : in_flight_shards_) {
        std::lock_guard<std::mutex> lock(shard.mutex);
        for (const auto& [_, task] : shard.tasks) {
            any = true;
            oldest = std::min(oldest, task.submitted_at);
        }
    }
    if (!any) {
        return std::chrono::nanoseconds{0};
    }
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - oldest);
}

bool TaskMonitor::in_flight_diagnostics_incomplete() const {
    return in_flight_dropped_count_.load(relaxed) != 0;
}

void TaskMonitor::set_in_flight_capacity(size_t capacity) {
    in_flight_capacity_.store(capacity, relaxed);
    // 缩容驱逐：低频配置路径，按固定数组序锁全部分片后统一挑最老逐出。
    std::vector<std::unique_lock<std::mutex>> locks;
    locks.reserve(kIdShardCount);
    for (auto& shard : in_flight_shards_) {
        locks.emplace_back(shard.mutex);
    }
    const size_t over = in_flight_count_.load(relaxed);
    if (over <= capacity) {
        return;
    }
    std::vector<std::pair<const std::string*, std::chrono::steady_clock::time_point>>
        candidates;
    candidates.reserve(over);
    for (auto& shard : in_flight_shards_) {
        for (const auto& [key, task] : shard.tasks) {
            candidates.emplace_back(&key, task.submitted_at);
        }
    }
    std::nth_element(
        candidates.begin(), candidates.end() - static_cast<std::ptrdiff_t>(over - capacity),
        candidates.end(),
        [](const auto& left, const auto& right) {
            return left.second > right.second;  // 最老的排尾部
        });
    const size_t evict = over - capacity;
    for (auto it = candidates.end() - static_cast<std::ptrdiff_t>(evict);
         it != candidates.end(); ++it) {
        for (auto& shard : in_flight_shards_) {
            if (shard.tasks.erase(*it->first) != 0) {
                break;
            }
        }
    }
    in_flight_count_.fetch_sub(evict, relaxed);
    // CR-075: 缩容驱逐单独计数——incomplete 只反映运行期准入丢弃，
    // 否则调小一次容量后所有快照永远 incomplete 且与真实丢混淆。
    in_flight_evicted_count_.fetch_add(evict, relaxed);
}

size_t TaskMonitor::get_in_flight_capacity() const {
    return in_flight_capacity_.load(relaxed);
}

void TaskMonitor::set_in_flight_sampling_rate(double rate) {
    // CR-074: 同 set_sampling_rate——先钳制再转换，负数/NaN 不再是 UB。
    if (!(rate >= 0.0)) rate = 0.0;
    if (rate > 1.0) rate = 1.0;
    const uint32_t percent = static_cast<uint32_t>(rate * 100.0);
    in_flight_sampling_rate_.store(std::min(percent, 100u), relaxed);
    if (percent == 0) {
        // CR-071 快速门的前提：门关时分片必须为空，否则 record_task_* 的
        // 无锁跳过会把已采样条目留在快照里永不结算。语义与 set_enabled(false)
        // 对齐——关闭即清空。
        size_t remaining = 0;
        for (auto& shard : in_flight_shards_) {
            std::lock_guard<std::mutex> lock(shard.mutex);
            remaining += shard.tasks.size();
            shard.tasks.clear();
        }
        in_flight_count_.fetch_sub(remaining, relaxed);
    }
}

double TaskMonitor::get_in_flight_sampling_rate() const {
    return in_flight_sampling_rate_.load(relaxed) / 100.0;
}

void TaskMonitor::set_enabled(bool enabled) {
    enabled_.store(enabled, relaxed);
    if (!enabled) {
        size_t remaining = 0;
        for (auto& shard : id_shards_) {
            std::lock_guard<std::mutex> lock(shard.mutex);
            shard.id_to_type.clear();
        }
        for (auto& shard : in_flight_shards_) {
            std::lock_guard<std::mutex> lock(shard.mutex);
            remaining += shard.tasks.size();
            shard.tasks.clear();
        }
        in_flight_count_.fetch_sub(remaining, relaxed);
    }
}

bool TaskMonitor::is_enabled() const {
    return enabled_.load(relaxed);
}

void TaskMonitor::set_sampling_rate(double rate) {
    // CR-074: 越界/NaN 直接做 double→uint32 转换是 UB（[conv.fpint]），
    // 实测负数会回绕成 100% 采样、NaN 变 0%。先钳制到 [0,1] 再转换。
    if (!(rate >= 0.0)) rate = 0.0;  // 同时处理负数与 NaN
    if (rate > 1.0) rate = 1.0;
    const uint32_t percent = static_cast<uint32_t>(rate * 100.0);
    sampling_rate_.store(percent, relaxed);
    if (percent == 0) {
        // CR-071 快速门的前提：统计采样关闭时 id 映射必须为空。已开始未
        // 结算的样本随清空丢弃——语义即"关闭统计采样"（CHANGELOG 0.7.0）。
        for (auto& shard : id_shards_) {
            std::lock_guard<std::mutex> lock(shard.mutex);
            shard.id_to_type.clear();
        }
    }
}

double TaskMonitor::get_sampling_rate() const {
    return sampling_rate_.load(relaxed) / 100.0;
}

bool TaskMonitor::should_sample() const {
    uint32_t rate = sampling_rate_.load(relaxed);
    if (rate >= 100) return true;
    if (rate == 0) return false;
    uint64_t count = sample_counter_.fetch_add(1, relaxed);
    return (count % 100) < rate;
}

bool TaskMonitor::should_sample_in_flight() const {
    const uint32_t rate = in_flight_sampling_rate_.load(relaxed);
    if (rate >= 100) return true;
    if (rate == 0) return false;
    const uint64_t count = in_flight_sample_counter_.fetch_add(1, relaxed);
    return (count % 100) < rate;
}

bool TaskMonitor::in_flight_active() const {
    return in_flight_sampling_rate_.load(relaxed) > 0 &&
           in_flight_capacity_.load(relaxed) > 0;
}

}  // namespace monitor
}  // namespace kairo
