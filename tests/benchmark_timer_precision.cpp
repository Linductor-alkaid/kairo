/**
 * Timer precision benchmark for Executor delayed and periodic tasks.
 *
 * Measures jitter (actual - expected) in microseconds for multiple periods.
 * Periods: 1, 5, 10, 50, 100 ms. Output: human-readable or JSON (--json).
 */

#include <kairo/executor.hpp>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <numeric>
#include <string>
#include <vector>

namespace {

using clock = std::chrono::steady_clock;

const std::vector<int64_t> kDefaultPeriodsMs = {1, 5, 10, 50, 100};
constexpr size_t kDefaultTasksPerPeriod = 50;
constexpr size_t kDefaultCyclesPerPeriod = 20;
constexpr size_t kDefaultMinThreads = 2;
constexpr size_t kDefaultMaxThreads = 4;
constexpr size_t kDefaultQueueCapacity = 10000;

struct Config {
    std::vector<int64_t> periods_ms = kDefaultPeriodsMs;
    size_t tasks_per_period = kDefaultTasksPerPeriod;
    size_t cycles_per_period = kDefaultCyclesPerPeriod;
    size_t min_threads = kDefaultMinThreads;
    size_t max_threads = kDefaultMaxThreads;
    size_t queue_capacity = kDefaultQueueCapacity;
    bool json_output = false;
};

size_t parse_size_t(const char* s, size_t default_val) {
    if (!s || !*s) return default_val;
    try {
        return static_cast<size_t>(std::stoul(s));
    } catch (...) {
        return default_val;
    }
}

bool parse_bool_env(const char* s) {
    if (!s || !*s) return false;
    return s[0] == '1' || s[0] == 't' || s[0] == 'T' || s[0] == 'y' || s[0] == 'Y';
}

void apply_env(Config& c) {
    const char* t = std::getenv("KAIRO_BENCHMARK_JSON");
    if (t && parse_bool_env(t)) c.json_output = true;
}

void parse_args(int argc, char* argv[], Config& c) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--json") {
            c.json_output = true;
            continue;
        }
        if (a == "--tasks-per-period" && i + 1 < argc) {
            c.tasks_per_period = parse_size_t(argv[++i], c.tasks_per_period);
            continue;
        }
        if (a == "--cycles-per-period" && i + 1 < argc) {
            c.cycles_per_period = parse_size_t(argv[++i], c.cycles_per_period);
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

struct JitterStats {
    double min_us = 0;
    double avg_us = 0;
    double p50_us = 0;
    double p95_us = 0;
    double p99_us = 0;
};

JitterStats compute_jitter_stats(std::vector<double>& samples_us) {
    JitterStats s;
    if (samples_us.empty()) return s;
    std::sort(samples_us.begin(), samples_us.end());
    const size_t n = samples_us.size();
    s.min_us = samples_us.front();
    s.avg_us = std::accumulate(samples_us.begin(), samples_us.end(), 0.0) /
               static_cast<double>(n);
    auto idx = [n](double p) -> size_t {
        size_t i = static_cast<size_t>(p * static_cast<double>(n - 1));
        return i >= n ? n - 1 : i;
    };
    s.p50_us = samples_us[idx(0.50)];
    s.p95_us = samples_us[idx(0.95)];
    s.p99_us = samples_us[idx(0.99)];
    return s;
}

void run_delayed(const Config& cfg, bool json_only) {
    kairo::Executor ex;
    kairo::ExecutorConfig ec = make_executor_config(cfg);
    if (!ex.initialize(ec)) {
        std::cerr << "benchmark_timer_precision: initialize failed" << std::endl;
        std::exit(1);
    }

    std::mutex mtx;
    std::map<int64_t, std::vector<double>> jitters_us;

    for (int64_t D_ms : cfg.periods_ms) {
        std::vector<std::future<void>> futures;
        futures.reserve(cfg.tasks_per_period);

        for (size_t i = 0; i < cfg.tasks_per_period; ++i) {
            auto submit_time = clock::now();
            auto fut = ex.submit_delayed(D_ms, [&mtx, &jitters_us, D_ms, submit_time]() {

                auto actual = clock::now();
                auto expected = submit_time + std::chrono::milliseconds(D_ms);
                double jitter_us =
                    std::chrono::duration<double, std::micro>(actual - expected).count();
                std::lock_guard<std::mutex> lock(mtx);
                jitters_us[D_ms].push_back(jitter_us);
            }).future;
            futures.push_back(std::move(fut));
        }

        for (auto& f : futures) f.get();
    }

    (void)ex.wait_for_completion(std::chrono::seconds{300});
    ex.shutdown(true);

    if (cfg.json_output) {
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "  {\"name\":\"timer_precision_delayed\",\"config\":{"
                  << "\"periods_ms\":[";
        for (size_t i = 0; i < cfg.periods_ms.size(); ++i)
            std::cout << (i ? "," : "") << cfg.periods_ms[i];
        std::cout << "],\"tasks_per_period\":" << cfg.tasks_per_period << "},\"metrics\":{";
        bool first = true;
        for (int64_t p : cfg.periods_ms) {
            auto it = jitters_us.find(p);
            if (it == jitters_us.end()) continue;
            auto& v = it->second;
            JitterStats st = compute_jitter_stats(v);
            std::string key = std::to_string(p) + "ms";
            if (!first) std::cout << ",";
            std::cout << "\"" << key << "\":{\"jitter_us\":{"
                      << "\"min\":" << st.min_us << ",\"avg\":" << st.avg_us
                      << ",\"p50\":" << st.p50_us << ",\"p95\":" << st.p95_us
                      << ",\"p99\":" << st.p99_us << "}}";
            first = false;
        }
        std::cout << "}}";
        return;
    }
    if (json_only) return;
    std::cout << "--- Timer Precision (Delayed) ---\n";
    for (int64_t p : cfg.periods_ms) {
        auto it = jitters_us.find(p);
        if (it == jitters_us.end()) continue;
        auto& v = it->second;
        JitterStats st = compute_jitter_stats(v);
        std::cout << "  " << p << " ms: min=" << st.min_us << " avg=" << st.avg_us
                  << " p50=" << st.p50_us << " p95=" << st.p95_us << " p99=" << st.p99_us
                  << " us (n=" << v.size() << ")\n";
    }
}

void run_periodic(const Config& cfg, bool json_only) {
    kairo::Executor ex;
    kairo::ExecutorConfig ec = make_executor_config(cfg);
    if (!ex.initialize(ec)) {
        std::cerr << "benchmark_timer_precision: initialize failed" << std::endl;
        std::exit(1);
    }

    std::mutex mtx;
    std::condition_variable cv;
    std::map<int64_t, std::vector<double>> jitters_us;
    std::map<int64_t, size_t> cycles_done;
    const size_t target_cycles = cfg.cycles_per_period;

    for (int64_t P_ms : cfg.periods_ms) {
        std::vector<double> samples;
        clock::time_point start;
        size_t k = 0;
        kairo::TimerHandle periodic_handle;

        auto fn = [&]() {
            auto now = clock::now();
            std::lock_guard<std::mutex> lock(mtx);
            if (k == 0) {
                start = now;
                samples.push_back(0.0);
            } else {
                auto expected = start + std::chrono::milliseconds(P_ms * static_cast<int64_t>(k));
                double jitter_us =
                    std::chrono::duration<double, std::micro>(now - expected).count();
                samples.push_back(jitter_us);
            }
            ++k;
            if (k >= target_cycles) {
                jitters_us[P_ms] = std::move(samples);
                cycles_done[P_ms] = k;
                cv.notify_one();
            }
        };

        periodic_handle = ex.submit_periodic(P_ms, fn);

        {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait(lock, [&] { return cycles_done.count(P_ms) && cycles_done[P_ms] >= target_cycles; });
        }

        (void)periodic_handle.cancel();
    }

    (void)ex.wait_for_completion(std::chrono::seconds{300});
    ex.shutdown(true);

    if (cfg.json_output) {
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "  {\"name\":\"timer_precision_periodic\",\"config\":{"
                  << "\"periods_ms\":[";
        for (size_t i = 0; i < cfg.periods_ms.size(); ++i)
            std::cout << (i ? "," : "") << cfg.periods_ms[i];
        std::cout << "],\"cycles_per_period\":" << cfg.cycles_per_period << "},\"metrics\":{";
        bool first = true;
        for (int64_t p : cfg.periods_ms) {
            auto it = jitters_us.find(p);
            if (it == jitters_us.end()) continue;
            auto v = it->second;
            JitterStats st = compute_jitter_stats(v);
            std::string key = std::to_string(p) + "ms";
            if (!first) std::cout << ",";
            std::cout << "\"" << key << "\":{\"jitter_us\":{"
                      << "\"min\":" << st.min_us << ",\"avg\":" << st.avg_us
                      << ",\"p50\":" << st.p50_us << ",\"p95\":" << st.p95_us
                      << ",\"p99\":" << st.p99_us << "}}";
            first = false;
        }
        std::cout << "}}";
        return;
    }
    if (json_only) return;
    std::cout << "--- Timer Precision (Periodic) ---\n";
    for (int64_t p : cfg.periods_ms) {
        auto it = jitters_us.find(p);
        if (it == jitters_us.end()) continue;
        auto& v = it->second;
        JitterStats st = compute_jitter_stats(v);
        std::cout << "  " << p << " ms: min=" << st.min_us << " avg=" << st.avg_us
                  << " p50=" << st.p50_us << " p95=" << st.p95_us << " p99=" << st.p99_us
                  << " us (n=" << v.size() << ")\n";
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    Config cfg;
    apply_env(cfg);
    parse_args(argc, argv, cfg);

    const bool json_only = cfg.json_output;

    if (cfg.json_output) {
        std::cout << "{\"benchmarks\":[\n";
    } else {
        std::cout << "========================================\n";
        std::cout << "Executor Timer Precision Benchmark\n";
        std::cout << "========================================\n\n";
    }

    run_delayed(cfg, json_only);
    if (cfg.json_output) std::cout << ",\n";
    else std::cout << "\n";

    run_periodic(cfg, json_only);

    if (cfg.json_output) {
        std::cout << "\n]}\n";
    } else {
        std::cout << "\n========================================\n";
        std::cout << "Timer precision benchmark complete\n";
        std::cout << "========================================\n";
    }

    return 0;
}
