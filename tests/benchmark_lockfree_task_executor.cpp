#include <kairo/lockfree_task_executor.hpp>
#include <iostream>
#include <iomanip>
#include <chrono>
#include <vector>
#include <atomic>
#include <algorithm>
#include <numeric>
#include <cstdlib>

// Sanitizer 构建下延迟断言无意义（插桩放大概率 10-100 倍，P99 必然超限），
// benchmark 不作为 TSAN/UBSAN 门禁（issue #188）：测量照常执行并输出，但
// 断言降级为提示。gcc 定义 __SANITIZE_THREAD__，clang 走 __has_feature。
// 注意必须嵌套：gcc 下 __has_feature 不是宏，`#if` 表达式里出现
// __has_feature(...) 是语法错误，&& 短路救不了解析阶段。
#if defined(__SANITIZE_THREAD__)
#define KAIRO_BENCH_UNDER_SANITIZER 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
#define KAIRO_BENCH_UNDER_SANITIZER 1
#endif
#endif

using namespace kairo;
using namespace std::chrono;

struct BenchResult {
    double avg_latency_ns = 0;
    double p50_latency_ns = 0;
    double p99_latency_ns = 0;
    double throughput_ops_per_sec = 0;
};

BenchResult benchmark_task_submission(size_t num_tasks) {
    LockFreeTaskExecutor exec(8192);
    exec.start();

    std::atomic<size_t> completed{0};
    std::vector<double> latencies_ns;
    latencies_ns.reserve(num_tasks);

    auto t0 = steady_clock::now();

    for (size_t i = 0; i < num_tasks; ++i) {
        auto submit_start = steady_clock::now();
        exec.push_task([&completed]() {
            completed.fetch_add(1, std::memory_order_relaxed);
        });
        auto submit_end = steady_clock::now();
        latencies_ns.push_back(duration<double, std::nano>(submit_end - submit_start).count());
    }

    auto t1 = steady_clock::now();

    // 等待任务完成
    while (completed.load() < num_tasks) {
        std::this_thread::sleep_for(microseconds(100));
    }

    exec.stop();

    double total_ns = duration<double, std::nano>(t1 - t0).count();

    std::sort(latencies_ns.begin(), latencies_ns.end());
    size_t p50_idx = latencies_ns.size() / 2;
    size_t p99_idx = static_cast<size_t>(0.99 * latencies_ns.size());

    BenchResult result;
    result.avg_latency_ns = std::accumulate(latencies_ns.begin(), latencies_ns.end(), 0.0) / latencies_ns.size();
    result.p50_latency_ns = latencies_ns[p50_idx];
    result.p99_latency_ns = latencies_ns[p99_idx];
    result.throughput_ops_per_sec = num_tasks * 1e9 / total_ns;

    return result;
}

BenchResult benchmark_throughput(size_t num_tasks) {
    LockFreeTaskExecutor exec(8192);
    exec.start();

    std::atomic<size_t> completed{0};

    auto t0 = steady_clock::now();

    for (size_t i = 0; i < num_tasks; ++i) {
        while (!exec.push_task([&completed]() {
            completed.fetch_add(1, std::memory_order_relaxed);
        })) {
            // 重试
        }
    }

    while (completed.load() < num_tasks) {
        std::this_thread::sleep_for(microseconds(10));
    }

    auto t1 = steady_clock::now();

    exec.stop();

    double total_ns = duration<double, std::nano>(t1 - t0).count();

    BenchResult result;
    result.throughput_ops_per_sec = num_tasks * 1e9 / total_ns;

    return result;
}

