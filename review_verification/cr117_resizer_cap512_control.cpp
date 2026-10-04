// CR-117 scenario B (control): normal queue_capacity=512 must BOTH expand and
// later shrink, proving the capacity=0 never-shrink result is specific to the
// zero-capacity threshold math and not a generally broken resizer.
//
// Same driving method as scenario A: private ThreadPoolResizer fed synthetic
// status; shrink requires >60s continuous (idle + queue-low) by design, so
// the idle watch runs up to 75s.
//
// queue_high with capacity=512: queue_size > 512 - ceil(512/5)=103, i.e.
// queue_size > 409 => feed queue_size=450 to trigger expansion.
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
    config.queue_capacity = 512;  // control: normal capacity

    ThreadPool pool;
    if (!pool.initialize(config)) {
        printf("FATAL: initialize failed\n");
        return 2;
    }
    ThreadPoolResizer resizer(pool, config);

    printf("initial total_threads=%zu (min=%zu max=%zu queue_capacity=%zu)\n",
           pool.get_status().total_threads, config.min_threads,
           config.max_threads, config.queue_capacity);

    bool expanded = false;
    for (int step = 0; step < 3 && !expanded; ++step) {
        resizer.update_status(/*queue_size=*/450, /*active=*/2,
                              /*total=*/pool.get_status().total_threads,
                              /*avg_wait_time_ms=*/5000.0);
        std::this_thread::sleep_for(chrono::milliseconds(1200));
        resizer.check_and_resize();
        size_t n = pool.get_status().total_threads;
        printf("expand step %d: total_threads=%zu\n", step, n);
        expanded = (n > config.min_threads);
    }
    size_t peak = pool.get_status().total_threads;
    printf("expansion %s, peak total_threads=%zu\n",
           expanded ? "OBSERVED" : "NOT OBSERVED", peak);

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
    if (shrunk) printf(" (to %zu at %lldms, expected ~60s idle gate)",
                       shrunk_to, static_cast<long long>(shrink_at_ms));
    printf(", final total_threads=%zu\n", pool.get_status().total_threads);

    bool control_ok = expanded && shrunk;
    printf("CR-117 control (capacity=512) verdict: %s\n",
           control_ok ? "CONTROL OK (expand + shrink both work)"
                      : "CONTROL FAILED");
    return control_ok ? 0 : 1;
}
