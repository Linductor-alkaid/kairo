/**
 * Performance baseline benchmarks for executor.
 *
 * Covers: submission throughput, task round-trip latency, end-to-end throughput.
 * Config: defaults + env vars (KAIRO_BENCHMARK_*) + CLI (--json, --tasks, etc.).
 * Output: human-readable text (default) or JSON (--json or KAIRO_BENCHMARK_JSON=1).
 */

#include <kairo/executor.hpp>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

namespace {

// Defaults
constexpr size_t kDefaultSubmitTasks = 50000;
constexpr size_t kDefaultLatencyTasks = 10000;
constexpr size_t kDefaultE2ETasks = 50000;
constexpr size_t kDefaultMinThreads = 4;
constexpr size_t kDefaultMaxThreads = 8;
constexpr size_t kDefaultQueueCapacity = 10000;

struct Config {
    size_t num_tasks_submit = kDefaultSubmitTasks;
    size_t num_tasks_latency = kDefaultLatencyTasks;
    size_t num_tasks_e2e = kDefaultE2ETasks;
    size_t min_threads = kDefaultMinThreads;
    size_t max_threads = kDefaultMaxThreads;
    size_t queue_capacity = kDefaultQueueCapacity;
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
    const char* t = std::getenv("KAIRO_BENCHMARK_TASKS");
    if (t) {
        size_t v = parse_size_t(t, c.num_tasks_submit);
        c.num_tasks_submit = c.num_tasks_latency = c.num_tasks_e2e = v;
    }
    t = std::getenv("KAIRO_BENCHMARK_MIN_THREADS");
    if (t) c.min_threads = parse_size_t(t, c.min_threads);
    t = std::getenv("KAIRO_BENCHMARK_MAX_THREADS");
    if (t) c.max_threads = parse_size_t(t, c.max_threads);
    t = std::getenv("KAIRO_BENCHMARK_QUEUE_CAPACITY");
    if (t) c.queue_capacity = parse_size_t(t, c.queue_capacity);
    t = std::getenv("KAIRO_BENCHMARK_JSON");
    if (t && parse_bool_env(t)) c.json_output = true;
}

void parse_args(int argc, char* argv[], Config& c) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--json") {
            c.json_output = true;
            continue;
        }
        if (a == "--tasks" && i + 1 < argc) {
            c.num_tasks_submit = c.num_tasks_latency = c.num_tasks_e2e =
                parse_size_t(argv[++i], c.num_tasks_submit);
            continue;
        }
        if (a == "--min-threads" && i + 1 < argc) {
            c.min_threads = parse_size_t(argv[++i], c.min_threads);
            continue;
        }
        if (a == "--max-threads" && i + 1 < argc) {
            c.max_threads = parse_size_t(argv[++i], c.max_threads);
            continue;
        }
        if (a == "--queue-capacity" && i + 1 < argc) {
            c.queue_capacity = parse_size_t(argv[++i], c.queue_capacity);
            continue;
        }
    }
}

kairo::ExecutorConfig make_executor_config(const Config& c) {
    kairo::ExecutorConfig ec;
    ec.min_threads = c.min_threads;
    ec.max_threads = c.max_threads;
    ec.queue_capacity = c.queue_capacity;
    return ec;
}

struct LatencyStats {
    double min_us = 0;
    double avg_us = 0;
    double p50_us = 0;
    double p95_us = 0;
    double p99_us = 0;
};

LatencyStats compute_latency_stats(std::vector<double>& samples_us) {
    LatencyStats s;
    if (samples_us.empty()) return s;
    std::sort(samples_us.begin(), samples_us.end());
    const size_t n = samples_us.size();
    s.min_us = samples_us.front();
    s.avg_us = std::accumulate(samples_us.begin(), samples_us.end(), 0.0) / static_cast<double>(n);
    auto idx = [n](double p) -> size_t {
        size_t i = static_cast<size_t>(p * static_cast<double>(n - 1));
        return i >= n ? n - 1 : i;
    };
    s.p50_us = samples_us[idx(0.50)];
    s.p95_us = samples_us[idx(0.95)];
    s.p99_us = samples_us[idx(0.99)];
    return s;
}

void run_submission_throughput(const Config& cfg, bool json_only) {
    kairo::Executor ex;
    kairo::ExecutorConfig ec = make_executor_config(cfg);
    if (!ex.initialize(ec)) {
        std::cerr << "benchmark_baseline: initialize failed" << std::endl;
        std::exit(1);
    }

    std::vector<std::future<void>> futures;
    futures.reserve(cfg.num_tasks_submit);

    auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < cfg.num_tasks_submit; ++i) {
        futures.push_back(ex.submit([]() noexcept {}));
    }
    auto t1 = std::chrono::steady_clock::now();

    for (auto& f : futures) f.get();
    ex.wait_for_completion();
    ex.shutdown(true);

    auto submit_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double throughput = (submit_ms > 0) ? (static_cast<double>(cfg.num_tasks_submit) * 1000.0 / submit_ms) : 0;

    if (cfg.json_output) {
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "  {\"name\":\"submission_throughput\",\"config\":{"
                  << "\"num_tasks\":" << cfg.num_tasks_submit
                  << ",\"min_threads\":" << cfg.min_threads
                  << ",\"max_threads\":" << cfg.max_threads
                  << ",\"queue_capacity\":" << cfg.queue_capacity
                  << "},\"metrics\":{"
                  << "\"throughput_tasks_per_sec\":" << throughput
                  << ",\"submit_time_ms\":" << submit_ms << "}}";
        return;
    }
    if (json_only) return;
    std::cout << "--- Submission Throughput ---\n";
    std::cout << "  Tasks: " << cfg.num_tasks_submit
              << ", Config: min_threads=" << cfg.min_threads
              << " max_threads=" << cfg.max_threads
              << " queue_capacity=" << cfg.queue_capacity << "\n";
    std::cout << "  Submit time: " << submit_ms << " ms\n";
    std::cout << "  Submission throughput: " << throughput << " tasks/s\n";
}

