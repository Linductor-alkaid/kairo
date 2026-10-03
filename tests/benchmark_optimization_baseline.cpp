/**
 * Optimization baseline benchmarks for v0.2.0
 * Measures current performance of optimization targets
 */

#include <kairo/executor.hpp>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <vector>
#include <atomic>

using namespace kairo;
using namespace std::chrono;

struct BenchResult {
    std::string name;
    double throughput_ops_per_sec = 0;
    double avg_latency_ns = 0;
};

// P1-4: Individual task submission throughput
BenchResult benchmark_submission_throughput() {
    const size_t num_tasks = 50000;
    Executor ex;
    ExecutorConfig cfg;
    cfg.min_threads = 4;
    cfg.max_threads = 8;
    cfg.enable_monitoring = false;
    ex.initialize(cfg);

    std::vector<std::future<void>> futures;
    futures.reserve(num_tasks);

    auto t0 = steady_clock::now();
    for (size_t i = 0; i < num_tasks; ++i) {
        futures.push_back(ex.submit([]() noexcept {}));
    }
    auto t1 = steady_clock::now();

    for (auto& f : futures) f.get();
    ex.shutdown(true);

    double total_ns = duration<double, std::nano>(t1 - t0).count();

    BenchResult result;
    result.name = "task_submission_throughput";
    result.throughput_ops_per_sec = num_tasks * 1e9 / total_ns;
    result.avg_latency_ns = total_ns / num_tasks;
    return result;
}

// P1-6: Monitoring overhead measurement
BenchResult benchmark_monitoring_overhead() {
    const size_t num_tasks = 50000;
    Executor ex;
    ExecutorConfig cfg;
    cfg.min_threads = 4;
    cfg.max_threads = 8;
    cfg.enable_monitoring = true;
    ex.initialize(cfg);

    std::vector<std::future<void>> futures;
    futures.reserve(num_tasks);

    auto t0 = steady_clock::now();
    for (size_t i = 0; i < num_tasks; ++i) {
        futures.push_back(ex.submit([]() noexcept {}));
    }
    auto t1 = steady_clock::now();

    for (auto& f : futures) f.get();
    ex.shutdown(true);

    double total_ns = duration<double, std::nano>(t1 - t0).count();

    BenchResult result;
    result.name = "with_monitoring_enabled";
    result.throughput_ops_per_sec = num_tasks * 1e9 / total_ns;
    result.avg_latency_ns = total_ns / num_tasks;
    return result;
}

void print_result(const BenchResult& r) {
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "  " << std::setw(35) << std::left << r.name << ": "
              << std::setw(12) << std::right << r.throughput_ops_per_sec << " ops/s, "
              << std::setw(8) << r.avg_latency_ns << " ns avg\n";
}

int main() {
    std::cout << "========================================\n";
    std::cout << "Optimization Baseline Benchmarks v0.2.0\n";
    std::cout << "========================================\n\n";

    std::cout << "Task Submission Performance:\n";
    auto r1 = benchmark_submission_throughput();
    print_result(r1);

    std::cout << "\nMonitoring Overhead:\n";
    auto r2 = benchmark_monitoring_overhead();
    print_result(r2);

    double overhead_percent = ((r2.avg_latency_ns - r1.avg_latency_ns) / r1.avg_latency_ns) * 100.0;
    std::cout << "  Monitoring overhead: " << std::fixed << std::setprecision(1)
              << overhead_percent << "%\n";

    std::cout << "\n========================================\n";
    std::cout << "Baseline complete\n";
    std::cout << "========================================\n";

    return 0;
}
