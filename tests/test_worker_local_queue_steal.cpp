// P-002 regression test for WorkerLocalQueue::steal() vs owner pop() race.
//
// Background: daily plan P-002 (2026-06-09) flagged that steal() computed
// its source index from back_index_ - 1 (or, in a hypothetical lock-free
// rewrite, from old_size - 1) without re-validating against the current
// size_. While the current implementation is mutex-protected (so the
// race is not directly visible), the dependency on back_index_ as the
// sole source of truth for "the last occupied slot" is fragile and was
// the symptom plan P-002 called out. The fix re-derives the steal
// position from a freshly-loaded size_ and uses an overflow-safe
// (back_index_ + buf_size - 1) % buf_size form.
//
// This test exercises the end-to-end invariant: every pushed task must
// be observed exactly once (either by the owner pop or by a thief
// steal) under aggressive concurrent push/pop/steal load.

#include <gtest/gtest.h>

#include "kairo/thread_pool/worker_local_queue.hpp"
#include "kairo/types.hpp"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using kairo::Task;
using kairo::WorkerLocalQueue;

namespace {

// Build a Task that increments `counter` when executed.
// NOTE: Task is neither copyable nor movable (it holds a std::atomic<bool>),
// so we never return a Task by value. Callers should use the helper below
// to push a fresh task directly.
void push_increment(WorkerLocalQueue& q, std::atomic<uint64_t>* counter) {
    Task t;
    t.task_id = "inc";
    t.priority = kairo::TaskPriority::NORMAL;
    t.function = [counter]() { counter->fetch_add(1, std::memory_order_relaxed); };
    t.submit_time_ns = 0;
    t.timeout_ms = 0;
    t.dependencies = {};
    t.cancelled.store(false, std::memory_order_release);
    // push(const Task&) takes by const reference, so the temporary binds
    // directly to the parameter and the deleted move constructor is
    // never invoked.
    (void)q.push(t);
}

} // namespace

// Basic smoke test: push + pop / push + steal round trip.
TEST(WorkerLocalQueueTest, PushPopStealRoundTrip) {
    WorkerLocalQueue q(/*capacity=*/16);

    Task t;
    t.task_id = "t0";
    t.priority = kairo::TaskPriority::NORMAL;
    t.function = []() {};
    ASSERT_TRUE(q.push(t));

    Task stolen;
    ASSERT_TRUE(q.steal(stolen));
    EXPECT_EQ(stolen.task_id, "t0");
    EXPECT_TRUE(q.empty());
    EXPECT_FALSE(q.steal(stolen));
}

// Steal respects LIFO from the back: with two pushes [a, b], the first
// steal must return b (the most recently pushed).
TEST(WorkerLocalQueueTest, StealTakesBackElement) {
    WorkerLocalQueue q(/*capacity=*/16);

    Task a;
    a.task_id = "a";
    a.function = []() {};
    Task b;
    b.task_id = "b";
    b.function = []() {};
    ASSERT_TRUE(q.push(a));
    ASSERT_TRUE(q.push(b));

    Task stolen;
    ASSERT_TRUE(q.steal(stolen));
    EXPECT_EQ(stolen.task_id, "b");

    ASSERT_TRUE(q.steal(stolen));
    EXPECT_EQ(stolen.task_id, "a");
}

