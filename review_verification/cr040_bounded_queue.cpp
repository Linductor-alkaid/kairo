// CR-040 verification: BoundedQueue KeepLatest/DropOldest drops the NEWEST
// item under producer/consumer contention.
//
// Scenario A (DropOldest + busy consumer):
//   capacity=8, one producer sending sequential ids with no delay, one
//   consumer doing try_pop + tiny sleep. With DropOldest semantics a failed
//   try_send should never happen (the oldest item is displaced instead).
//   Every failed send = a NEWEST item lost => a visible gap in the sequence
//   the consumer receives.
// Scenario B (KeepLatest + slow consumer):
//   capacity=8, producer sends ids 1..1000 slowly, consumer pops every ~1ms.
//   KeepLatest must preserve the latest ids: the max received id must equal
//   the last id, and the tail window [N-7, N] must all be received.
#include <kairo/comm/bounded_queue.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <thread>
#include <vector>

using kairo::comm::BoundedQueue;
using kairo::comm::CommEvent;
using kairo::comm::DropPolicy;

using Clock = std::chrono::steady_clock;

namespace {

struct ScenarioAResult {
    uint64_t sent_ok = 0;
    uint64_t send_failures = 0;
    uint64_t fail_depth_ge_capacity = 0;  // depth looked full at failure
    uint64_t fail_depth_lt_capacity = 0;  // depth looked NOT full at failure
    uint64_t received = 0;
    uint64_t gaps = 0;  // received[next] != received[prev]+1 transitions
    uint64_t stats_dropped = 0;
    uint64_t stats_sent = 0;
    std::vector<std::string> gap_examples;
    std::vector<uint64_t> fail_depth_samples;
};

void run_scenario_a(ScenarioAResult& r) {
    BoundedQueue<uint64_t> q(8, DropPolicy::DropOldest, true, "cr040A", "cr040A");

    std::atomic<bool> producer_done{false};
    std::atomic<bool> consumer_running{true};

    auto consumer = std::thread([&] {
        uint64_t last = 0;
        bool first = true;
        while (consumer_running.load(std::memory_order_relaxed)) {
            auto item = q.try_pop();
            if (!item) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
                continue;
            }
            const uint64_t seq = item->value;
            if (first) {
                last = seq;
                first = false;
            } else {
                if (seq != last + 1) {
                    ++r.gaps;
                    if (r.gap_examples.size() < 8) {
                        char buf[96];
                        std::snprintf(buf, sizeof(buf),
                                      "received %llu then %llu (expected %llu)",
                                      static_cast<unsigned long long>(last),
                                      static_cast<unsigned long long>(seq),
                                      static_cast<unsigned long long>(last + 1));
                        r.gap_examples.emplace_back(buf);
                    }
                }
                last = std::max(last, seq);
            }
            ++r.received;
        }
        // final drain
        for (;;) {
            auto item = q.try_pop();
            if (!item) break;
            const uint64_t seq = item->value;
            if (seq != last + 1) ++r.gaps;
            last = std::max(last, seq);
            ++r.received;
        }
    });

    const auto deadline = Clock::now() + std::chrono::seconds(2);
    uint64_t seq = 0;
    while (Clock::now() < deadline) {
        ++seq;
        std::optional<CommEvent> event;
        const bool ok = q.enqueue(seq, event);
        if (ok) {
            ++r.sent_ok;
        } else {
            ++r.send_failures;
            const uint64_t depth = q.size();
            if (depth >= q.capacity()) {
                ++r.fail_depth_ge_capacity;
            } else {
                ++r.fail_depth_lt_capacity;
            }
            if (r.fail_depth_samples.size() < 16) {
                r.fail_depth_samples.push_back(depth);
            }
        }
    }
    producer_done.store(true);
    // let consumer drain the tail
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    consumer_running.store(false);
    consumer.join();

    const auto stats = q.stats();
    r.stats_dropped = stats.dropped_count;
    r.stats_sent = stats.sent_count;
}

