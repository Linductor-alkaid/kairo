// CR-106: submit_auto routes through route_task -> manager get_executor_capabilities()
// on every submission: locks every backend and builds a capability vector each time.
// Method: single-thread round-trips (submit + future.get) 100k times for
//   (1) plain submit()             -> baseline
//   (2) submit_auto()              -> baseline + routing
//   (3) get_executor_capabilities() alone -> the per-call collection cost.
#include <kairo/executor.hpp>

#include <chrono>
#include <cstdio>

using namespace kairo;

namespace {

double now_s() {
    return std::chrono::duration<double>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

int main() {
    const size_t total = 100000;
    Executor executor;
    ExecutorConfig ec;
    ec.min_threads = 2;
    ec.max_threads = 2;
    if (!executor.initialize(ec)) {
        std::printf("initialize FAILED\n");
        return 1;
    }

    // Warmup.
    for (size_t i = 0; i < 10000; ++i) {
        executor.submit([] {}).get();
    }

    {
        const double t0 = now_s();
        for (size_t i = 0; i < total; ++i) {
            executor.submit([] {}).get();
        }
        const double e = now_s() - t0;
        std::printf("submit()        : %.3fs  ops/s=%.0f  avg_us=%.2f\n",
                    e, total / e, e / total * 1e6);
    }
    {
        const double t0 = now_s();
        for (size_t i = 0; i < total; ++i) {
            executor.submit_auto([] {}).get();
        }
        const double e = now_s() - t0;
        std::printf("submit_auto()   : %.3fs  ops/s=%.0f  avg_us=%.2f\n",
                    e, total / e, e / total * 1e6);
    }
    {
        const size_t cap_calls = 100000;
        const double t0 = now_s();
        size_t sink = 0;
        for (size_t i = 0; i < cap_calls; ++i) {
            sink += executor.get_executor_capabilities().size();
        }
        const double e = now_s() - t0;
        std::printf("get_executor_capabilities(): %.3fs  calls/s=%.0f  avg_us=%.2f  (sink=%zu)\n",
                    e, cap_calls / e, e / cap_calls * 1e6, sink);
    }
    executor.shutdown();
    return 0;
}
