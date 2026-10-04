// CR-102 rep variant: interleaved repeated measurement of tracked round-trip
// throughput at retention capacities {1024, 8, 0}; reports every rep plus the
// best (min-latency) rep per capacity to reduce scheduler noise.
#include <kairo/executor.hpp>

#include <chrono>
#include <cstdio>
#include <vector>

using namespace kairo;

namespace {

double now_s() {
    return std::chrono::duration<double>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

double run_once(size_t capacity, size_t total) {
    Executor executor;
    ExecutorConfig ec;
    ec.min_threads = 2;
    ec.max_threads = 2;
    if (!executor.initialize(ec)) {
        std::printf("initialize FAILED\n");
        return -1.0;
    }
    executor.set_task_graph_retention_capacity(capacity);
    const double t0 = now_s();
    for (size_t i = 0; i < total; ++i) {
        executor.submit_with_handle([]() noexcept {}).future.get();
    }
    const double e = now_s() - t0;
    executor.shutdown();
    return e;
}

} // namespace

int main() {
    const size_t total = 50000;
    const size_t caps[] = {1024, 8, 0, 1024, 8, 0, 1024, 8, 0};
    // Warmup.
    run_once(8, 10000);
    std::vector<double> a, b, c;
    for (size_t r = 0; r < sizeof(caps) / sizeof(caps[0]); ++r) {
        const double e = run_once(caps[r], total);
        const double us = e / total * 1e6;
        std::printf("cap=%4zu rep=%zu  ops/s=%.0f  avg_us=%.2f\n",
                    caps[r], r, total / e, us);
        if (caps[r] == 1024) a.push_back(us);
        else if (caps[r] == 8) b.push_back(us);
        else c.push_back(us);
    }
    auto best = [](std::vector<double>& v) {
        double m = v[0];
        for (double x : v) m = std::min(m, x);
        return m;
    };
    std::printf("BEST avg_us: cap1024=%.2f cap8=%.2f cap0=%.2f\n",
                best(a), best(b), best(c));
    return 0;
}
