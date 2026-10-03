// CR-054 verification: RealtimeThreadExecutor's built-in cycle loop
// (simple_cycle_loop, realtime_thread_executor.cpp:441-491) sleeps with
// std::this_thread::sleep_until(next_cycle_time) and only checks running_
// at the top of the loop. stop_and_join() just flips running_ and joins,
// so stop latency can approach a full cycle period: the worker finishes its
// sleep_until before observing the stop request.
//
// Test: cycle period 500ms, trivial callback, no ICycleManager (built-in
// loop). Warm up, then measure stop_and_join() duration across rounds.
// Expected if confirmed: latencies roughly uniform in [0, 500ms), mean ~250ms.
#include <kairo/realtime_thread_executor.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

using kairo::RealtimeThreadConfig;
using kairo::RealtimeThreadExecutor;

using Clock = std::chrono::steady_clock;

int main(int argc, char** argv) {
    const int period_ms = argc > 1 ? std::atoi(argv[1]) : 500;
    const int rounds = argc > 2 ? std::atoi(argv[2]) : 8;

    std::printf("=== CR-054: built-in loop stop latency, period=%dms, %d rounds ===\n",
                period_ms, rounds);
    std::vector<double> latencies;
    for (int r = 0; r < rounds; ++r) {
        RealtimeThreadConfig config;
        config.thread_name = "cr054_rt";
        config.cycle_period_ns = period_ms * 1'000'000LL;
        config.max_tasks_per_cycle = 0;
        config.cycle_callback = [] {};  // trivial: no user callback delay

        RealtimeThreadExecutor executor("cr054", config, /*enable_stats=*/false, 1024);
        if (!executor.start()) {
            std::printf("round %d: start FAILED\n", r);
            return 2;
        }
        // warm-up: let at least two cycles complete
        std::this_thread::sleep_for(std::chrono::milliseconds(period_ms * 2 + 100));
        const auto cycle_count = executor.get_status().cycle_count;

        const auto t0 = Clock::now();
        executor.stop_and_join();
        const double stop_ms =
            std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        latencies.push_back(stop_ms);
        std::printf("round %d: warmup_cycles=%lld stop_and_join=%.1fms\n",
                    r, static_cast<long long>(cycle_count), stop_ms);
    }

    double mean = 0;
    for (double v : latencies) mean += v;
    mean /= static_cast<double>(latencies.size());
    const double max_v = *std::max_element(latencies.begin(), latencies.end());
    std::printf("summary: mean=%.1fms max=%.1fms (period=%dms)\n", mean, max_v,
                period_ms);
    std::printf("verdict: %s\n",
                (mean > period_ms * 0.25 || max_v > period_ms * 0.6)
                    ? "CONFIRMED - stop waits up to a full cycle"
                    : "NOT REPRODUCED - stop returns promptly");
    return (mean > period_ms * 0.25 || max_v > period_ms * 0.6) ? 1 : 0;
}
