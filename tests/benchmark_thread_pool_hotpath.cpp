/**
 * Thread-pool submission hot-path benchmark (P1 of the 2026-09 performance
 * audit convergence plan).
 *
 * Measures, directly against ThreadPool (no facade layer):
 *   1. Multi-producer submission throughput (1..16 concurrent producers).
 *   2. Wake-to-execute latency distribution: time from try_submit() to the
 *      first instruction of the task body, sampled into per-producer
 *      preallocated slots (no shared-write contention on the sampling path).
 *
 * Config: env KAIRO_BENCHMARK_* + CLI (--json, --tasks, --producers).
 * Output: human-readable text (default) or JSON lines (--json).
 */

#include "kairo/thread_pool/thread_pool.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <iomanip>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

using kairo::ThreadPool;
using kairo::ThreadPoolConfig;

namespace {

constexpr size_t kDefaultTasksPerProducer = 20000;
constexpr std::array<unsigned, 6> kDefaultProducerScales = {1, 2, 4, 8, 12, 16};
constexpr unsigned kDefaultWorkers = 8;

struct Config {
    size_t tasks_per_producer = kDefaultTasksPerProducer;
    unsigned workers = kDefaultWorkers;
    bool json_output = false;
};

size_t parse_size_t(const char* s, size_t default_val) {
    if (!s || !*s) return default_val;
    try {
        unsigned long v = std::stoul(s);
        return static_cast<size_t>(v);
    } catch (...) {
        return default_val;
    }
}

bool parse_bool_env(const char* s) {
    if (!s || !*s) return false;
    return s[0] == '1' || s[0] == 't' || s[0] == 'T' || s[0] == 'y' || s[0] == 'Y';
}

void apply_env(Config& c) {
    if (const char* t = std::getenv("KAIRO_BENCHMARK_TASKS")) {
        c.tasks_per_producer = parse_size_t(t, c.tasks_per_producer);
    }
    if (const char* t = std::getenv("KAIRO_BENCHMARK_WORKERS")) {
        unsigned v = static_cast<unsigned>(parse_size_t(t, c.workers));
        if (v > 0) c.workers = v;
    }
    if (const char* t = std::getenv("KAIRO_BENCHMARK_JSON")) {
        if (parse_bool_env(t)) c.json_output = true;
    }
}

struct LatencyStats {
    double avg_us = 0;
    double p50_us = 0;
    double p95_us = 0;
    double p99_us = 0;
    double max_us = 0;
};

LatencyStats compute_latency_stats(std::vector<double>& samples_us) {
    LatencyStats s;
    if (samples_us.empty()) return s;
    std::sort(samples_us.begin(), samples_us.end());
    const size_t n = samples_us.size();
    s.avg_us = std::accumulate(samples_us.begin(), samples_us.end(), 0.0) /
               static_cast<double>(n);
    auto idx = [n](double p) -> size_t {
        size_t i = static_cast<size_t>(p * static_cast<double>(n - 1));
        return i >= n ? n - 1 : i;
    };
    s.p50_us = samples_us[idx(0.50)];
    s.p95_us = samples_us[idx(0.95)];
    s.p99_us = samples_us[idx(0.99)];
    s.max_us = samples_us.back();
    return s;
}

void run_producer_scale(const Config& cfg, unsigned producers) {
    const size_t total_tasks = cfg.tasks_per_producer * producers;

    ThreadPoolConfig pool_cfg;
    pool_cfg.min_threads = cfg.workers;
    pool_cfg.max_threads = cfg.workers;
    ThreadPool pool;
    if (!pool.initialize(pool_cfg)) {
        std::fprintf(stderr, "benchmark_thread_pool_hotpath: initialize failed\n");
        std::exit(1);
    }

    // ---- Phase 1: saturated multi-producer submission throughput ----
    std::atomic<size_t> done{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> producers_threads;
    producers_threads.reserve(producers);

    const auto t0 = std::chrono::steady_clock::now();
    for (unsigned p = 0; p < producers; ++p) {
        producers_threads.emplace_back([&]() {
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (size_t i = 0; i < cfg.tasks_per_producer; ++i) {
                pool.try_submit([&done]() noexcept {
                    done.fetch_add(1, std::memory_order_relaxed);
                });
            }
        });
    }
    go.store(true, std::memory_order_release);
    for (auto& t : producers_threads) t.join();
    const auto t1 = std::chrono::steady_clock::now();

    pool.wait_for_completion();
    const auto t2 = std::chrono::steady_clock::now();

    if (done.load() != total_tasks) {
        std::fprintf(stderr,
                     "benchmark_thread_pool_hotpath: task loss detected (%zu/%zu)\n",
                     done.load(), total_tasks);
        std::exit(1);
    }

    const double submit_ms =
        std::chrono::duration<double, std::milli>(t1 - t0).count();
    const double throughput =
        submit_ms > 0 ? static_cast<double>(total_tasks) * 1000.0 / submit_ms : 0;
    const double drain_ms =
        std::chrono::duration<double, std::milli>(t2 - t0).count();

    // ---- Phase 2: wake-to-exec latency distribution ----
    // Spaced submissions let the pool drain and workers re-park between
    // samples, so the measured delta is dominated by the wake path
    // (generation bump -> futex notify -> worker scan -> task body) rather
    // than by queueing behind a saturated backlog.
    constexpr size_t kLatencySamplesPerProducer = 200;
    constexpr auto kSampleSpacing = std::chrono::microseconds(200);
    std::atomic<size_t> latency_done{0};

    // 每个样本一个预分配槽位，由 (producer, sample index) 唯一寻址：任务闭包
    // 只写自己独占的槽，采样路径上没有任何共享容器写入（issue #194：多个
    // worker 并发 push_back 同一 per-producer vector 是 harness 数据竞争）。
    // 槽位在 shutdown(true) join 全部 worker 之后才被单线程读取。
    const size_t latency_total = kLatencySamplesPerProducer * producers;
    std::vector<double> slots(latency_total, 0.0);

    std::vector<std::thread> latency_producers;
    latency_producers.reserve(producers);
    for (unsigned p = 0; p < producers; ++p) {
        latency_producers.emplace_back([&, p]() {
            for (size_t i = 0; i < kLatencySamplesPerProducer; ++i) {
                const auto submit_time = std::chrono::steady_clock::now();
                double* slot = &slots[p * kLatencySamplesPerProducer + i];
                pool.try_submit([&latency_done, slot, submit_time]() noexcept {
                    const auto exec_time = std::chrono::steady_clock::now();
                    *slot = std::chrono::duration<double, std::micro>(
                                exec_time - submit_time)
                                .count();
                    latency_done.fetch_add(1, std::memory_order_relaxed);
                });
                std::this_thread::sleep_for(kSampleSpacing);
            }
        });
    }
    for (auto& t : latency_producers) t.join();
    pool.wait_for_completion();
    pool.shutdown(true);

    if (latency_done.load() != latency_total) {
        std::fprintf(stderr,
                     "benchmark_thread_pool_hotpath: latency task loss (%zu/%zu)\n",
                     latency_done.load(), latency_total);
        std::exit(1);
    }

    LatencyStats lat = compute_latency_stats(slots);

    if (cfg.json_output) {
        std::printf(
            "  {\"name\":\"multi_producer_hotpath\",\"config\":{"
            "\"producers\":%u,\"workers\":%u,\"tasks_per_producer\":%zu,"
            "\"total_tasks\":%zu},\"metrics\":{"
            "\"submission_throughput_tasks_per_sec\":%.2f,"
            "\"submit_time_ms\":%.2f,\"drain_time_ms\":%.2f,"
            "\"wake_to_exec_us\":{\"avg\":%.3f,\"p50\":%.3f,\"p95\":%.3f,"
            "\"p99\":%.3f,\"max\":%.3f}}}",
            producers, cfg.workers, cfg.tasks_per_producer, total_tasks,
            throughput, submit_ms, drain_ms, lat.avg_us, lat.p50_us, lat.p95_us,
            lat.p99_us, lat.max_us);
    } else {
        std::printf("--- Producers: %u x %zu tasks (workers=%u) ---\n", producers,
                    cfg.tasks_per_producer, cfg.workers);
        std::printf(
            "  Submission throughput: %.0f tasks/s (submit %.2f ms, drain %.2f ms)\n",
            throughput, submit_ms, drain_ms);
        std::printf(
            "  Wake-to-exec latency (us): avg=%.3f p50=%.3f p95=%.3f p99=%.3f max=%.3f\n",
            lat.avg_us, lat.p50_us, lat.p95_us, lat.p99_us, lat.max_us);
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    Config cfg;
    apply_env(cfg);
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--json") {
            cfg.json_output = true;
        } else if (a == "--tasks" && i + 1 < argc) {
            cfg.tasks_per_producer = parse_size_t(argv[++i], cfg.tasks_per_producer);
        } else if (a == "--workers" && i + 1 < argc) {
            unsigned v = static_cast<unsigned>(parse_size_t(argv[++i], cfg.workers));
            if (v > 0) cfg.workers = v;
        }
    }

    if (cfg.json_output) {
        std::printf("{\"benchmarks\":[\n");
    } else {
        std::printf("========================================\n");
        std::printf("ThreadPool Submission Hot-Path Benchmark\n");
        std::printf("========================================\n\n");
    }

    bool first = true;
    for (unsigned producers : kDefaultProducerScales) {
        if (cfg.json_output && !first) std::printf(",\n");
        run_producer_scale(cfg, producers);
        first = false;
    }

    if (cfg.json_output) {
        std::printf("\n]}\n");
    } else {
        std::printf("\n========================================\n");
        std::printf("Hot-path benchmark complete\n");
        std::printf("========================================\n");
    }
    return 0;
}
