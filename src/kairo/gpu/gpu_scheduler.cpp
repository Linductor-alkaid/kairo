#include "kairo/gpu/gpu_scheduler.hpp"
#include <mutex>
#include <cmath>
#include <algorithm>

namespace kairo {
namespace gpu {

GpuScheduler::GpuScheduler()
    : config_() {}

GpuScheduler::GpuScheduler(const Config& config)
    : config_(config) {}

bool GpuScheduler::is_similar(const TaskCharacteristics& a, const PerformanceRecord& b) {
    // Data size within 2x range
    double ratio = (b.data_size_bytes == 0) ? 0.0
        : static_cast<double>(a.data_size_bytes) / static_cast<double>(b.data_size_bytes);
    if (ratio < 0.5 || ratio > 2.0) return false;

    // Compute intensity within 50% range
    float diff = std::fabs(a.compute_intensity - b.compute_intensity);
    float max_val = std::max(a.compute_intensity, b.compute_intensity);
    if (max_val > 0.0f && diff / max_val > 0.5f) return false;

    return true;
}

ExecutorChoice GpuScheduler::decide(const TaskCharacteristics& characteristics) const {
    std::shared_lock lock(mutex_);

    // User preference hint always wins
    if (characteristics.prefer_gpu) {
        return ExecutorChoice::GPU;
    }

    // Adaptive scheduling: use historical data if enabled and available
    if (config_.enable_adaptive && !history_.empty()) {
        double cpu_total = 0.0, gpu_total = 0.0;
        int cpu_count = 0, gpu_count = 0;

        for (const auto& record : history_) {
            if (!is_similar(characteristics, record)) continue;
            if (record.executor == ExecutorChoice::CPU) {
                cpu_total += record.execution_time_ms;
                ++cpu_count;
            } else {
                gpu_total += record.execution_time_ms;
                ++gpu_count;
            }
        }

        // Need at least 2 records for each executor to make a prediction
        if (cpu_count >= 2 && gpu_count >= 2) {
            double cpu_avg = cpu_total / cpu_count;
            double gpu_avg = gpu_total / gpu_count;
            return (gpu_avg < cpu_avg) ? ExecutorChoice::GPU : ExecutorChoice::CPU;
        }
    }

    // Fallback: heuristic-based decision
    bool large_data = characteristics.data_size_bytes >= config_.data_size_threshold;
    bool compute_heavy = characteristics.compute_intensity >= config_.compute_intensity_threshold;

    if (large_data && compute_heavy) {
        return ExecutorChoice::GPU;
    }

    return ExecutorChoice::CPU;
}

void GpuScheduler::record_performance(const TaskCharacteristics& characteristics,
                                      ExecutorChoice executor,
                                      double execution_time_ms) {
    std::unique_lock lock(mutex_);
    history_.push_back({characteristics.data_size_bytes,
                        characteristics.compute_intensity,
                        executor, execution_time_ms});
    while (history_.size() > config_.history_size) {
        history_.pop_front();
    }
}

double GpuScheduler::predict_time(const TaskCharacteristics& characteristics,
                                  ExecutorChoice executor) const {
    std::shared_lock lock(mutex_);
    double total = 0.0;
    int count = 0;

    for (const auto& record : history_) {
        if (record.executor != executor) continue;
        if (!is_similar(characteristics, record)) continue;
        total += record.execution_time_ms;
        ++count;
    }

    return (count >= 2) ? total / count : -1.0;
}

void GpuScheduler::update_config(const Config& config) {
    std::unique_lock lock(mutex_);
    config_ = config;
    // Trim history if new size is smaller
    while (history_.size() > config_.history_size) {
        history_.pop_front();
    }
}

GpuScheduler::Config GpuScheduler::get_config() const {
    std::shared_lock lock(mutex_);
    return config_;
}

size_t GpuScheduler::history_count() const {
    std::shared_lock lock(mutex_);
    return history_.size();
}

void GpuScheduler::clear_history() {
    std::unique_lock lock(mutex_);
    history_.clear();
}

} // namespace gpu
} // namespace kairo
