// CR-005: park_worker lost wakeup (lockfree_task_executor.cpp:526-544).
//
// Runtime evidence attempt (single worker, x86):
//   Phase A (as specified): producer pushes 500k tasks at random 20-200us
//     intervals; each task records its execution timestamp; latency
//     percentiles and outlier clusters are reported.
//   Phase B (park-driving): 20k tasks at 400-1200us intervals, which exceeds
//     the ~550us idle ramp (32 PAUSE + 32 yield + 50x10us sleep) and forces
//     the worker into futex park before most tasks. A lost wakeup would show
//     up as a cluster of latencies ~= one inter-arrival gap (hundreds of us).
//
// A lost wakeup makes a task wait until the NEXT producer push bumps
// wake_seq_ and observes the parked bit, so outliers should cluster near the
// inter-arrival distribution and far above the median.

#include "kairo/lockfree_task_executor.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <random>
#include <thread>
#include <vector>
#include <sys/prctl.h>

using kairo::LockFreeTaskExecutor;

namespace {

long long now_ns() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

void run_phase(const char* name, size_t n, int gap_min_us, int gap_max_us,
               std::vector<long long>& enqueue_ns, std::vector<long long>& exec_ns) {
    LockFreeTaskExecutor ex(65536);
    if (!ex.start()) {
        std::printf("[%s] FATAL: start failed\n", name);
        return;
    }
    enqueue_ns.assign(n, 0);
    exec_ns.assign(n, 0);

    std::mt19937 rng(12345);
    std::uniform_int_distribution<int> gap_us(gap_min_us, gap_max_us);

    const auto t0 = now_ns();
    for (size_t i = 0; i < n; ++i) {
        std::this_thread::sleep_for(std::chrono::microseconds(gap_us(rng)));
        enqueue_ns[i] = now_ns();
        while (!ex.push_task([i, &exec_ns] { exec_ns[i] = now_ns(); })) {
            // queue full (not expected at this rate): spin-retry same slot
            std::this_thread::yield();
        }
    }
    const auto pushed_done = now_ns();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (ex.processed_count() < n &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto t1 = now_ns();

    std::vector<long long> lat(n);
    for (size_t i = 0; i < n; ++i) lat[i] = exec_ns[i] - enqueue_ns[i];
    std::vector<long long> sorted = lat;
    std::sort(sorted.begin(), sorted.end());
    auto pct = [&](double p) {
        size_t idx = static_cast<size_t>(p * static_cast<double>(n - 1));
        return sorted[idx];
    };
    const long long p50 = pct(0.50);
    const long long p999 = pct(0.999);
    const long long p9999 = pct(0.9999);
    const long long mx = sorted.back();
    const long long thresh = p50 * 100 + 1;
    size_t outliers = 0;
    for (long long v : lat) {
        if (v > thresh) ++outliers;
    }

    std::printf("[%s] n=%zu wall=%.1fs (push loop %.1fs)\n", name, n,
                (t1 - t0) / 1e9, (pushed_done - t0) / 1e9);
    std::printf("[%s] latency us: p50=%.1f p90=%.1f p99=%.1f p99.9=%.1f "
                "p99.99=%.1f max=%.1f  outliers(>100x p50, %lldus)=%zu\n",
                name, p50 / 1e3, pct(0.90) / 1e3, pct(0.99) / 1e3,
                p999 / 1e3, p9999 / 1e3, mx / 1e3, thresh / 1000, outliers);

    // top-10 outliers with their distance to the next enqueue (lost-wakeup
    // signature: latency ~= gap to next enqueue)
    std::vector<size_t> order(n);
    for (size_t i = 0; i < n; ++i) order[i] = i;
    std::partial_sort(order.begin(), order.begin() + std::min<size_t>(10, n),
                      order.end(), [&](size_t a, size_t b) { return lat[a] > lat[b]; });
    std::printf("[%s] top outliers (idx: latency_us | next_gap_us | class):\n", name);
    // Lost-wakeup signature: a parked worker that misses the notify stays
    // asleep until the NEXT producer push (fetch_add + notify_one), so the
    // outlier's latency is bounded by its inter-arrival gap. Latencies far
    // EXCEEDING the next gap cannot be a lost wakeup (=> scheduler noise).
    size_t signature = 0, unexplained = 0;
    for (size_t i = 0; i + 1 < n; ++i) {
        if (lat[i] <= thresh) continue;
        const long long gap =
            (enqueue_ns[i + 1] > 0) ? enqueue_ns[i + 1] - enqueue_ns[i] : 0;
        if (gap > 0 && lat[i] <= gap + 100000LL) {
            ++signature;
        } else {
            ++unexplained;
        }
    }
    std::printf("[%s] outlier classes: lost-wakeup-signature=%zu "
                "unexplained(scheduler)=%zu\n", name, signature, unexplained);
    for (size_t k = 0; k < std::min<size_t>(10, n); ++k) {
        const size_t i = order[k];
        const long long next_gap =
            (i + 1 < n && enqueue_ns[i + 1] > 0) ? enqueue_ns[i + 1] - enqueue_ns[i] : -1;
        const bool sig = next_gap > 0 && lat[i] <= next_gap + 100000LL;
        std::printf("  #%zu: %.1f | %.1f | %s\n", i, lat[i] / 1e3, next_gap / 1e3,
                    sig ? "SIGNATURE" : "scheduler-noise");
    }

    ex.stop_and_join();
}

}  // namespace

int main() {
    // tighten sleep granularity so the 20-200us producer gaps are meaningful
    prctl(PR_SET_TIMERSLACK, 1UL, 0, 0, 0);

    std::vector<long long> enq, exe;
    run_phase("A 20-200us", 500000, 20, 200, enq, exe);
    std::vector<long long> enq2, exe2;
    run_phase("B 400-1200us(park)", 20000, 400, 1200, enq2, exe2);
    std::printf("CR-005 runtime probe finished\n");
    return 0;
}
