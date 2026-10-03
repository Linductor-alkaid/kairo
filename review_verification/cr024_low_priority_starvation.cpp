// CR-024: strict priority without aging -> LOW tasks starve while HIGH
// short tasks keep flowing (priority_scheduler.cpp:129-186, dequeue checks
// CRITICAL->HIGH->NORMAL->LOW in strict order, no aging).
//
// Method:
//  1. 4-worker pool (min=max=4, no resize monitor thread).
//  2. Four submitter threads flood ~2ms HIGH tasks for ~3s, with
//     backpressure (submit paused while queue_size > 15000) so the backlog
//     and the final drain stay bounded (~5-8s).
//  3. Only AFTER the backlog is deep (>8000 queued, workers saturated), the
//     main thread submits 4 LOW sentinel tasks (equal to worker count) and
//     keeps sampling every 50ms until the flood ends.
//
// CONFIRMED iff: zero sentinels start while HIGH submission is ongoing
// (saturated queue), and sentinels run only after submissions stop and the
// backlog drains.
//
// Known nuance (observed in a pre-run): during the last ~100ms of the drain,
// HIGH tasks sitting in worker-local queues are invisible to the global
// scheduler's strict order, so a LOW may start marginally before queue_size
// reaches 0. The claim under test is starvation while HIGH submission is
// ongoing, which the t_flood_end criterion captures.
#include "kairo/thread_pool/thread_pool.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

using namespace kairo;
namespace chrono = std::chrono;

static int64_t now_ns() {
    return chrono::duration_cast<chrono::nanoseconds>(
               chrono::steady_clock::now().time_since_epoch())
        .count();
}

int main() {
    setbuf(stdout, nullptr);

    ThreadPool pool;
    ThreadPoolConfig config;
    config.min_threads = 4;
    config.max_threads = 4;
    config.queue_capacity = 10000;
    if (!pool.initialize(config)) {
        printf("FATAL: initialize failed\n");
        return 2;
    }

    std::atomic<int> sentinel_started{0};
    std::atomic<int> sentinel_done{0};
    std::atomic<int64_t> first_sentinel_start_ns{0};
    std::atomic<int64_t> sentinel_start_ns[4] = {{0}, {0}, {0}, {0}};
    std::atomic<bool> flooding{true};
    std::atomic<long> high_submitted{0};
    std::atomic<int> high_completed{0};

    const int64_t t_flood_start = now_ns();

    // ---- Step 1: HIGH flood with backpressure ----
    std::vector<std::thread> submitters;
    for (int w = 0; w < 4; ++w) {
        submitters.emplace_back([&]() {
            while (flooding.load(std::memory_order_relaxed)) {
                if (pool.get_status().queue_size > 15000) {
                    std::this_thread::sleep_for(chrono::milliseconds(2));
                    continue;
                }
                if (pool.try_submit_priority(2, [&high_completed]() {
                        std::this_thread::sleep_for(chrono::milliseconds(2));
                        high_completed.fetch_add(1, std::memory_order_relaxed);
                    })) {
                    high_submitted.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    // ---- Step 2: once backlog is deep, inject LOW sentinels ----
    size_t depth_at_inject = 0;
    for (int i = 0; i < 3000; ++i) {
        depth_at_inject = pool.get_status().queue_size;
        if (depth_at_inject > 8000) break;
        std::this_thread::sleep_for(chrono::milliseconds(2));
    }
    const int64_t t_sentinel_submit = now_ns();
    bool all_accepted = true;
    for (int k = 0; k < 4; ++k) {
        if (!pool.try_submit_priority(0, [&]() {
                int64_t t = now_ns();
                int64_t expected = 0;
                first_sentinel_start_ns.compare_exchange_strong(expected, t);
                sentinel_start_ns[sentinel_started.fetch_add(1)].store(t);
                std::this_thread::sleep_for(chrono::milliseconds(1));
                sentinel_done.fetch_add(1);
            })) {
            all_accepted = false;
        }
    }
    printf("4 LOW sentinels submitted at t=%lldms (queue depth=%zu, "
           "accepted=%s)\n",
           static_cast<long long>((t_sentinel_submit - t_flood_start) / 1e6),
           depth_at_inject, all_accepted ? "true" : "false");

    // ---- Step 3: sampling until the flood window ends (3s total) ----
    int printed = 0;
    int started_during_flood_samples = 0;
    while ((now_ns() - t_flood_start) < 3000000000LL) {  // 3s flood window
        std::this_thread::sleep_for(chrono::milliseconds(50));
        int started = sentinel_started.load();
        ThreadPoolStatus st = pool.get_status();
        if (started > 0) ++started_during_flood_samples;
        if (printed < 10) {
            printf("flood: t=%lldms submitted=%ld high_done=%d queue=%zu "
                   "sentinel_started=%d\n",
                   static_cast<long long>((now_ns() - t_flood_start) / 1e6),
                   high_submitted.load(), high_completed.load(), st.queue_size,
                   started);
            ++printed;
        }
    }
    flooding.store(false);
    for (auto& t : submitters) t.join();
    const int64_t t_flood_end = now_ns();
    const long total_high = high_submitted.load();
    printf("flood ended: t=%lldms, total HIGH submitted=%ld\n",
           static_cast<long long>((t_flood_end - t_flood_start) / 1e6),
           total_high);

    // ---- Step 4: wait for drain + sentinels (bounded 60s) ----
    int64_t t_drained = -1;
    for (int i = 0; i < 1200; ++i) {
        std::this_thread::sleep_for(chrono::milliseconds(50));
        ThreadPoolStatus st = pool.get_status();
        if (t_drained < 0 && st.queue_size == 0 &&
            high_completed.load() >= total_high) {
            t_drained = now_ns();
        }
        if (sentinel_done.load() >= 4 && t_drained > 0) break;
    }
    for (int i = 0; i < 100 && sentinel_done.load() < 4; ++i) {
        std::this_thread::sleep_for(chrono::milliseconds(50));
    }

    int64_t first_start = first_sentinel_start_ns.load();
    double first_start_s =
        first_start > 0 ? (first_start - t_flood_start) / 1e9 : -1.0;
    double flood_end_s = (t_flood_end - t_flood_start) / 1e9;
    printf("first sentinel start: %.3fs after flood start (flood ran for "
           "%.3fs); sentinel_started_during_flood_samples=%d\n",
           first_start_s, flood_end_s, started_during_flood_samples);
    for (int k = 0; k < 4; ++k) {
        int64_t st_ns = sentinel_start_ns[k].load();
        printf("sentinel %d: started %s flood end (%.3fs rel. flood start, "
               "done=%d)\n",
               k, (st_ns >= t_flood_end) ? "after" : "BEFORE",
               st_ns > 0 ? (st_ns - t_flood_start) / 1e9 : -1.0,
               sentinel_done.load());
    }

    bool confirmed =
        all_accepted && (started_during_flood_samples == 0) &&
        (first_start >= t_flood_end);
    printf("CR-024 verdict: %s (LOW starved during entire sustained HIGH "
           "submission window; ran only after submissions stopped)\n",
           confirmed ? "CONFIRMED" : "NOT REPRODUCED");
    pool.shutdown();
    return confirmed ? 0 : 1;
}
