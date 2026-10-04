// CR-102: terminal-retention trim runs a linear scan + vector erase(begin)
// under task_graph_mutex_ on every tracked completion once the terminal order
// exceeds task_graph_retention_capacity_ (default 1024).
// Method: sequential submit_with_handle round-trips, 100k total, comparing
// default capacity (1024) vs small capacity (8) vs 0. The per-op delta
// isolates the O(capacity) trim cost. Also report per-10k-block latency trend
// inside each run to show whether cost is bounded or growing.
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

void run_case(const char* label, size_t retention_capacity, size_t total) {
    Executor executor;
    ExecutorConfig ec;
    ec.min_threads = 2;
    ec.max_threads = 2;
    if (!executor.initialize(ec)) {
        std::printf("%s: initialize FAILED\n", label);
        return;
    }
    executor.set_task_graph_retention_capacity(retention_capacity);

    std::vector<double> block_us(static_cast<size_t>(total / 10000), 0.0);
    const double t0 = now_s();
    for (size_t i = 0; i < total; ++i) {
        const double a = now_s();
        auto submission = executor.submit_with_handle([]() noexcept {});
        submission.future.get();
        block_us[i / 10000] += (now_s() - a);
    }
    const double elapsed = now_s() - t0;
    std::printf("%-24s capacity=%4zu  total=%zu  elapsed=%.3fs  ops/s=%.0f  avg_us=%.2f\n",
                label, retention_capacity, total, elapsed,
                static_cast<double>(total) / elapsed, elapsed / total * 1e6);
    // Per-10k-block average latency (us/op) to show the trend.
    std::printf("  blocks(us/op):");
    for (size_t b = 0; b < block_us.size(); b += 2) {
        std::printf(" %.2f", block_us[b] / 10000.0 * 1e6);
    }
    std::printf("\n");
    executor.shutdown();
}

} // namespace

int main() {
    const size_t total = 100000;
    // Warmup (page in allocator etc.) on a tiny separate instance.
    run_case("warmup", 8, 20000);
    run_case("A_default", 1024, total);
    run_case("B_small", 8, total);
    run_case("C_zero", 0, total);
    return 0;
}
