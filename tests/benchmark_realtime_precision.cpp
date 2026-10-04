/**
 * Real-time thread cycle precision benchmark.
 *
 * Measures jitter (actual - expected) in microseconds for RealtimeThreadExecutor
 * cycle_callback at multiple periods. Periods: 1, 5, 10, 50, 100 ms.
 * Output: human-readable or JSON (--json).
 */

#include <kairo/executor.hpp>
#include <kairo/config.hpp>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <numeric>
#include <string>
#include <vector>
#include <thread>
#ifdef __linux__
#include <sched.h>
#endif

namespace {

using clock = std::chrono::steady_clock;

const std::vector<int64_t> kDefaultPeriodsMs = {1, 5, 10, 50, 100};
constexpr size_t kDefaultCyclesPerPeriod = 20;

struct Config {
    std::vector<int64_t> periods_ms = kDefaultPeriodsMs;
    size_t cycles_per_period = kDefaultCyclesPerPeriod;
    bool json_output = false;
};

struct EnvironmentInfo {
    std::string compiler;
    std::string scheduler;
    int cpu = -1;
};

EnvironmentInfo environment_info() {
    EnvironmentInfo info;
#if defined(__clang__)
    info.compiler = "clang-" __clang_version__;
#elif defined(__GNUC__)
    info.compiler = "gcc-" __VERSION__;
#elif defined(_MSC_VER)
    info.compiler = "msvc-" + std::to_string(_MSC_VER);
#else
    info.compiler = "unknown";
#endif
#ifdef __linux__
    // Android 也定义 __linux__；bionic 自 API 21 提供 sched_getcpu /
    // sched_getscheduler，但调度策略可能返回 SCHED_NORMAL 且 affinity 受限。
    info.cpu = sched_getcpu();
    switch (sched_getscheduler(0)) {
    case SCHED_FIFO: info.scheduler = "SCHED_FIFO"; break;
    case SCHED_RR: info.scheduler = "SCHED_RR"; break;
    case SCHED_OTHER: info.scheduler = "SCHED_OTHER"; break;
    default: info.scheduler = "unknown"; break;
    }
#else
    info.scheduler = "platform-default";
#endif
    return info;
}

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
        if (a == "--cycles-per-period" && i + 1 < argc) {
            c.cycles_per_period = parse_size_t(argv[++i], c.cycles_per_period);
            continue;
        }
    }
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

void run_realtime_precision(const Config& cfg, bool json_only) {
    const EnvironmentInfo env = environment_info();
    kairo::Executor ex;
    kairo::ExecutorConfig ec;
    ec.min_threads = 2;
    ec.max_threads = 4;
    if (!ex.initialize(ec)) {
        std::cerr << "benchmark_realtime_precision: initialize failed" << std::endl;
        std::exit(1);
    }

    std::mutex mtx;
    std::condition_variable cv;
    std::map<int64_t, std::vector<double>> jitters_us;
    std::map<int64_t, size_t> cycles_done;
    const size_t target_cycles = cfg.cycles_per_period;

    for (int64_t P_ms : cfg.periods_ms) {
        const int64_t period_ns = P_ms * 1000000;
        std::vector<double> samples;
        clock::time_point start;
        size_t k = 0;
        std::string task_name = "rt_" + std::to_string(P_ms) + "ms";

        kairo::RealtimeThreadConfig rt_config;
        rt_config.thread_name = task_name;
        rt_config.cycle_period_ns = period_ns;
        rt_config.cycle_callback = [&]() {
            auto now = clock::now();
            std::lock_guard<std::mutex> lock(mtx);
            if (k == 0) {
                start = now;
                samples.push_back(0.0);
            } else {
                auto expected = start + std::chrono::nanoseconds(period_ns * static_cast<int64_t>(k));
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

        if (!ex.register_realtime_task(task_name, rt_config)) {
            std::cerr << "benchmark_realtime_precision: register_realtime_task failed"
                      << std::endl;
            std::exit(1);
        }
        if (!ex.start_realtime_task(task_name)) {
            std::cerr << "benchmark_realtime_precision: start_realtime_task failed"
                      << std::endl;
            std::exit(1);
        }

        {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait(lock, [&] {
                return cycles_done.count(P_ms) && cycles_done[P_ms] >= target_cycles;
            });
        }

        ex.stop_realtime_task(task_name);
    }

    ex.shutdown(false);

    if (cfg.json_output) {
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "  {\"name\":\"realtime_precision\",\"measurement\":{"
                  << "\"jitter_definition\":\"cycle_callback_entry_minus_expected\","
                  << "\"first_sample_is_baseline\":true,\"startup_wait_excluded\":true},"
                  << "\"environment\":{";
        std::cout << "\"compiler\":\"" << env.compiler << "\",\"scheduler\":\""
                  << env.scheduler << "\",\"cpu\":" << env.cpu << "},\"config\":{";
        std::cout << "\"periods_ms\":[";
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
    std::cout << "--- Real-time Thread Cycle Precision ---\n";
    std::cout << "  compiler=" << env.compiler << ", scheduler=" << env.scheduler
              << ", sample_cpu=" << env.cpu << "\n";
    std::cout << "  measurement: callback entry minus expected deadline; first sample is baseline;"
                 " startup wait excluded\n";
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
        std::cout << "Real-time Thread Precision Benchmark\n";
        std::cout << "========================================\n\n";
    }

    run_realtime_precision(cfg, json_only);

    if (cfg.json_output) {
        std::cout << "\n]}\n";
    } else {
        std::cout << "\n========================================\n";
        std::cout << "Real-time precision benchmark complete\n";
        std::cout << "========================================\n";
    }

    return 0;
}
