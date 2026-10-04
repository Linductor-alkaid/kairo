/**
 * Lifecycle snapshot performance baseline.
 *
 * Measures an idle, initialized Executor. It is a comparison baseline, not a
 * latency guarantee: host load, build flags, backend registrations, and the
 * number of retained failure/statistics entries affect the result.
 */

#include <kairo/executor.hpp>
#include <kairo/monitor/executor_snapshot_formatter.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>

namespace {

constexpr size_t kDefaultIterations = 1000;

size_t parse_iterations(int argc, char* argv[]) {
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::string(argv[index]) == "--iterations") {
            try {
                return std::max<size_t>(1, std::stoull(argv[index + 1]));
            } catch (...) {
                return kDefaultIterations;
            }
        }
    }
    return kDefaultIterations;
}

double average_ns(std::chrono::nanoseconds total, size_t iterations) {
    return static_cast<double>(total.count()) / static_cast<double>(iterations);
}

} // namespace

int main(int argc, char* argv[]) {
    const size_t iterations = parse_iterations(argc, argv);
    kairo::Executor executor;
    kairo::ExecutorConfig config;
    config.min_threads = 1;
    config.max_threads = 1;
    if (!executor.initialize(config)) {
        std::cerr << "benchmark_lifecycle_snapshot: initialize failed\n";
        return 1;
    }

    // Warm up lazy standard-library buffers and the executor status path.
    const auto warm_snapshot = executor.get_snapshot();
    (void)kairo::monitor::format_executor_snapshot_with_metrics(warm_snapshot);

    std::chrono::nanoseconds collection_wall_time{0};
    std::chrono::nanoseconds collection_reported_time{0};
    kairo::ExecutorSnapshot snapshot;
    for (size_t index = 0; index < iterations; ++index) {
        const auto start = std::chrono::steady_clock::now();
        snapshot = executor.get_snapshot();
        collection_wall_time += std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start);
        collection_reported_time += snapshot.collection_duration;
        std::atomic_signal_fence(std::memory_order_seq_cst);
    }

    std::chrono::nanoseconds formatting_wall_time{0};
    std::chrono::nanoseconds formatting_reported_time{0};
    size_t formatting_allocations = 0;
    size_t formatted_bytes = 0;
    for (size_t index = 0; index < iterations; ++index) {
        const auto start = std::chrono::steady_clock::now();
        auto export_result = kairo::monitor::format_executor_snapshot_with_metrics(snapshot);
        formatting_wall_time += std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start);
        formatting_reported_time += export_result.metrics.formatting_duration;
        formatting_allocations += export_result.metrics.formatting_allocation_count;
        formatted_bytes = export_result.text.size();
        std::atomic_signal_fence(std::memory_order_seq_cst);
    }
    executor.shutdown();

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "{\n";
    std::cout << "  \"benchmark\": \"lifecycle_snapshot\",\n";
    std::cout << "  \"scenario\": \"idle_initialized_async\",\n";
    std::cout << "  \"iterations\": " << iterations << ",\n";
    std::cout << "  \"collection\": {\n";
    std::cout << "    \"wall_avg_ns\": " << average_ns(collection_wall_time, iterations) << ",\n";
    std::cout << "    \"reported_avg_ns\": " << average_ns(collection_reported_time, iterations) << "\n";
    std::cout << "  },\n";
    std::cout << "  \"formatting\": {\n";
    std::cout << "    \"wall_avg_ns\": " << average_ns(formatting_wall_time, iterations) << ",\n";
    std::cout << "    \"reported_avg_ns\": " << average_ns(formatting_reported_time, iterations) << ",\n";
    std::cout << "    \"local_allocation_avg\": "
              << static_cast<double>(formatting_allocations) / static_cast<double>(iterations) << ",\n";
    std::cout << "    \"output_bytes\": " << formatted_bytes << "\n";
    std::cout << "  }\n";
    std::cout << "}\n";
    return 0;
}
