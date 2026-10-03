#include "kairo/monitor/task_monitor.hpp"
#include <algorithm>
#include <atomic>

namespace kairo {
namespace monitor {

void TaskMonitor::record_task_queued(const std::string& task_id,
                                     const std::string& task_type,
                                     const std::string& executor_name) {
    if (!enabled_.load(std::memory_order_relaxed) || !should_sample_in_flight()) {
        return;
    }
    const size_t capacity = in_flight_capacity_.load(std::memory_order_relaxed);
    if (capacity == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (in_flight_tasks_.size() >= capacity) {
        ++in_flight_dropped_count_;
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    in_flight_tasks_.emplace(task_id, TaskLifecycleSnapshot{
        task_id, task_type, executor_name, TaskLifecycleState::Queued, now, now});
}

void TaskMonitor::record_task_pending(const std::string& task_id,
                                      const std::string& task_type,
                                      const std::string& executor_name) {
    if (!enabled_.load(std::memory_order_relaxed) || !should_sample_in_flight()) {
        return;
    }
    const size_t capacity = in_flight_capacity_.load(std::memory_order_relaxed);
    if (capacity == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (in_flight_tasks_.size() >= capacity) {
        ++in_flight_dropped_count_;
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    in_flight_tasks_.emplace(task_id, TaskLifecycleSnapshot{
        task_id, task_type, executor_name, TaskLifecycleState::Pending, now, now});
}

void TaskMonitor::record_task_state(const std::string& task_id, TaskLifecycleState state) {
    if (!enabled_.load(std::memory_order_relaxed)) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = in_flight_tasks_.find(task_id);
    if (it != in_flight_tasks_.end()) {
        it->second.state = state;
        it->second.state_changed_at = std::chrono::steady_clock::now();
    }
}

void TaskMonitor::record_task_terminal(const std::string& task_id) {
    if (!enabled_.load(std::memory_order_relaxed)) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    in_flight_tasks_.erase(task_id);
}

TaskMonitor::~TaskMonitor() {
    // 与最后一个持锁读者（线程池 worker 的 record_task_*）建立 happens-before。
    // 析构若不取 mutex_，map 成员的无锁 clear 与 shutdown/析构竞态下 worker
    // 尚未结束的持锁访问构成数据竞争（TSAN 报告；若 worker 真在临界区内则
    // 是真实 UAF）。取锁后：worker 在临界区内则阻塞等待其退出；已退出则
    // unlock→lock 边使成员析构 formallly 排序在最后一次访问之后。
    // mutex_ 在类内声明先于各 map，逆序析构保证其最后销毁。
    std::lock_guard<std::mutex> lock(mutex_);
}

void TaskMonitor::record_task_start(const std::string& task_id,
                                    const std::string& task_type) {
    if (!enabled_.load(std::memory_order_relaxed)) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto in_flight = in_flight_tasks_.find(task_id);
    if (in_flight != in_flight_tasks_.end()) {
        in_flight->second.state = TaskLifecycleState::Running;
        in_flight->second.state_changed_at = std::chrono::steady_clock::now();
    }
    if (should_sample()) {
        task_id_to_type_[task_id] = task_type;
    }
}

void TaskMonitor::record_task_complete(const std::string& task_id,
                                       bool success,
                                       int64_t execution_time_ns) {
    if (!enabled_.load(std::memory_order_relaxed)) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = task_id_to_type_.find(task_id);
    in_flight_tasks_.erase(task_id);
    if (it == task_id_to_type_.end()) {
        return;  // 未 record_task_start，忽略
    }
    std::string task_type = std::move(it->second);
    task_id_to_type_.erase(it);

    Stats& s = type_stats_[task_type];
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
    if (!enabled_.load(std::memory_order_relaxed)) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = task_id_to_type_.find(task_id);
    in_flight_tasks_.erase(task_id);
    if (it == task_id_to_type_.end()) {
        return;
    }
    std::string task_type = std::move(it->second);
    task_id_to_type_.erase(it);

    Stats& s = type_stats_[task_type];
    s.total_count += 1;
    s.timeout_count += 1;
}

TaskStatistics TaskMonitor::get_statistics(const std::string& task_type) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = type_stats_.find(task_type);
    if (it == type_stats_.end()) {
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
    std::lock_guard<std::mutex> lock(mutex_);
    std::map<std::string, TaskStatistics> result;
    for (const auto& [k, s] : type_stats_) {
        TaskStatistics t;
        t.total_count = s.total_count;
        t.success_count = s.success_count;
        t.fail_count = s.fail_count;
        t.timeout_count = s.timeout_count;
        t.total_execution_time_ns = s.total_execution_time_ns;
        t.max_execution_time_ns = s.max_execution_time_ns;
        t.min_execution_time_ns = s.min_execution_time_ns;
        result[k] = t;
    }
    return result;
}

std::vector<TaskLifecycleSnapshot> TaskMonitor::get_in_flight_tasks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<TaskLifecycleSnapshot> result;
    result.reserve(in_flight_tasks_.size());
    for (const auto& [_, task] : in_flight_tasks_) {
        result.push_back(task);
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.submitted_at < right.submitted_at;
    });
    return result;
}

InFlightTaskDiagnostics TaskMonitor::get_in_flight_diagnostics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    InFlightTaskDiagnostics result;
    result.count = in_flight_tasks_.size();
    result.dropped_count = in_flight_dropped_count_;
    result.incomplete = result.dropped_count != 0;
    result.tasks.reserve(result.count);
    auto oldest = std::chrono::steady_clock::now();
    for (const auto& [_, task] : in_flight_tasks_) {
        ++result.state_counts[task.state];
        result.tasks.push_back(task);
        oldest = std::min(oldest, task.submitted_at);
    }
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
    std::lock_guard<std::mutex> lock(mutex_);
    std::map<TaskLifecycleState, size_t> result;
    for (const auto& [_, task] : in_flight_tasks_) {
        ++result[task.state];
    }
    return result;
}

size_t TaskMonitor::get_in_flight_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return in_flight_tasks_.size();
}

size_t TaskMonitor::get_in_flight_evicted_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return in_flight_evicted_count_;
}

size_t TaskMonitor::get_in_flight_dropped_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return in_flight_dropped_count_;
}

