// CR-024 / NN-05: optional anti-starvation aging for PriorityScheduler.
//
// Contract tests for set_aging_policy(enabled, boost_interval_ns):
//   - Default (disabled) behavior must stay bit-identical to the strict
//     priority scheduler (case A).
//   - When enabled, each queue-top task's effective priority is
//     effective = min(CRITICAL, base + waited / boost_interval), and the
//     best queue-top by (effective, EDF, submit FIFO) is popped. In-queue
//     heap order is not reshuffled.
//
// Timing-dependent cases deliberately oversize the sleep/interval ratio
// (>= 5x) so slow CI machines cannot undershoot a boost level: sleep_for
// guarantees at least the requested duration and extra delay only
// increases waited_ns, which can only help the starved task.

#include <gtest/gtest.h>

#include "kairo/types.hpp"
#include "kairo/thread_pool/priority_scheduler.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace kairo;

namespace {

constexpr int64_t kMsToNs = 1'000'000;

int64_t steady_now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Task carries std::atomic<bool> (not movable/copyable construct-wise), so
// fill an already-constructed instance instead of returning one by value.
void fill_task(Task& task, const std::string& id, TaskPriority priority,
               int64_t submit_time_ns, int64_t deadline_ns = 0) {
    task.task_id = id;
    task.priority = priority;
    task.submit_time_ns = submit_time_ns;
    task.deadline_ns = deadline_ns;
    task.function = []() noexcept {};
}

}  // namespace

// --------------------------------------------------------------------------
// (a) Aging disabled by default: strict priority, no promotion.
// --------------------------------------------------------------------------
TEST(PrioritySchedulerAgingTest, AgingDisabledByDefaultStrictPriority) {
    PriorityScheduler scheduler;  // aging defaults to disabled

    // One LOW submitted first (earliest), then a flood of fresh CRITICALs.
    Task low;
    fill_task(low, "low_first", TaskPriority::LOW, /*submit=*/1000);
    scheduler.enqueue(low);

    for (int i = 0; i < 16; ++i) {
        Task critical;
        fill_task(critical, "crit_" + std::to_string(i), TaskPriority::CRITICAL,
                  /*submit=*/2000 + i);
        scheduler.enqueue(critical);
    }

    // Strict priority: all 16 CRITICALs must come out before the LOW.
    Task out;
    for (int i = 0; i < 16; ++i) {
        ASSERT_TRUE(scheduler.dequeue(out)) << "iteration " << i;
        EXPECT_EQ(out.priority, TaskPriority::CRITICAL) << "iteration " << i;
    }
    ASSERT_TRUE(scheduler.dequeue(out));
    EXPECT_EQ(out.priority, TaskPriority::LOW);
    EXPECT_TRUE(scheduler.empty());
    EXPECT_FALSE(scheduler.dequeue(out));
}

// --------------------------------------------------------------------------
// (b) LOW starved past CRITICAL cap beats fresh CRITICALs (FIFO tie-break
//     at equal effective priority: earlier submit_time wins).
// --------------------------------------------------------------------------
TEST(PrioritySchedulerAgingTest, AgingPromotesStarvedLowAboveFreshCritical) {
    PriorityScheduler scheduler;
    // 50ms per level; cap is 3 levels -> 150ms of waiting tops out at
    // effective == CRITICAL. Sleep 250ms = 5x interval for CI margin.
    scheduler.set_aging_policy(true, 50 * kMsToNs);

    Task low;
    fill_task(low, "starved_low", TaskPriority::LOW, steady_now_ns());
    scheduler.enqueue(low);

    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    // Fresh CRITICAL flood: effective stays 3 (already top level); LOW's
    // waited/interval >= 5 tops out at 3 as well. Equal effective + no
    // deadlines -> submit FIFO decides: earlier LOW must win.
    const int64_t base = steady_now_ns();
    for (int i = 0; i < 16; ++i) {
        Task critical;
        fill_task(critical, "fresh_crit_" + std::to_string(i),
                  TaskPriority::CRITICAL, base + i);
        scheduler.enqueue(critical);
    }

    Task out;
    ASSERT_TRUE(scheduler.dequeue(out));
    EXPECT_EQ(out.task_id, "starved_low")
        << "aged LOW (earliest submit at equal effective priority) must "
           "dequeue first";

    for (int i = 0; i < 16; ++i) {
        ASSERT_TRUE(scheduler.dequeue(out)) << "iteration " << i;
        EXPECT_EQ(out.priority, TaskPriority::CRITICAL) << "iteration " << i;
    }
    EXPECT_TRUE(scheduler.empty());
}