// Regression for P-002: under concurrent owner-pop + thief-steal, each
// task must be executed exactly once. We push a fixed batch of tasks,
// then run two threads:
//   * "owner" thread: aggressively pops from the front
//   * "thief" thread: aggressively steals from the back
// and verify that pop_count + steal_count == pushed_count and that the
// per-task atomic counter == 1.
TEST(WorkerLocalQueueTest, StealRaceTest) {
    // kRounds bounds the race iterations on fast machines; kTimeBudget
    // bounds them on slow/loaded ones (2-core CI runners in Debug can
    // take ~10x longer per round than a workstation, and a fixed round
    // count previously blew the 30s ctest timeout there). kMinRounds
    // keeps meaningful coverage even on a heavily loaded machine.
    constexpr int kRounds = 10000;
    constexpr int kMinRounds = 128;
    constexpr int kBatch = 64;
    constexpr auto kTimeBudget = std::chrono::seconds{2};
    const auto budget_deadline = std::chrono::steady_clock::now() + kTimeBudget;

    for (int round = 0; round < kRounds; ++round) {
        WorkerLocalQueue q(/*capacity=*/kBatch + 4);

        // Each task is tagged with a unique id; we hand out one counter
        // per task and assert it gets incremented exactly once.
        std::vector<std::atomic<uint64_t>> counters(kBatch);
        for (auto& c : counters) {
            c.store(0, std::memory_order_relaxed);
        }

        for (int i = 0; i < kBatch; ++i) {
            push_increment(q, &counters[i]);
        }

        std::atomic<int> pop_count{0};
        std::atomic<int> steal_count{0};
        std::atomic<bool> owner_done{false};

        std::thread owner([&]() {
            Task t;
            while (!owner_done.load(std::memory_order_acquire)) {
                if (q.pop(t)) {
                    t.function();
                    pop_count.fetch_add(1, std::memory_order_relaxed);
                } else {
                    std::this_thread::yield();
                }
            }
            // Drain anything left after the thief finishes.
            while (q.pop(t)) {
                t.function();
                pop_count.fetch_add(1, std::memory_order_relaxed);
            }
        });

        std::thread thief([&]() {
            Task t;
            // Loop until we have observed every task exactly once.
            while (steal_count.load(std::memory_order_relaxed)
                       + pop_count.load(std::memory_order_relaxed)
                   < kBatch) {
                if (q.steal(t)) {
                    t.function();
                    steal_count.fetch_add(1, std::memory_order_relaxed);
                } else {
                    std::this_thread::yield();
                }
            }
            EXPECT_FALSE(q.steal(t)) << "queue should be drained";
        });

        // Stop the owner as soon as the batch is fully consumed or after
        // a bounded contention window, whichever comes first. The window
        // only matters while the drain is still in progress (that is the
        // pop/steal interleaving under test); once every task is observed,
        // keeping the owner spinning on an empty queue adds wall time on
        // loaded CI runners without exercising anything new.
        const auto contention_deadline = std::chrono::steady_clock::now() +
                                         std::chrono::microseconds(200);
        while (pop_count.load(std::memory_order_relaxed) +
                       steal_count.load(std::memory_order_relaxed) <
                   kBatch &&
               std::chrono::steady_clock::now() < contention_deadline) {
            std::this_thread::yield();
        }
        owner_done.store(true, std::memory_order_release);

        owner.join();
        thief.join();

        ASSERT_EQ(pop_count.load() + steal_count.load(), kBatch)
            << "round=" << round
            << " pop=" << pop_count.load()
            << " steal=" << steal_count.load();

        for (int i = 0; i < kBatch; ++i) {
            EXPECT_EQ(counters[i].load(std::memory_order_relaxed), 1u)
                << "round=" << round << " i=" << i
                << " pop=" << pop_count.load()
                << " steal=" << steal_count.load();
        }

        if (round + 1 >= kMinRounds &&
            std::chrono::steady_clock::now() >= budget_deadline) {
            break;
        }
    }
}

// Sanity test for empty() consistency with size().
TEST(WorkerLocalQueueTest, EmptySizeConsistency) {
    WorkerLocalQueue q(/*capacity=*/8);
    EXPECT_TRUE(q.empty());
    EXPECT_EQ(q.size(), 0u);

    Task t;
    t.task_id = "x";
    t.function = []() {};
    ASSERT_TRUE(q.push(t));
    EXPECT_FALSE(q.empty());
    EXPECT_EQ(q.size(), 1u);

    Task out;
    ASSERT_TRUE(q.pop(out));
    EXPECT_TRUE(q.empty());
    EXPECT_EQ(q.size(), 0u);
}