void run_round_trip_latency(const Config& cfg, bool json_only) {
    kairo::Executor ex;
    kairo::ExecutorConfig ec = make_executor_config(cfg);
    if (!ex.initialize(ec)) {
        std::cerr << "benchmark_baseline: initialize failed" << std::endl;
        std::exit(1);
    }

    std::vector<std::future<void>> futures;
    futures.reserve(cfg.num_tasks_latency);
    for (size_t i = 0; i < cfg.num_tasks_latency; ++i) {
        futures.push_back(ex.submit([]() noexcept {}));
    }

    std::vector<double> latencies_us;
    latencies_us.reserve(cfg.num_tasks_latency);
    for (auto& f : futures) {
        auto t0 = std::chrono::steady_clock::now();
        f.get();
        auto t1 = std::chrono::steady_clock::now();
        latencies_us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
    }
    ex.wait_for_completion();
    ex.shutdown(true);

    LatencyStats s = compute_latency_stats(latencies_us);

    if (cfg.json_output) {
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "  {\"name\":\"round_trip_latency\",\"config\":{"
                  << "\"num_tasks\":" << cfg.num_tasks_latency
                  << ",\"min_threads\":" << cfg.min_threads
                  << ",\"max_threads\":" << cfg.max_threads
                  << ",\"queue_capacity\":" << cfg.queue_capacity
                  << "},\"metrics\":{\"latency_us\":{"
                  << "\"min\":" << s.min_us << ",\"avg\":" << s.avg_us
                  << ",\"p50\":" << s.p50_us << ",\"p95\":" << s.p95_us
                  << ",\"p99\":" << s.p99_us << "}}}";
        return;
    }
    if (json_only) return;
    std::cout << "--- Task Round-Trip Latency ---\n";
    std::cout << "  Tasks: " << cfg.num_tasks_latency
              << ", Config: min_threads=" << cfg.min_threads
              << " max_threads=" << cfg.max_threads
              << " queue_capacity=" << cfg.queue_capacity << "\n";
    std::cout << "  Latency (us): min=" << s.min_us << " avg=" << s.avg_us
              << " p50=" << s.p50_us << " p95=" << s.p95_us << " p99=" << s.p99_us << "\n";
}

void run_e2e_throughput(const Config& cfg, bool json_only) {
    kairo::Executor ex;
    kairo::ExecutorConfig ec = make_executor_config(cfg);
    if (!ex.initialize(ec)) {
        std::cerr << "benchmark_baseline: initialize failed" << std::endl;
        std::exit(1);
    }

    std::atomic<size_t> done{0};
    std::vector<std::future<void>> futures;
    futures.reserve(cfg.num_tasks_e2e);

    auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < cfg.num_tasks_e2e; ++i) {
        futures.push_back(ex.submit([&done]() noexcept {
            int x = 0;
            for (int j = 0; j < 100; ++j) x += j;
            (void)x;
            done.fetch_add(1);
        }));
    }
    for (auto& f : futures) f.get();
    ex.wait_for_completion();
    auto t1 = std::chrono::steady_clock::now();
    ex.shutdown(true);

    auto total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double throughput = (total_ms > 0) ? (static_cast<double>(cfg.num_tasks_e2e) * 1000.0 / total_ms) : 0;

    if (cfg.json_output) {
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "  {\"name\":\"e2e_throughput\",\"config\":{"
                  << "\"num_tasks\":" << cfg.num_tasks_e2e
                  << ",\"min_threads\":" << cfg.min_threads
                  << ",\"max_threads\":" << cfg.max_threads
                  << ",\"queue_capacity\":" << cfg.queue_capacity
                  << "},\"metrics\":{"
                  << "\"throughput_tasks_per_sec\":" << throughput
                  << ",\"total_time_ms\":" << total_ms << "}}";
        return;
    }
    if (json_only) return;
    std::cout << "--- End-to-End Throughput ---\n";
    std::cout << "  Tasks: " << cfg.num_tasks_e2e
              << ", Config: min_threads=" << cfg.min_threads
              << " max_threads=" << cfg.max_threads
              << " queue_capacity=" << cfg.queue_capacity << "\n";
    std::cout << "  Total time: " << total_ms << " ms\n";
    std::cout << "  E2E throughput: " << throughput << " tasks/s\n";
}

} // namespace

int main(int argc, char* argv[]) {
    Config cfg;
    apply_env(cfg);
    parse_args(argc, argv, cfg);

    const bool json_only = cfg.json_output;

    if (cfg.json_output) {
        std::cout << "{\"benchmarks\":[\n";
    } else {
        std::cout << "========================================\n";
        std::cout << "Executor Performance Baseline\n";
        std::cout << "========================================\n\n";
    }

    run_submission_throughput(cfg, json_only);
    if (cfg.json_output) std::cout << ",\n";
    else std::cout << "\n";

    run_round_trip_latency(cfg, json_only);
    if (cfg.json_output) std::cout << ",\n";
    else std::cout << "\n";

    run_e2e_throughput(cfg, json_only);

    if (cfg.json_output) {
        std::cout << "\n]}\n";
    } else {
        std::cout << "\n========================================\n";
        std::cout << "Baseline complete\n";
        std::cout << "========================================\n";
    }

    return 0;
}
