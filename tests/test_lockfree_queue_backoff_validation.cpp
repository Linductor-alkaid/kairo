#include <gtest/gtest.h>
#include "kairo/util/lockfree_queue.hpp"
#include <kairo/lockfree_task_executor.hpp>

#include <cstddef>
#include <algorithm>
#include <atomic>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

using kairo::LockFreeTaskExecutor;
using kairo::util::LockFreeQueue;

namespace {

void count_before_publish(void* context) {
    static_cast<std::atomic<size_t>*>(context)->fetch_add(1, std::memory_order_relaxed);
}

} // namespace

TEST(LockFreeQueueHookTest, ConcurrentInstallAndPush) {
    constexpr size_t kProducerCount = 4;
    constexpr size_t kPushesPerProducer = 2000;
    constexpr size_t kHookUpdates = 10000;

    LockFreeQueue<size_t> queue(1024);
    std::atomic<size_t> next_value{0};
    std::atomic<size_t> accepted{0};
    std::atomic<size_t> hook_calls{0};
    std::atomic<bool> producers_finished{false};
    std::vector<size_t> consumed;
    consumed.reserve(kProducerCount * kPushesPerProducer);

    std::thread consumer([&] {
        while (!producers_finished.load(std::memory_order_acquire) ||
               consumed.size() < accepted.load(std::memory_order_acquire)) {
            size_t value = 0;
            if (queue.pop(value)) {
                consumed.push_back(value);
            } else {
                std::this_thread::yield();
            }
        }
    });

    std::vector<std::thread> producers;
    producers.reserve(kProducerCount);
    for (size_t producer = 0; producer < kProducerCount; ++producer) {
        producers.emplace_back([&] {
            for (size_t pushed = 0; pushed < kPushesPerProducer;) {
                const size_t value = next_value.fetch_add(1, std::memory_order_relaxed);
                if (queue.push(value)) {
                    accepted.fetch_add(1, std::memory_order_release);
                    ++pushed;
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }

    std::thread installer([&] {
        for (size_t update = 0; update < kHookUpdates; ++update) {
            queue.set_before_publish_hook(count_before_publish, &hook_calls);
            queue.set_before_publish_hook(nullptr, nullptr);
        }
    });

    for (auto& producer : producers) {
        producer.join();
    }
    producers_finished.store(true, std::memory_order_release);
    installer.join();
    consumer.join();
    queue.set_before_publish_hook(nullptr, nullptr);

    EXPECT_EQ(accepted.load(std::memory_order_acquire), kProducerCount * kPushesPerProducer);
    EXPECT_EQ(consumed.size(), accepted.load(std::memory_order_acquire));
    std::sort(consumed.begin(), consumed.end());
    EXPECT_EQ(std::adjacent_find(consumed.begin(), consumed.end()), consumed.end());
}

TEST(LockFreeQueueBackoffValidationTest, test_lockfree_queue_rejects_zero_backoff) {
    EXPECT_THROW((LockFreeQueue<int>(64, 0)), std::invalid_argument);
}

TEST(LockFreeQueueBackoffValidationTest, test_lockfree_queue_huge_backoff_is_clamped) {
    LockFreeQueue<int> q(64, std::numeric_limits<size_t>::max());

    int out = 0;
    EXPECT_TRUE(q.push(7));
    EXPECT_TRUE(q.push(11));
    EXPECT_TRUE(q.pop(out));
    EXPECT_EQ(out, 7);
    EXPECT_TRUE(q.pop(out));
    EXPECT_EQ(out, 11);
    EXPECT_FALSE(q.pop(out));
}

TEST(LockFreeQueueBackoffValidationTest, test_lockfree_task_executor_invalid_backoff_throws) {
    EXPECT_THROW((LockFreeTaskExecutor(64, 0)), std::invalid_argument);
}

TEST(LockFreeQueueBackoffValidationTest, test_lockfree_queue_normal_backoff_works) {
    LockFreeQueue<int> q(64, 2);

    int out = 0;
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(q.push(i));
    }
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(q.pop(out));
        EXPECT_EQ(out, i);
    }
    EXPECT_TRUE(q.empty());
}
