#pragma once

#include "kairo/types.hpp"

#include <chrono>
#include <string>

namespace kairo::monitor {

/**
 * @brief Formats a lifecycle snapshot as stable, line-oriented diagnostic text.
 *
 * The output is intended for logs and support bundles. It contains no task
 * callable, user payload, communication payload, or exception_ptr data.
 */
std::string format_executor_snapshot(const ExecutorSnapshot& snapshot);

/** @brief Text export metrics for a single formatter invocation. */
struct ExecutorSnapshotTextMetrics {
    std::chrono::nanoseconds formatting_duration{0};
    size_t formatting_allocation_count = 0;
};

/** @brief Text plus per-call formatter metrics for performance baselines. */
struct ExecutorSnapshotTextExport {
    std::string text;
    ExecutorSnapshotTextMetrics metrics;
};

/**
 * @brief Formats a snapshot and records formatter-local allocation and duration metrics.
 *
 * The allocation count includes the formatter stream buffer and final output string;
 * it does not attempt to count allocations made by user code or snapshot collection.
 */
ExecutorSnapshotTextExport format_executor_snapshot_with_metrics(
    const ExecutorSnapshot& snapshot);

} // namespace kairo::monitor
