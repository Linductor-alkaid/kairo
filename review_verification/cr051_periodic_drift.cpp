// CR-051 verification: facade periodic timer has no drift compensation
// (fixed-delay behavior) - include/executor/timer.hpp timer_thread_loop
// recomputes record.next_execute_time = now + interval_ms from the actual
// wake/dispatch time instead of the original planned deadline.
//
// Claimed symptom: period=100ms with a 30ms callback => mean interval ~130ms
// and cumulative drift (>1s late by tick 50).
//
// Path under test: kairo::Executor::submit_periodic() =>
// detail::TimerScheduler (timer.hpp). Ticks are dispatched asynchronously to
// the default thread pool; the next deadline is computed at tick-build time
// from the loop's `now`. Measurements below report what actually happens.
//
// Run: cr051_periodic_drift [sleep_us]   (default 30000us = 30ms)
#include <kairo/executor.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

namespace {

struct RunStats {
    std::vector<double> intervals_ms;
    double mean_ms = 0;
    double max_ms = 0;
    double min_ms = 0;
    double drift_ms = 0;  // t_last - (t_first + (n-1)*period)
    int ticks = 0;
};

RunStats measure(const char* label, int period_ms, int callback_sleep_ms, int n_ticks) {
    kairo::Executor executor;
    kairo::ExecutorConfig config;
    config.min_threads = 2;
    config.max_threads = 4;
    if (!executor.initialize(config)) {
        std::printf("%s: initialize FAILED\n", label);
        return {};
    }

    std::vector<Clock::time_point> trigger_times;
    trigger_times.reserve(n_ticks + 2);
    std::atomic<bool> done{false};

    auto handle = executor.submit_periodic(
        period_ms,
        [&, period_ms]() {
            trigger_times.push_back(Clock::now());
            if (callback_sleep_ms > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(callback_sleep_ms));
            }
            if (static_cast<int>(trigger_times.size()) >= n_ticks) {
                done.store(true);
            }
        });

    const auto deadline = Clock::now() + std::chrono::seconds(30);
    while (!done.load() && Clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    handle.cancel();
    executor.shutdown(true);

    RunStats stats;
    stats.ticks = static_cast<int>(trigger_times.size());
    if (stats.ticks < 3) {
        std::printf("%s: only %d ticks, inconclusive\n", label, stats.ticks);
        return stats;
    }
    const double planned = period_ms;
    for (size_t i = 1; i < trigger_times.size(); ++i) {
        const double dt = std::chrono::duration<double, std::milli>(
                              trigger_times[i] - trigger_times[i - 1])
                              .count();
        stats.intervals_ms.push_back(dt);
        stats.mean_ms += dt;
        stats.max_ms = std::max(stats.max_ms, dt);
        if (stats.min_ms == 0) stats.min_ms = dt;
        stats.min_ms = std::min(stats.min_ms, dt);
    }
    stats.mean_ms /= static_cast<double>(stats.intervals_ms.size());
    const double span = std::chrono::duration<double, std::milli>(
                            trigger_times.back() - trigger_times.front())
                            .count();
    stats.drift_ms = span - planned * (stats.ticks - 1);

    std::printf("%s: ticks=%d mean_interval=%.2fms min=%.2fms max=%.2fms "
                "(period=%dms, callback=%dms)\n",
                label, stats.ticks, stats.mean_ms, stats.min_ms, stats.max_ms,
                period_ms, callback_sleep_ms);
    std::printf("%s: total drift over %d ticks = %.2fms "
                "(CR claim: mean ~%dms and drift >1000ms)\n",
                label, stats.ticks, stats.drift_ms, period_ms + callback_sleep_ms);
    // print first 10 intervals for shape
    std::printf("%s: intervals[0..9]:", label);
    for (size_t i = 0; i < stats.intervals_ms.size() && i < 10; ++i) {
        std::printf(" %.1f", stats.intervals_ms[i]);
    }
    std::printf("\n");
    return stats;
}

}  // namespace

int main(int argc, char** argv) {
    const int sleep_ms = argc > 1 ? std::atoi(argv[1]) : 30;
    const int ticks = argc > 2 ? std::atoi(argv[2]) : 50;

    std::printf("=== CR-051 control: period=100ms, NO callback sleep ===\n");
    const RunStats control = measure("control", 100, 0, ticks);

    std::printf("\n=== CR-051: period=100ms, callback sleeps %dms ===\n", sleep_ms);
    const RunStats loaded = measure("loaded", 100, sleep_ms, ticks);

    if (control.ticks < 3 || loaded.ticks < 3) {
        std::printf("INCONCLUSIVE\n");
        return 2;
    }
    std::printf("\nverdict: %s\n",
                (loaded.mean_ms > 100.0 + sleep_ms * 0.7 && loaded.drift_ms > 1000.0)
                    ? "CONFIRMED - fixed-delay drift"
                    : "NOT REPRODUCED - interval stays ~100ms, no cumulative drift");
    return 0;
}
