// CR-117 scenario A: queue_capacity=0 -> resizer expands on ANY backlog but
// can NEVER shrink.
//
// Resizer threshold math (thread_pool_resizer.cpp:67-97) with capacity == 0:
//   should_expand: queue_high = queue_size > 0 - 0  => any queue_size > 0
//   should_shrink: queue_low  = queue_size < 0 / 5  => queue_size < 0, never
//                  true => shrink condition unsatisfiable forever.
//
// Method: idle pool (min=2, max=4, queue_capacity=0) + a PRIVATE
// ThreadPoolResizer instance driven directly via update_status()/
// check_and_resize() (bypasses the 1s monitor tick; the 60s idle timer is
// inherent to should_shrink and is waited out for real).
//
// CONFIRMED iff: expansion fires (2->3->4 workers) once queue_size>0 and
// avg_wait>100ms are reported, and during 75s of reported-total-idle
// (queue_size=0, active=0) no shrink ever happens (threads stay 4).
#include "kairo/thread_pool/thread_pool.hpp"
#include "kairo/thread_pool/thread_pool_resizer.hpp"

#include <chrono>
#include <cstdio>
#include <thread>

using namespace kairo;
namespace chrono = std::chrono;

int main() {
    setbuf(stdout, nullptr);

    ThreadPoolConfig config;
    config.min_threads = 2;
    config.max_threads = 4;
    config.queue_capacity = 0;  // the CR-117 configuration under test

    ThreadPool pool;
    if (!pool.initialize(config)) {
        printf("FATAL: initialize failed\n");
        return 2;
    }
    ThreadPoolResizer resizer(pool, config);

    printf("initial total_threads=%zu (min=%zu max=%zu queue_capacity=%zu)\n",
           pool.get_status().total_threads, config.min_threads,
           config.max_threads, config.queue_capacity);

    // ---- Expansion phase: report backlog + high wait time ----
    bool expanded = false;
    for (int step = 0; step < 3 && !expanded; ++step) {
        resizer.update_status(/*queue_size=*/3, /*active=*/2,
                              /*total=*/pool.get_status().total_threads,
                              /*avg_wait_time_ms=*/5000.0);
        std::this_thread::sleep_for(chrono::milliseconds(1200));  // pass 1s gate
        resizer.check_and_resize();
        size_t n = pool.get_status().total_threads;
        printf("expand step %d: total_threads=%zu\n", step, n);
        expanded = (n > config.min_threads);
    }
    size_t peak = pool.get_status().total_threads;
    printf("expansion %s, peak total_threads=%zu\n",
           expanded ? "OBSERVED" : "NOT OBSERVED", peak);

    // ---- Shrink-watch phase: report fully idle for 75s ----
    bool shrunk = false;
    size_t shrunk_to = 0;
    int64_t shrink_at_ms = -1;
    const auto t0 = chrono::steady_clock::now();
    while (chrono::steady_clock::now() - t0 < chrono::seconds(75)) {
        resizer.update_status(/*queue_size=*/0, /*active=*/0,
                              /*total=*/pool.get_status().total_threads,
                              /*avg_wait_time_ms=*/0.0);
        resizer.check_and_resize();
        size_t n = pool.get_status().total_threads;
        if (n < peak) {
            shrunk = true;
            shrunk_to = n;
            shrink_at_ms =
                chrono::duration_cast<chrono::milliseconds>(
                    chrono::steady_clock::now() - t0).count();
            break;
        }
        std::this_thread::sleep_for(chrono::milliseconds(200));
    }
    int64_t watched_ms = chrono::duration_cast<chrono::milliseconds>(
                             chrono::steady_clock::now() - t0).count();
    printf("idle watch: %.1fs, shrink observed=%s", watched_ms / 1e3,
           shrunk ? "yes" : "no");
    if (shrunk) printf(" (to %zu at %lldms)", shrunk_to,
                       static_cast<long long>(shrink_at_ms));
    printf(", final total_threads=%zu\n", pool.get_status().total_threads);

    bool confirmed = expanded && !shrunk;
    printf("CR-117 (capacity=0) verdict: %s\n",
           confirmed
               ? "CONFIRMED (expands on any backlog; no shrink after 75s idle "
                 "though threshold is 60s)"
               : "NOT REPRODUCED");
    return confirmed ? 0 : 1;
}
