#include "kairo/task_inference.hpp"

// 0.7.1 §3.0.2 任务特征自动推断历史表实现。
// 设计语义见 docs/design/scheduling_runtime.md §13。

#include <algorithm>
#include <cmath>

namespace kairo {

namespace detail {

namespace {

constexpr double kDurationAlpha = 0.25;  // 执行时长 EWMA 系数
constexpr double kIntervalBeta = 0.25;   // 间隔 EWMA/EW 方差系数

}  // namespace

bool TaskInferenceTable::name_matches(
    const std::string& name,
    std::initializer_list<const char*> patterns) {
    if (name.empty()) {
        return false;
    }
    return std::any_of(patterns.begin(), patterns.end(),
                       [&name](const char* pattern) {
                           return name.find(pattern) != std::string::npos;
                       });
}

void TaskInferenceTable::record(const SchedulingFeedback& feedback,
                                int64_t now_ns) noexcept {
    // 反馈 task_id 在 submit_auto 路径即任务名；其余路径（如依赖图等）
    // 的合成 id 不构成有意义的推断键，但记录无害（键空间有界）。
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(feedback.task_id);
    if (it == entries_.end()) {
        if (entries_.size() >= kMaxTrackedNames) {
            ++dropped_names_;
            return;
        }
        it = entries_.emplace(feedback.task_id, Entry{}).first;
    }
    Entry& entry = it->second;

    const double duration =
        static_cast<double>(std::max<int64_t>(feedback.execution_duration_ns, 0));
    entry.duration_ewma_ns = entry.samples == 0
                                 ? duration
                                 : (1.0 - kDurationAlpha) * entry.duration_ewma_ns +
                                       kDurationAlpha * duration;
    ++entry.samples;

    if (entry.last_completion_ns != 0) {
        const double interval =
            static_cast<double>(std::max<int64_t>(now_ns - entry.last_completion_ns, 0));
        if (entry.interval_samples == 0) {
            entry.interval_ewma_ns = interval;
        } else {
            const double delta = interval - entry.interval_ewma_ns;
            entry.interval_ewma_ns += kIntervalBeta * delta;
            entry.interval_ew_variance =
                (1.0 - kIntervalBeta) *
                (entry.interval_ew_variance + kIntervalBeta * delta * delta);
        }
        ++entry.interval_samples;
    }
    entry.last_completion_ns = now_ns;
}

TaskCharacteristics TaskInferenceTable::analyze(const std::string& task_name,
                                                int64_t) const {
    TaskCharacteristics characteristics;
    characteristics.is_blocking_io =
        name_matches(task_name, {"_io_", "_read_", "_write_"});
    characteristics.needs_low_latency =
        name_matches(task_name, {"_realtime_", "_control_"});

    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = entries_.find(task_name);
    if (it != entries_.end()) {
        const Entry& entry = it->second;
        characteristics.samples = static_cast<size_t>(entry.samples);
        characteristics.estimated_duration_ns =
            static_cast<size_t>(std::max<double>(entry.duration_ewma_ns, 0.0));
        if (entry.interval_samples >= kMinIntervalSamples &&
            entry.interval_ewma_ns > 0.0) {
            const double stddev = std::sqrt(
                std::max<double>(entry.interval_ew_variance, 0.0));
            characteristics.is_periodic =
                stddev / entry.interval_ewma_ns < kPeriodicRegularRatio;
        }
    }
    return characteristics;
}

bool TaskInferenceTable::knows(const std::string& task_name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.find(task_name) != entries_.end();
}

uint64_t TaskInferenceTable::dropped_names() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return dropped_names_;
}

}  // namespace detail

}  // namespace kairo
