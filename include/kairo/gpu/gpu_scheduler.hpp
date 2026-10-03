#pragma once

#include <cstddef>
#include <shared_mutex>
#include <deque>
#include <chrono>

namespace kairo {
namespace gpu {

/**
 * @brief Task characteristics for scheduling decisions
 */
struct TaskCharacteristics {
    size_t data_size_bytes = 0;        ///< Total data size to process
    float compute_intensity = 1.0f;     ///< Compute intensity hint (1.0=baseline, >1.0=compute-heavy)
    bool prefer_gpu = false;            ///< User preference hint
};

/**
 * @brief Executor choice result
 */
enum class ExecutorChoice {
    CPU,    ///< Use CPU thread pool
    GPU     ///< Use GPU executor
};

/**
 * @brief Performance record for adaptive scheduling
 */
struct PerformanceRecord {
    size_t data_size_bytes;
    float compute_intensity;
    ExecutorChoice executor;
    double execution_time_ms;
};

/**
 * @brief GPU scheduler for automatic CPU/GPU task routing
 *
 * Uses heuristic-based decision making to choose between CPU and GPU execution.
 * Considers data size and compute intensity.
 */
class GpuScheduler {
public:
    /**
     * @brief Scheduler configuration
     */
    struct Config {
        size_t data_size_threshold = 1024 * 1024;  ///< Data size threshold (bytes, default 1MB)
        float compute_intensity_threshold = 2.0f;   ///< Compute intensity threshold (default 2.0x)
        bool enable_adaptive = false;               ///< Enable adaptive scheduling (default false)
        size_t history_size = 100;                  ///< Max performance history records (default 100)
    };

    /**
     * @brief Construct scheduler with default config
     */
    GpuScheduler();

    /**
     * @brief Construct scheduler with config
     */
    explicit GpuScheduler(const Config& config);

    /**
     * @brief Decide which executor to use
     *
     * Decision logic:
     * - If prefer_gpu hint is set, choose GPU
     * - If adaptive scheduling enabled and sufficient history exists, use prediction
     * - If data_size >= threshold AND compute_intensity >= threshold, choose GPU
     * - Otherwise, choose CPU
     *
     * @param characteristics Task characteristics
     * @return ExecutorChoice CPU or GPU
     */
    ExecutorChoice decide(const TaskCharacteristics& characteristics) const;

    /**
     * @brief Record task performance for adaptive scheduling
     * @param characteristics Task characteristics
     * @param executor Executor used
     * @param execution_time_ms Execution time in milliseconds
     */
    void record_performance(const TaskCharacteristics& characteristics,
                           ExecutorChoice executor,
                           double execution_time_ms);

    /**
     * @brief Predict execution time for given characteristics and executor
     * @param characteristics Task characteristics
     * @param executor Executor to predict for
     * @return Predicted execution time in ms, or -1.0 if no prediction available
     */
    double predict_time(const TaskCharacteristics& characteristics,
                       ExecutorChoice executor) const;

    /**
     * @brief Update scheduler configuration
     */
    void update_config(const Config& config);

    /**
     * @brief Get current configuration
     */
    Config get_config() const;

    /**
     * @brief Get number of performance records
     */
    size_t history_count() const;

    /**
     * @brief Clear performance history
     */
    void clear_history();

private:
    Config config_;
    mutable std::shared_mutex mutex_;
    std::deque<PerformanceRecord> history_;

    /// Find similar records by data size bucket and compute intensity range
    static bool is_similar(const TaskCharacteristics& a, const PerformanceRecord& b);
};

} // namespace gpu
} // namespace kairo
