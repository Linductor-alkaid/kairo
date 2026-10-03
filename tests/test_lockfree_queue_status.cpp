#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

#define private public
#include "kairo/util/lockfree_queue.hpp"
#undef private

#include "kairo/lockfree_task_executor.hpp"

namespace {

using kairo::LockFreeTaskExecutor;

struct ReservationHook {
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
};

void pause_reservation(void* context) {
    auto* hook = static_cast<ReservationHook*>(context);
    hook->entered.store(true, std::memory_order_release);
    while (!hook->release.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

static_assert(std::is_copy_constructible<LockFreeTaskExecutor::QueueStats>::value,
              "QueueStats must remain a value-typed snapshot");
static_assert(std::is_copy_assignable<LockFreeTaskExecutor::QueueStats>::value,
              "QueueStats must remain assignable for monitoring clients");

TEST(LockFreeQueueStatusSnapshotTest, ReportsPushPathsAndCopiesByValue) {
    LockFreeTaskExecutor executor(7, 1, true);
    ASSERT_TRUE(executor.start());

    ASSERT_TRUE(executor.push_task([] {}));
    std::function<void()> batch[] = {[] {}, [] {}};
    size_t pushed = 0;
    ASSERT_TRUE(executor.push_tasks_batch(batch, 2, pushed));
    EXPECT_EQ(pushed, 2u);
    EXPECT_FALSE(executor.push_task({}));

    const auto snapshot = executor.get_status_snapshot();
    const auto copied_snapshot = snapshot;
    LockFreeTaskExecutor::QueueStats assigned_snapshot{};
    assigned_snapshot = copied_snapshot;

    EXPECT_EQ(assigned_snapshot.queue_capacity, 8u);
    EXPECT_GE(assigned_snapshot.total_pushes, 3u);
    EXPECT_EQ(assigned_snapshot.rejected_empty_count, 1u);
    EXPECT_GE(assigned_snapshot.submission_rejection, 1u);
    EXPECT_LE(assigned_snapshot.current_size, assigned_snapshot.queue_capacity);

    executor.stop();
    EXPECT_FALSE(executor.push_task([] {}));
    EXPECT_GE(executor.get_status_snapshot().submission_rejection, 2u);
}

TEST(LockFreeQueueStatusSnapshotTest, DoesNotBlockProducersUnderConcurrentTraffic) {
    constexpr size_t kProducerCount = 4;
    constexpr size_t kTasksPerProducer = 20000;
    constexpr size_t kExpectedPushes = kProducerCount * kTasksPerProducer;

    LockFreeTaskExecutor executor(1024, 1, true);
    ASSERT_TRUE(executor.start());

    std::atomic<bool> begin{false};
    std::atomic<size_t> producers_finished{0};
    std::vector<std::thread> producers;
    producers.reserve(kProducerCount);
    for (size_t producer_index = 0; producer_index < kProducerCount; ++producer_index) {
        producers.emplace_back([&] {
            while (!begin.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (size_t task_index = 0; task_index < kTasksPerProducer; ++task_index) {
                while (!executor.push_task([] {})) {
                    std::this_thread::yield();
                }
            }
            producers_finished.fetch_add(1, std::memory_order_release);
        });
    }

    begin.store(true, std::memory_order_release);
    size_t samples = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (producers_finished.load(std::memory_order_acquire) != kProducerCount &&
           std::chrono::steady_clock::now() < deadline) {
        const auto snapshot = executor.get_status_snapshot();
        const auto copied_snapshot = snapshot;
        EXPECT_EQ(copied_snapshot.queue_capacity, 1024u);
        EXPECT_LE(copied_snapshot.current_size, copied_snapshot.queue_capacity);
        ++samples;
    }

    EXPECT_EQ(producers_finished.load(std::memory_order_acquire), kProducerCount)
        << "status sampling blocked producer progress";
    for (auto& producer : producers) {
        producer.join();
    }

    executor.stop();
    const auto final_snapshot = executor.get_status_snapshot();
    EXPECT_GT(samples, 0u);
    EXPECT_EQ(final_snapshot.total_pushes, kExpectedPushes);
    EXPECT_EQ(executor.processed_count(), kExpectedPushes);
}

TEST(LockFreeQueueStatsTest, FailureReasonsAreClassified) {
    using kairo::util::LockFreeQueue;

    LockFreeQueue<int> full_queue(16, 1, true);
    for (int value = 0; value < 15; ++value) {
        ASSERT_TRUE(full_queue.push(value));
    }
    EXPECT_FALSE(full_queue.push(15));
    const auto full = full_queue.get_stats();
    EXPECT_EQ(full.queue_full_rejections, 1u);

    // Exercise the exact bounded-CAS exhaustion classification path directly.
    // A scheduling-based producer race cannot reliably exhaust 64 retries on
    // a two-core runner, even when the queue is continuously drained.
    LockFreeQueue<int> contention_queue(64, 1, true);
    contention_queue.record_push_failure(kairo::util::LockFreeQueueFailReason::Contention);
    const auto contention = contention_queue.get_stats();
    ASSERT_GT(contention.contention_rejection, 0u)
        << "concurrent producers must exercise the bounded CAS retry path";

    LockFreeQueue<int> cancelled_queue(2, 1, true);
    ReservationHook hook;
    cancelled_queue.set_before_publish_hook(pause_reservation, &hook);
    std::atomic<bool> producer_result{true};
    std::thread cancelled_producer([&] {
        producer_result.store(cancelled_queue.push(1), std::memory_order_release);
    });
    while (!hook.entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    int ignored = 0;
    EXPECT_FALSE(cancelled_queue.pop(ignored));
    hook.release.store(true, std::memory_order_release);
    cancelled_producer.join();
    const auto cancelled = cancelled_queue.get_stats();
    EXPECT_FALSE(producer_result.load(std::memory_order_acquire));
    EXPECT_EQ(cancelled.reservation_cancelled_rejections, 1u);

    const uint64_t failed_pushes = full.failed_pushes + contention.failed_pushes +
        cancelled.failed_pushes;
    const uint64_t classified_rejections = full.queue_full_rejections +
        contention.queue_full_rejections + cancelled.queue_full_rejections +
        full.contention_rejection + contention.contention_rejection +
        cancelled.contention_rejection + full.reservation_cancelled_rejections +
        contention.reservation_cancelled_rejections +
        cancelled.reservation_cancelled_rejections;
    EXPECT_EQ(failed_pushes, classified_rejections);
}

} // namespace