// --------------------------------------------------------------------------
// (c) One boost level lifts LOW above NORMAL; remaining NORMALs keep FIFO.
// --------------------------------------------------------------------------
TEST(PrioritySchedulerAgingTest, AgingSingleLevelPromotionBeatsNormal) {
    PriorityScheduler scheduler;
    scheduler.set_aging_policy(true, 100 * kMsToNs);

    Task low;
    fill_task(low, "single_boost_low", TaskPriority::LOW, steady_now_ns());
    scheduler.enqueue(low);

    // ~1.5x the interval -> at least one full level gained (effective 1).
    // Overshooting only raises LOW further, which still beats NORMAL.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    const int64_t base = steady_now_ns();
    for (int i = 0; i < 8; ++i) {
        Task normal;
        fill_task(normal, "norm_" + std::to_string(i), TaskPriority::NORMAL,
                  base + i);
        scheduler.enqueue(normal);
    }

    Task out;
    ASSERT_TRUE(scheduler.dequeue(out));
    EXPECT_EQ(out.task_id, "single_boost_low")
        << "LOW boosted by one level must beat NORMAL at equal effective "
           "priority (earlier submit wins)";

    for (int i = 0; i < 8; ++i) {
        ASSERT_TRUE(scheduler.dequeue(out)) << "iteration " << i;
        EXPECT_EQ(out.task_id, "norm_" + std::to_string(i))
            << "remaining NORMALs must keep submit FIFO order";
    }
    EXPECT_TRUE(scheduler.empty());
}

// --------------------------------------------------------------------------
// (d) With aging enabled but nobody waiting long enough, EDF inside one
//     level must not drift: deadline ascending, no-deadline last.
// --------------------------------------------------------------------------
TEST(PrioritySchedulerAgingTest, AgingNoWaitKeepsEdfWithinLevel) {
    PriorityScheduler scheduler;
    // 1h interval: an immediate dequeue can never accumulate a boost.
    scheduler.set_aging_policy(true, 3'600'000'000'000LL);

    const int64_t t0 = steady_now_ns();
    Task late;
    fill_task(late, "edf_late", TaskPriority::NORMAL, t0, t0 + 900'000);
    Task none;
    fill_task(none, "edf_none", TaskPriority::NORMAL, t0 + 1, 0);
    Task early;
    fill_task(early, "edf_early", TaskPriority::NORMAL, t0 + 2, t0 + 100'000);

    scheduler.enqueue(late);
    scheduler.enqueue(none);
    scheduler.enqueue(early);

    Task out;
    ASSERT_TRUE(scheduler.dequeue(out));
    EXPECT_EQ(out.task_id, "edf_early");
    ASSERT_TRUE(scheduler.dequeue(out));
    EXPECT_EQ(out.task_id, "edf_late");
    ASSERT_TRUE(scheduler.dequeue(out));
    EXPECT_EQ(out.task_id, "edf_none") << "no-deadline task must stay last";
    EXPECT_TRUE(scheduler.empty());
}

// --------------------------------------------------------------------------
// (e) dequeue_batch follows the same effective-priority global order.
// --------------------------------------------------------------------------
TEST(PrioritySchedulerAgingTest, AgingDequeueBatchRespectsEffectiveOrder) {
    PriorityScheduler scheduler;
    scheduler.set_aging_policy(true, 50 * kMsToNs);

    Task low;
    fill_task(low, "batch_low", TaskPriority::LOW, steady_now_ns());
    scheduler.enqueue(low);

    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    const int64_t base = steady_now_ns();
    for (int i = 0; i < 4; ++i) {
        Task critical;
        fill_task(critical, "batch_crit_" + std::to_string(i),
                  TaskPriority::CRITICAL, base + i);
        scheduler.enqueue(critical);
    }

    std::unique_ptr<Task> out[32];
    const size_t n = scheduler.dequeue_batch(out, 32);
    ASSERT_EQ(n, 5u);
    ASSERT_NE(out[0], nullptr);
    EXPECT_EQ(out[0]->task_id, "batch_low")
        << "dequeue_batch first element must be the aged LOW";
    for (size_t i = 1; i < n; ++i) {
        ASSERT_NE(out[i], nullptr) << "slot " << i;
        EXPECT_EQ(out[i]->priority, TaskPriority::CRITICAL) << "slot " << i;
    }
    EXPECT_TRUE(scheduler.empty());
}

// --------------------------------------------------------------------------
// (f) Invalid interval (<= 0) is clamped to 1ns: any waited_ns > 0 boosts
//     a task straight to the cap, so a LOW that waited at all crosses over
//     fresh CRITICALs at the FIFO tie-break.
// --------------------------------------------------------------------------
TEST(PrioritySchedulerAgingTest, AgingInvalidIntervalTreatedAsMinimal) {
    PriorityScheduler scheduler;
    scheduler.set_aging_policy(true, /*boost_interval_ns=*/-5);

    Task low;
    fill_task(low, "clamped_low", TaskPriority::LOW, steady_now_ns());
    scheduler.enqueue(low);

    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    Task critical;
    fill_task(critical, "clamped_crit", TaskPriority::CRITICAL,
              steady_now_ns());
    scheduler.enqueue(critical);

    Task out;
    ASSERT_TRUE(scheduler.dequeue(out));
    EXPECT_EQ(out.task_id, "clamped_low")
        << "with 1ns clamp, any wait tops LOW out at CRITICAL and its "
           "earlier submit must win";
    ASSERT_TRUE(scheduler.dequeue(out));
    EXPECT_EQ(out.task_id, "clamped_crit");
    EXPECT_TRUE(scheduler.empty());
}

// --------------------------------------------------------------------------
// (g) Concurrency smoke: a LOW submitted before a CRITICAL flood must be
//     reachable while 2 producer threads keep enqueueing. Prints how many
//     CRITICALs were dequeued before the LOW surfaced. With FIFO tie-break
//     at equal effective priority that count is expected to be ~0; a value
//     near 20000 indicates the tie-break is reversed (LIFO).
// --------------------------------------------------------------------------
TEST(PrioritySchedulerAgingTest, AgingConcurrentFloodSmoke) {
    constexpr int kFloodTotal = 20000;
    constexpr int kProducers = 2;

    PriorityScheduler scheduler;
    scheduler.set_aging_policy(true, 50 * kMsToNs);

    Task low;
    fill_task(low, "flood_low", TaskPriority::LOW, steady_now_ns());
    scheduler.enqueue(low);

    std::atomic<int> produced{0};
    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&scheduler, &produced] {
            for (;;) {
                const int i = produced.fetch_add(1);
                if (i >= kFloodTotal) break;
                Task critical;
                fill_task(critical, "flood_crit_" + std::to_string(i),
                          TaskPriority::CRITICAL, steady_now_ns());
                scheduler.enqueue(critical);
            }
        });
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(5);
    // Let the flood pile up before draining, so the smoke actually measures
    // the aging path under contention (otherwise the consumer can grab the
    // LOW before the first producer even starts). 200ms >= 4x interval:
    // LOW is deterministically at the effective-priority cap by then.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    bool found_low = false;
    int64_t criticals_before_low = 0;
    Task out;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!scheduler.dequeue(out)) {
            std::this_thread::yield();
            continue;
        }
        if (out.task_id == "flood_low") {
            found_low = true;
            break;
        }
        ++criticals_before_low;
    }

    for (auto& t : producers) {
        t.join();
    }

    ASSERT_TRUE(found_low)
        << "starved LOW not dequeued within 5s under CRITICAL flood";
    std::cout << "[  INFO  ] CRITICALs dequeued before starved LOW: "
              << criticals_before_low << " (FIFO tie-break expects ~0)"
              << std::endl;

    // Drain the rest: nothing may be lost.
    size_t total_after = 0;
    while (scheduler.dequeue(out)) {
        EXPECT_EQ(out.priority, TaskPriority::CRITICAL);
        ++total_after;
    }
    EXPECT_EQ(criticals_before_low + total_after,
              static_cast<int64_t>(kFloodTotal))
        << "every flooded CRITICAL must still be delivered exactly once";
    EXPECT_TRUE(scheduler.empty());
}

