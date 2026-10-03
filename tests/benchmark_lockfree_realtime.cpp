/**
 * Benchmark for P0-1: Lock-free realtime task passing optimization
 * Measures task submission latency and throughput improvements
 */

#include <kairo/executor.hpp>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <vector>
#include <atomic>
#include <algorithm>
#include <numeric>

using namespace kairo;
using namespace std::chrono;

struct BenchResult {
    double avg_latency_ns = 0;
    double p99_latency_ns = 0;
    double throughput_ops_per_sec = 0;
};

BenchResult benchmark_realtime_task_submission() {
    const size_t num_tasks = 10000;
    const int64_t cycle_period_ns = 1000000;  // 1ms

    Executor ex;
    ExecutorConfig cfg;
    cfg.min_threads = 2;
    cfg.max_threads = 2;
    ex.initialize(cfg);

    std::atomic<size_t> completed{0};

    RealtimeThreadConfig rt_cfg;
    rt_cfg.thread_name = "bench_rt";
    rt_cfg.cycle_period_ns = cycle_period_ns;
    rt_cfg.thread_priority = 0;
    rt_cfg.cycle_callback = [&]() noexcept {
        // Cycle callback
    };

    ex.register_realtime_task("bench_rt", rt_cfg);
    ex.start_realtime_task("bench_rt");

    auto* rt_exec = ex.get_realtime_executor("bench_rt");
    if (!rt_exec) {
        std::cerr << "Failed to get realtime executor\n";
        return BenchResult{};
    }

    std::vector<double> latencies_ns;
    latencies_ns.reserve(num_tasks);

    auto t0 = steady_clock::now();
    for (size_t i = 0; i < num_tasks; ++i) {
        auto submit_start = steady_clock::now();
        rt_exec->push_task([&completed]() noexcept {
            completed.fetch_add(1, std::memory_order_relaxed);
        });
        auto submit_end = steady_clock::now();
        latencies_ns.push_back(duration<double, std::nano>(submit_end - submit_start).count());
    }
    auto t1 = steady_clock::now();

    // Wait for tasks to complete
    std::this_thread::sleep_for(milliseconds(100));

    ex.stop_realtime_task("bench_rt");
    ex.shutdown(true);

    double total_ns = duration<double, std::nano>(t1 - t0).count();

    std::sort(latencies_ns.begin(), latencies_ns.end());
    size_t p99_idx = static_cast<size_t>(0.99 * static_cast<double>(latencies_ns.size()));

    BenchResult result;
    result.avg_latency_ns = std::accumulate(latencies_ns.begin(), latencies_ns.end(), 0.0) / latencies_ns.size();
    result.p99_latency_ns = latencies_ns[p99_idx];
    result.throughput_ops_per_sec = num_tasks * 1e9 / total_ns;

    return result;
}

int main() {
    std::cout << "========================================\n";
    std::cout << "Lock-Free Realtime Task Passing Benchmark\n";
    std::cout << "P0-1 Optimization Verification\n";
    std::cout << "========================================\n\n";

    auto result = benchmark_realtime_task_submission();

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Realtime Task Submission:\n";
    std::cout << "  Average latency: " << result.avg_latency_ns << " ns\n";
    std::cout << "  p99 latency: " << result.p99_latency_ns << " ns\n";
    std::cout << "  Throughput: " << result.throughput_ops_per_sec << " ops/s\n";

    std::cout << "\n========================================\n";
    std::cout << "Benchmark complete\n";
    std::cout << "========================================\n";

    return 0;
}