std::chrono::nanoseconds TaskMonitor::get_oldest_in_flight_age() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (in_flight_tasks_.empty()) {
        return std::chrono::nanoseconds{0};
    }
    auto oldest = std::chrono::steady_clock::now();
    for (const auto& [_, task] : in_flight_tasks_) {
        oldest = std::min(oldest, task.submitted_at);
    }
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - oldest);
}

bool TaskMonitor::in_flight_diagnostics_incomplete() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return in_flight_dropped_count_ != 0;
}

void TaskMonitor::set_in_flight_capacity(size_t capacity) {
    in_flight_capacity_.store(capacity, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(mutex_);
    while (in_flight_tasks_.size() > capacity) {
        auto oldest = std::min_element(in_flight_tasks_.begin(), in_flight_tasks_.end(),
            [](const auto& left, const auto& right) {
                return left.second.submitted_at < right.second.submitted_at;
            });
        in_flight_tasks_.erase(oldest);
        // CR-075: 缩容驱逐单独计数——incomplete 只反映运行期准入丢弃，
        // 否则调小一次容量后所有快照永远 incomplete 且与真实丢混淆。
        ++in_flight_evicted_count_;
    }
}

size_t TaskMonitor::get_in_flight_capacity() const {
    return in_flight_capacity_.load(std::memory_order_relaxed);
}

void TaskMonitor::set_in_flight_sampling_rate(double rate) {
    // CR-074: 同 set_sampling_rate——先钳制再转换，负数/NaN 不再是 UB。
    if (!(rate >= 0.0)) rate = 0.0;
    if (rate > 1.0) rate = 1.0;
    const uint32_t percent = static_cast<uint32_t>(rate * 100.0);
    in_flight_sampling_rate_.store(std::min(percent, 100u), std::memory_order_relaxed);
}

double TaskMonitor::get_in_flight_sampling_rate() const {
    return in_flight_sampling_rate_.load(std::memory_order_relaxed) / 100.0;
}

void TaskMonitor::set_enabled(bool enabled) {
    enabled_.store(enabled, std::memory_order_relaxed);
    if (!enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        task_id_to_type_.clear();
        in_flight_tasks_.clear();
    }
}

bool TaskMonitor::is_enabled() const {
    return enabled_.load(std::memory_order_relaxed);
}

void TaskMonitor::set_sampling_rate(double rate) {
    // CR-074: 越界/NaN 直接做 double→uint32 转换是 UB（[conv.fpint]），
    // 实测负数会回绕成 100% 采样、NaN 变 0%。先钳制到 [0,1] 再转换。
    if (!(rate >= 0.0)) rate = 0.0;  // 同时处理负数与 NaN
    if (rate > 1.0) rate = 1.0;
    const uint32_t percent = static_cast<uint32_t>(rate * 100.0);
    sampling_rate_.store(percent, std::memory_order_relaxed);
}

double TaskMonitor::get_sampling_rate() const {
    return sampling_rate_.load(std::memory_order_relaxed) / 100.0;
}

bool TaskMonitor::should_sample() const {
    uint32_t rate = sampling_rate_.load(std::memory_order_relaxed);
    if (rate >= 100) return true;
    if (rate == 0) return false;
    uint64_t count = sample_counter_.fetch_add(1, std::memory_order_relaxed);
    return (count % 100) < rate;
}

bool TaskMonitor::should_sample_in_flight() const {
    const uint32_t rate = in_flight_sampling_rate_.load(std::memory_order_relaxed);
    if (rate >= 100) return true;
    if (rate == 0) return false;
    const uint64_t count = in_flight_sample_counter_.fetch_add(1, std::memory_order_relaxed);
    return (count % 100) < rate;
}

}  // namespace monitor
}  // namespace kairo