// --------------------------------------------------------------------------
// Extra diagnostic (beyond the a-g contract): cross-queue EDF rank. LOW
// deterministically pre-aged (submit_time in the past) to exactly HIGH's
// level must lose to nothing on the EDF field: a task with a deadline
// ranks ahead of a no-deadline task (Task::operator< convention: "no
// deadline ranks last"). Isolates the deadline sentinel from the FIFO
// tie-break exercised in (b).
// --------------------------------------------------------------------------
TEST(PrioritySchedulerAgingTest, AgingCrossQueueEdfNoDeadlineRanksLast) {
    PriorityScheduler scheduler;
    scheduler.set_aging_policy(true, 100 * kMsToNs);

    const int64_t t0 = steady_now_ns();
    // LOW waited 250ms at enqueue time (submit in the past): boost = 2 ->
    // effective = 2, tied with a fresh HIGH (level 2, no boost). Dequeue
    // happens immediately, so the tie holds deterministically.
    Task low_with_deadline;
    fill_task(low_with_deadline, "edf_low", TaskPriority::LOW,
              /*submit=*/t0 - 250 * kMsToNs, /*deadline=*/t0 + 100'000);
    scheduler.enqueue(low_with_deadline);

    Task high_no_deadline;
    fill_task(high_no_deadline, "edf_high_none", TaskPriority::HIGH,
              /*submit=*/t0);
    scheduler.enqueue(high_no_deadline);

    Task out;
    ASSERT_TRUE(scheduler.dequeue(out));
    EXPECT_EQ(out.task_id, "edf_low")
        << "at equal effective priority the deadline-bearing task must "
           "dequeue before the no-deadline task";
    ASSERT_TRUE(scheduler.dequeue(out));
    EXPECT_EQ(out.task_id, "edf_high_none");
    EXPECT_TRUE(scheduler.empty());
}