struct ScenarioBResult {
    uint64_t send_failures = 0;
    std::vector<uint64_t> failed_seqs;
    uint64_t received = 0;
    uint64_t max_received = 0;
    uint64_t stats_dropped = 0;
    uint64_t stats_overwritten = 0;
    std::vector<uint64_t> missing_in_tail;  // ids in [N-7, N] never received
};

void run_scenario_b(ScenarioBResult& r) {
    constexpr uint64_t kTotal = 1000;
    constexpr uint64_t kCapacity = 8;
    BoundedQueue<uint64_t> q(kCapacity, DropPolicy::KeepLatest, true, "cr040B", "cr040B");

    std::atomic<bool> producer_done{false};
    std::atomic<bool> consumer_running{true};

    std::vector<uint64_t> received_seqs;
    std::mutex received_mutex;

    auto consumer = std::thread([&] {
        while (consumer_running.load(std::memory_order_relaxed)) {
            auto item = q.try_pop();
            if (item) {
                std::lock_guard<std::mutex> lock(received_mutex);
                received_seqs.push_back(item->value);
                continue;
            }
            if (producer_done.load(std::memory_order_relaxed)) {
                // drain fast once producer is finished
                for (;;) {
                    auto more = q.try_pop();
                    if (!more) break;
                    std::lock_guard<std::mutex> lock(received_mutex);
                    received_seqs.push_back(more->value);
                }
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    for (uint64_t seq = 1; seq <= kTotal; ++seq) {
        std::optional<CommEvent> event;
        if (q.enqueue(seq, event)) {
            // accepted
        } else {
            ++r.send_failures;
            if (r.failed_seqs.size() < 32) r.failed_seqs.push_back(seq);
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    producer_done.store(true);
    consumer.join();

    {
        std::lock_guard<std::mutex> lock(received_mutex);
        r.received = received_seqs.size();
        for (uint64_t s : received_seqs) r.max_received = std::max(r.max_received, s);
    }
    const uint64_t tail_lo = kTotal - kCapacity + 1;  // 993
    for (uint64_t s = tail_lo; s <= kTotal; ++s) {
        if (std::find(received_seqs.begin(), received_seqs.end(), s) ==
            received_seqs.end()) {
            r.missing_in_tail.push_back(s);
        }
    }
    const auto stats = q.stats();
    r.stats_dropped = stats.dropped_count;
    r.stats_overwritten = stats.overwritten_count;
}

}  // namespace

int main() {
    std::printf("=== CR-040 Scenario A: DropOldest, capacity=8, busy consumer, 2s ===\n");
    ScenarioAResult a;
    run_scenario_a(a);
    std::printf("A: sent_ok=%llu send_failures=%llu received=%llu gaps=%llu\n",
                (unsigned long long)a.sent_ok, (unsigned long long)a.send_failures,
                (unsigned long long)a.received, (unsigned long long)a.gaps);
    std::printf("A: failures with depth>=capacity: %llu, with depth<capacity: %llu\n",
                (unsigned long long)a.fail_depth_ge_capacity,
                (unsigned long long)a.fail_depth_lt_capacity);
    std::printf("A: stats.dropped_count=%llu stats.sent_count=%llu\n",
                (unsigned long long)a.stats_dropped, (unsigned long long)a.stats_sent);
    for (const auto& g : a.gap_examples) std::printf("A: GAP: %s\n", g.c_str());
    std::printf("A: fail depth samples:");
    for (uint64_t d : a.fail_depth_samples) std::printf(" %llu", (unsigned long long)d);
    std::printf("\n");
    const bool a_confirmed = a.send_failures > 0 && a.gaps > 0;
    std::printf("A verdict: %s\n", a_confirmed ? "CONFIRMED (newest lost under DropOldest)"
                                               : "NOT REPRODUCED\n");

    std::printf("\n=== CR-040 Scenario B: KeepLatest, capacity=8, slow consumer, 1000 ids ===\n");
    ScenarioBResult b;
    run_scenario_b(b);
    std::printf("B: send_failures=%llu received=%llu max_received=%llu\n",
                (unsigned long long)b.send_failures, (unsigned long long)b.received,
                (unsigned long long)b.max_received);
    std::printf("B: stats.dropped_count=%llu stats.overwritten_count=%llu\n",
                (unsigned long long)b.stats_dropped,
                (unsigned long long)b.stats_overwritten);
    std::printf("B: failed seqs:");
    for (uint64_t s : b.failed_seqs) std::printf(" %llu", (unsigned long long)s);
    std::printf("\nB: missing in tail [993..1000]:");
    for (uint64_t s : b.missing_in_tail) std::printf(" %llu", (unsigned long long)s);
    std::printf("\n");
    bool b_confirmed = b.send_failures > 0 || b.max_received != 1000 ||
                             !b.missing_in_tail.empty();
    std::printf("B verdict: %s\n",
                b_confirmed ? "CONFIRMED (latest ids lost under KeepLatest)"
                            : "NOT REPRODUCED");

    // B2: adversarial timing for the same KeepLatest race. Producer fast
    // (50us between sends, 20000 sends) so the queue is permanently full and
    // every send takes the KeepLatest recycle path; consumer pops every ~1ms
    // so it is occasionally inside try_pop (holding the consumer flag the
    // recycle path needs). Any failed send here = the newest item dropped.
    std::printf("\n=== CR-040 Scenario B2: KeepLatest, capacity=8, saturated queue ===\n");
    {
        constexpr uint64_t kTotal = 20000;
        BoundedQueue<uint64_t> q(8, DropPolicy::KeepLatest, true, "cr040B2", "cr040B2");
        std::atomic<bool> producer_done{false};
        std::vector<uint64_t> received_seqs;
        std::mutex received_mutex;
        auto consumer = std::thread([&] {
            while (true) {
                auto item = q.try_pop();
                if (item) {
                    std::lock_guard<std::mutex> lock(received_mutex);
                    received_seqs.push_back(item->value);
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
                    continue;
                }
                if (producer_done.load(std::memory_order_relaxed)) {
                    for (;;) {
                        auto more = q.try_pop();
                        if (!more) break;
                        std::lock_guard<std::mutex> lock(received_mutex);
                        received_seqs.push_back(more->value);
                    }
                    break;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        });
        uint64_t failures_b2 = 0;
        std::vector<uint64_t> failed_b2;
        for (uint64_t seq = 1; seq <= kTotal; ++seq) {
            std::optional<CommEvent> event;
            if (!q.enqueue(seq, event)) {
                ++failures_b2;
                if (failed_b2.size() < 32) failed_b2.push_back(seq);
            }
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        producer_done.store(true);
        consumer.join();
        uint64_t max_recv_b2 = 0;
        for (uint64_t s : received_seqs) max_recv_b2 = std::max(max_recv_b2, s);
        const auto stats_b2 = q.stats();
        std::printf("B2: send_failures=%llu received=%zu max_received=%llu\n",
                    (unsigned long long)failures_b2, received_seqs.size(),
                    (unsigned long long)max_recv_b2);
        std::printf("B2: stats.dropped_count=%llu stats.overwritten_count=%llu\n",
                    (unsigned long long)stats_b2.dropped_count,
                    (unsigned long long)stats_b2.overwritten_count);
        std::printf("B2: failed seqs:");
        for (uint64_t s : failed_b2) std::printf(" %llu", (unsigned long long)s);
        std::printf("\nB2 verdict: %s\n",
                    (failures_b2 > 0 || max_recv_b2 != kTotal)
                        ? "CONFIRMED (latest ids lost under KeepLatest)"
                        : "NOT REPRODUCED");
        if (failures_b2 > 0 || max_recv_b2 != kTotal) b_confirmed = true;
    }

    return (a_confirmed || b_confirmed) ? 1 : 0;
}