// latency_single_task: measures submit-to-execution latency for isolated tasks.
// Asserts P99 < 100µs — confirms the hybrid backoff reduces worst-case idle delay
// from the original 100µs busy-sleep baseline. The 100µs ceiling matches the
// pre-P-003 sleep duration, so the test guards against regression to the slower
// behavior. Note: actual P99 on 2-vCPU CI runners has been observed at ~58µs
// (within the budget); 100µs leaves headroom for hardware variance.
static bool latency_single_task() {
    constexpr size_t NUM_SAMPLES = 500;
    constexpr double P99_LIMIT_US = 100.0;

    LockFreeTaskExecutor exec(8192);
    exec.start();

    std::vector<double> latencies_us;
    latencies_us.reserve(NUM_SAMPLES);

    for (size_t i = 0; i < NUM_SAMPLES; ++i) {
        // Drain the executor — ensure it is idle before each sample
        std::this_thread::sleep_for(microseconds(200));

        std::atomic<bool> done{false};
        auto submit_time = steady_clock::now();

        exec.push_task([&done, &submit_time]() {
            auto exec_time = steady_clock::now();
            (void)exec_time;  // latency computed after done is set
            done.store(true, std::memory_order_release);
        });

        // Spin-wait — timeout 10ms
        auto deadline = steady_clock::now() + milliseconds(10);
        while (!done.load(std::memory_order_acquire)) {
            if (steady_clock::now() > deadline) {
                std::cerr << "[latency_single_task] TIMEOUT waiting for task " << i << "\n";
                exec.stop();
                return false;
            }
            std::this_thread::yield();
        }

        auto finish_time = steady_clock::now();
        double us = duration<double, std::micro>(finish_time - submit_time).count();
        latencies_us.push_back(us);
    }

    exec.stop();

    std::sort(latencies_us.begin(), latencies_us.end());
    size_t p50_idx = latencies_us.size() / 2;
    size_t p99_idx = static_cast<size_t>(0.99 * (latencies_us.size() - 1));
    double p50 = latencies_us[p50_idx];
    double p99 = latencies_us[p99_idx];

    std::cout << "\n[latency_single_task]\n";
    std::cout << "  Samples : " << NUM_SAMPLES << "\n";
    std::cout << "  P50     : " << std::fixed << std::setprecision(2) << p50 << " µs\n";
    std::cout << "  P99     : " << p99 << " µs  (limit: " << P99_LIMIT_US << " µs)\n";

    if (p99 >= P99_LIMIT_US) {
#ifdef KAIRO_BENCH_UNDER_SANITIZER
        std::cerr << "[latency_single_task] SKIP: P99 " << p99
                  << " µs >= " << P99_LIMIT_US
                  << " µs, but latency assertions are exempt under sanitizers\n";
#else
        std::cerr << "[latency_single_task] FAIL: P99 " << p99
                  << " µs >= " << P99_LIMIT_US << " µs\n";
        return false;
#endif
    }
    std::cout << "  PASS\n";
    return true;
}

int main() {
    std::cout << "========================================\n";
    std::cout << "LockFreeTaskExecutor Performance Benchmark\n";
    std::cout << "========================================\n\n";

    std::cout << std::fixed << std::setprecision(2);

    // 测试1: 任务提交延迟
    std::cout << "Test 1: Task Submission Latency (10,000 tasks)\n";
    auto result1 = benchmark_task_submission(10000);
    std::cout << "  Average latency: " << result1.avg_latency_ns << " ns\n";
    std::cout << "  p50 latency: " << result1.p50_latency_ns << " ns\n";
    std::cout << "  p99 latency: " << result1.p99_latency_ns << " ns\n";
    std::cout << "  Throughput: " << result1.throughput_ops_per_sec << " ops/s\n\n";

    // 测试2: 吞吐量
    std::cout << "Test 2: Throughput (100,000 tasks)\n";
    auto result2 = benchmark_throughput(100000);
    std::cout << "  Throughput: " << result2.throughput_ops_per_sec << " ops/s\n\n";

    std::cout << "========================================\n";
    std::cout << "Benchmark complete\n";
    std::cout << "========================================\n";

    // Latency assertion test — must pass for CI green
    if (!latency_single_task()) {
        return EXIT_FAILURE;
    }

    return 0;
}
