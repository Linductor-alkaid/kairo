#include <kairo/comm.hpp>

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace std::chrono_literals;

namespace {

using kairo::comm::ChannelOptions;
using kairo::comm::CommErrorCode;
using kairo::comm::CommEventKind;
using kairo::comm::DropPolicy;
using kairo::comm::MpscChannel;
using kairo::comm::SpscChannel;

ChannelOptions options(size_t capacity,
                       DropPolicy drop_policy = DropPolicy::RejectNewest) {
    ChannelOptions opts;
    opts.capacity = capacity;
    opts.drop_policy = drop_policy;
    return opts;
}

thread_local uint64_t* active_stored_destructions = nullptr;

class TrackedQueuePayload {
public:
    explicit TrackedQueuePayload(int value) noexcept : value_(value) {}

    TrackedQueuePayload(const TrackedQueuePayload& other) noexcept
        : value_(other.value_), stored_(true) {}

    TrackedQueuePayload(TrackedQueuePayload&& other) noexcept
        : value_(other.value_), stored_(true) {
        other.stored_ = false;
    }

    ~TrackedQueuePayload() {
        if (stored_ && active_stored_destructions != nullptr) {
            ++*active_stored_destructions;
        }
    }

private:
    int value_ = 0;
    bool stored_ = false;
};

struct BlockingCopyPayload {
    int value = 0;
    std::atomic<bool>* copy_entered = nullptr;
    std::atomic<bool>* allow_copy = nullptr;

    BlockingCopyPayload() = default;

    explicit BlockingCopyPayload(
        int input,
        std::atomic<bool>* entered = nullptr,
        std::atomic<bool>* allowed = nullptr) noexcept
        : value(input), copy_entered(entered), allow_copy(allowed) {}

    BlockingCopyPayload(const BlockingCopyPayload& other) noexcept
        : value(other.value),
          copy_entered(other.copy_entered),
          allow_copy(other.allow_copy) {
        if (copy_entered == nullptr) return;
        copy_entered->store(true, std::memory_order_release);
        while (!allow_copy->load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }

    BlockingCopyPayload(BlockingCopyPayload&&) noexcept = default;
    BlockingCopyPayload& operator=(const BlockingCopyPayload&) noexcept = default;
    BlockingCopyPayload& operator=(BlockingCopyPayload&&) noexcept = default;
};
TEST(CommChannelTest, SingleProducerSingleConsumerKeepsFifoOrder) {
    MpscChannel<int> channel(options(4));

    EXPECT_TRUE(channel.try_send(1));
    EXPECT_TRUE(channel.try_send(2));
    EXPECT_TRUE(channel.try_send(3));

    int value = 0;
    EXPECT_TRUE(channel.try_receive(value));
    EXPECT_EQ(value, 1);
    EXPECT_TRUE(channel.try_receive(value));
    EXPECT_EQ(value, 2);
    EXPECT_TRUE(channel.try_receive(value));
    EXPECT_EQ(value, 3);
    EXPECT_FALSE(channel.try_receive(value));

    const auto stats = channel.stats();
    EXPECT_EQ(stats.sent_count, 3U);
    EXPECT_EQ(stats.received_count, 3U);
    EXPECT_EQ(stats.peak_depth, 3U);
    EXPECT_EQ(stats.current_depth, 0U);
    EXPECT_EQ(channel.capacity(), 4U);
    EXPECT_TRUE(channel.empty());
}

TEST(CommChannelTest, MultipleProducersSingleConsumerReceivesEachValueOnce) {
    constexpr int kProducerCount = 4;
    constexpr int kItemsPerProducer = 250;
    constexpr int kTotalItems = kProducerCount * kItemsPerProducer;

    MpscChannel<int> channel(options(128));
    std::atomic<int> producers_done{0};
    std::vector<std::thread> producers;

    for (int producer = 0; producer < kProducerCount; ++producer) {
        producers.emplace_back([&, producer] {
            for (int i = 0; i < kItemsPerProducer; ++i) {
                const int value = producer * kItemsPerProducer + i;
                while (!channel.try_send(value)) {
                    std::this_thread::yield();
                }
            }
            producers_done.fetch_add(1, std::memory_order_acq_rel);
        });
    }

    std::unordered_set<int> received;
    received.reserve(kTotalItems);
    while (static_cast<int>(received.size()) < kTotalItems) {
        int value = -1;
        if (channel.try_receive(value)) {
            received.insert(value);
            continue;
        }
        EXPECT_LT(producers_done.load(std::memory_order_acquire), kProducerCount);
        std::this_thread::yield();
    }

    for (auto& producer : producers) {
        producer.join();
    }

    EXPECT_EQ(received.size(), static_cast<size_t>(kTotalItems));
    for (int value = 0; value < kTotalItems; ++value) {
        EXPECT_EQ(received.count(value), 1U);
    }
    EXPECT_EQ(channel.stats().sent_count, static_cast<uint64_t>(kTotalItems));
    EXPECT_EQ(channel.stats().received_count, static_cast<uint64_t>(kTotalItems));
}

TEST(CommChannelTest, SmallCapacitySurvivesHeavyConcurrentNodeReuse) {
    constexpr int kProducerCount = 8;
    constexpr int kItemsPerProducer = 4000;
    constexpr int kTotalItems = kProducerCount * kItemsPerProducer;

    MpscChannel<int> channel(options(4));
    std::atomic<bool> start{false};
    std::atomic<int> producers_done{0};
    std::vector<std::thread> producers;
    producers.reserve(kProducerCount);

    for (int producer = 0; producer < kProducerCount; ++producer) {
        producers.emplace_back([&, producer] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int index = 0; index < kItemsPerProducer; ++index) {
                const int value = producer * kItemsPerProducer + index;
                while (!channel.try_send(value)) {
                    std::this_thread::yield();
                }
            }
            producers_done.fetch_add(1, std::memory_order_release);
        });
    }

    std::vector<int> received;
    received.reserve(kTotalItems);
    std::thread consumer([&] {
        while (producers_done.load(std::memory_order_acquire) != kProducerCount ||
               !channel.empty()) {
            int value = -1;
            if (channel.try_receive(value)) {
                received.push_back(value);
            } else {
                std::this_thread::yield();
            }
        }
    });

    start.store(true, std::memory_order_release);
    for (auto& producer : producers) producer.join();
    consumer.join();

    ASSERT_EQ(received.size(), static_cast<size_t>(kTotalItems));
    std::vector<uint8_t> seen(kTotalItems, 0);
    for (const int value : received) {
        ASSERT_GE(value, 0);
        ASSERT_LT(value, kTotalItems);
        EXPECT_EQ(seen[static_cast<size_t>(value)]++, 0U);
    }
    EXPECT_EQ(channel.stats().sent_count, static_cast<uint64_t>(kTotalItems));
    EXPECT_EQ(channel.stats().received_count, static_cast<uint64_t>(kTotalItems));
}

TEST(CommChannelTest, RejectNewestReportsFullAndDropStats) {
    MpscChannel<int> channel(options(2));
    int dropped_events = 0;
    channel.set_event_callback([&](const kairo::comm::CommEvent& event) noexcept {
        if (event.kind == CommEventKind::Dropped) {
            ++dropped_events;
        }
    });

    EXPECT_TRUE(channel.try_send(10));
    EXPECT_TRUE(channel.try_send(11));
    EXPECT_FALSE(channel.try_send(12));

    auto stats = channel.stats();
    EXPECT_EQ(stats.sent_count, 2U);
    EXPECT_EQ(stats.dropped_count, 1U);
    EXPECT_EQ(stats.current_depth, 2U);
    EXPECT_EQ(dropped_events, 1);

    int value = 0;
    EXPECT_TRUE(channel.try_receive(value));
    EXPECT_EQ(value, 10);
    EXPECT_TRUE(channel.try_receive(value));
    EXPECT_EQ(value, 11);
}

TEST(CommChannelTest, DropOldestKeepsNewerValuesAndCountsDrop) {
    MpscChannel<int> channel(options(2, DropPolicy::DropOldest));

    EXPECT_TRUE(channel.try_send(1));
    EXPECT_TRUE(channel.try_send(2));
    EXPECT_TRUE(channel.try_send(3));

    int value = 0;
    EXPECT_TRUE(channel.try_receive(value));
    EXPECT_EQ(value, 2);
    EXPECT_TRUE(channel.try_receive(value));
    EXPECT_EQ(value, 3);
    EXPECT_FALSE(channel.try_receive(value));
    EXPECT_EQ(channel.stats().dropped_count, 1U);
}

TEST(CommChannelTest, KeepLatestOverwritesBufferedValues) {
    MpscChannel<int> channel(options(2, DropPolicy::KeepLatest));

    EXPECT_TRUE(channel.try_send(1));
    EXPECT_TRUE(channel.try_send(2));
    EXPECT_TRUE(channel.try_send(3));

    int value = 0;
    EXPECT_TRUE(channel.try_receive(value));
    EXPECT_EQ(value, 3);
    EXPECT_FALSE(channel.try_receive(value));
    EXPECT_EQ(channel.stats().overwritten_count, 1U);
}
TEST(CommChannelTest, DropOldestNeverRejectsAfterDisplacingAValue) {
    constexpr int kProducerCount = 4;
    constexpr int kAttemptsPerProducer = 4000;

    MpscChannel<TrackedQueuePayload> channel(
        options(1, DropPolicy::DropOldest));
    ASSERT_TRUE(channel.try_send(TrackedQueuePayload{0}));

    std::atomic<bool> start{false};
    std::atomic<uint64_t> failed_sends{0};
    std::atomic<uint64_t> failed_after_displacement{0};
    std::vector<std::thread> producers;
    producers.reserve(kProducerCount);

    for (int producer = 0; producer < kProducerCount; ++producer) {
        producers.emplace_back([&, producer] {
            uint64_t local_destructions = 0;
            active_stored_destructions = &local_destructions;
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int attempt = 0; attempt < kAttemptsPerProducer; ++attempt) {
                const uint64_t before = local_destructions;
                if (!channel.try_send(
                        TrackedQueuePayload{producer * kAttemptsPerProducer +
                                            attempt + 1})) {
                    failed_sends.fetch_add(1, std::memory_order_relaxed);
                    if (local_destructions != before) {
                        failed_after_displacement.fetch_add(
                            1, std::memory_order_relaxed);
                    }
                }
            }
            active_stored_destructions = nullptr;
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& producer : producers) producer.join();

    // CR-040: 置换路径的有界重试落地后，竞争拒绝已近乎消除（是否出现
    // 取决于极端时序），不再断言 failed_sends > 0——那建立在"竞争必然
    // 拒绝"的旧语义上。仍然必须成立的硬不变式：拒绝绝不允许发生在
    // "已置换旧值"之后（新值已构造却被丢弃且未入队）。
    (void)failed_sends;
    EXPECT_EQ(failed_after_displacement.load(std::memory_order_relaxed), 0U);
}
TEST(CommChannelTest, KeepLatestAdmittedBeforeCloseCompletesAndDelaysDrain) {
    MpscChannel<BlockingCopyPayload> channel(
        options(1, DropPolicy::KeepLatest));
    ASSERT_TRUE(channel.try_send(BlockingCopyPayload{1}));

    std::atomic<bool> copy_entered{false};
    std::atomic<bool> allow_copy{false};
    const BlockingCopyPayload replacement{2, &copy_entered, &allow_copy};
    std::atomic<bool> send_result{false};

    std::thread producer([&] {
        send_result.store(channel.try_send(replacement), std::memory_order_release);
    });

    while (!copy_entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    channel.close();

    EXPECT_FALSE(channel.is_drained());
    EXPECT_FALSE(channel.try_send(BlockingCopyPayload{3}));

    allow_copy.store(true, std::memory_order_release);
    producer.join();
    EXPECT_TRUE(send_result.load(std::memory_order_acquire));
    EXPECT_FALSE(channel.is_drained());

    BlockingCopyPayload received;
    ASSERT_TRUE(channel.try_receive(received));
    EXPECT_EQ(received.value, 2);
    EXPECT_TRUE(channel.is_drained());
}

TEST(CommChannelTest, IsDrainedRequiresCloseAndCompleteDrain) {
    MpscChannel<int> channel(options(2));

    EXPECT_FALSE(channel.is_drained());
    EXPECT_TRUE(channel.try_send(7));
    channel.close();
    EXPECT_FALSE(channel.is_drained());

    int value = 0;
    EXPECT_TRUE(channel.try_receive(value));
    EXPECT_EQ(value, 7);
    EXPECT_TRUE(channel.is_drained());
}
TEST(CommChannelTest, CloseRejectsNewSendsButAllowsDrainingBufferedValues) {
    MpscChannel<int> channel(options(2));

    EXPECT_TRUE(channel.try_send(7));
    channel.close();
    EXPECT_TRUE(channel.is_closed());
    EXPECT_FALSE(channel.try_send(8));

    int value = 0;
    EXPECT_TRUE(channel.try_receive(value));
    EXPECT_EQ(value, 7);
    EXPECT_FALSE(channel.try_receive(value));
    EXPECT_EQ(channel.stats().closed_send_count, 1U);
}

TEST(CommChannelTest, ConcurrentCloseRejectsLaterSendsAndDrainsEveryAcceptedValue) {
    constexpr int kProducerCount = 4;
    constexpr int kAttemptsPerProducer = 500;
    constexpr int kMaximumValues = kProducerCount * kAttemptsPerProducer;

    MpscChannel<int> channel(options(kMaximumValues));
    std::atomic<bool> start{false};
    std::atomic<bool> closed{false};
    std::atomic<int> attempts{0};
    std::atomic<int> producers_done{0};
    std::array<std::vector<int>, kProducerCount> accepted;
    std::array<bool, kProducerCount> accepted_after_close{};
    std::vector<std::thread> producers;

    for (int producer = 0; producer < kProducerCount; ++producer) {
        producers.emplace_back([&, producer] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            for (int index = 0; index < kAttemptsPerProducer; ++index) {
                const int value = producer * kAttemptsPerProducer + index;
                attempts.fetch_add(1, std::memory_order_release);
                if (channel.try_send(value)) accepted[producer].push_back(value);
                std::this_thread::yield();
            }
            while (!closed.load(std::memory_order_acquire)) std::this_thread::yield();
            accepted_after_close[producer] =
                channel.try_send(kMaximumValues + producer);
            producers_done.fetch_add(1, std::memory_order_release);
        });
    }

    std::vector<int> received;
    received.reserve(kMaximumValues);
    std::thread consumer([&] {
        while (producers_done.load(std::memory_order_acquire) != kProducerCount ||
               !channel.empty()) {
            int value = -1;
            if (channel.try_receive(value)) {
                received.push_back(value);
            } else {
                std::this_thread::yield();
            }
        }
    });

    start.store(true, std::memory_order_release);
    while (attempts.load(std::memory_order_acquire) < 64) std::this_thread::yield();
    channel.close();
    closed.store(true, std::memory_order_release);

    for (auto& producer : producers) producer.join();
    consumer.join();

    std::unordered_set<int> expected;
    for (int producer = 0; producer < kProducerCount; ++producer) {
        EXPECT_FALSE(accepted_after_close[producer]);
        expected.insert(accepted[producer].begin(), accepted[producer].end());
    }
    const std::unordered_set<int> observed(received.begin(), received.end());
    EXPECT_EQ(observed, expected);
    EXPECT_EQ(received.size(), expected.size());
    EXPECT_EQ(channel.stats().sent_count, expected.size());
    EXPECT_EQ(channel.stats().received_count, expected.size());
    EXPECT_GE(channel.stats().closed_send_count,
              static_cast<uint64_t>(kProducerCount));
}

TEST(CommChannelTest, ReceiveForTimesOutAndCloseWakesWaiter) {
    MpscChannel<int> channel(options(1));

    int value = 0;
    const auto timeout = channel.receive_for(value, 10ms);
    EXPECT_FALSE(timeout);
    EXPECT_EQ(timeout.error_code, CommErrorCode::Timeout);
    EXPECT_EQ(channel.stats().timeout_count, 1U);

    std::atomic<bool> waiter_done{false};
    std::thread waiter([&] {
        const auto result = channel.receive_for(value, 2s);
        EXPECT_FALSE(result);
        EXPECT_EQ(result.error_code, CommErrorCode::Closed);
        waiter_done.store(true, std::memory_order_release);
    });

    std::this_thread::sleep_for(20ms);
    EXPECT_FALSE(waiter_done.load(std::memory_order_acquire));
    channel.close();
    waiter.join();
    EXPECT_TRUE(waiter_done.load(std::memory_order_acquire));
}

TEST(CommChannelTest, SendForWaitsForCapacity) {
    MpscChannel<int> channel(options(1));
    EXPECT_TRUE(channel.try_send(1));

    std::thread consumer([&] {
        std::this_thread::sleep_for(20ms);
        int value = 0;
        EXPECT_TRUE(channel.try_receive(value));
        EXPECT_EQ(value, 1);
    });

    const auto result = channel.send_for(2, 1s);
    EXPECT_TRUE(result);
    consumer.join();

    int value = 0;
    EXPECT_TRUE(channel.try_receive(value));
    EXPECT_EQ(value, 2);
}

TEST(CommChannelTest, SendForTimesOutWhenRejectNewestChannelStaysFull) {
    MpscChannel<int> channel(options(1));
    EXPECT_TRUE(channel.try_send(1));

    const auto result = channel.send_for(2, 10ms);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error_code, CommErrorCode::Timeout);
    EXPECT_EQ(channel.stats().timeout_count, 1U);
}

TEST(CommChannelTest, SupportsNonTrivialAndMoveOnlyPayloads) {
    MpscChannel<std::string> strings(options(2));
    EXPECT_TRUE(strings.try_send(std::string{"sensor"}));

    std::string text;
    EXPECT_TRUE(strings.try_receive(text));
    EXPECT_EQ(text, "sensor");

    MpscChannel<std::unique_ptr<int>> pointers(options(1));
    EXPECT_TRUE(pointers.try_send(std::make_unique<int>(42)));

    std::unique_ptr<int> pointer;
    EXPECT_TRUE(pointers.try_receive(pointer));
    ASSERT_NE(pointer, nullptr);
    EXPECT_EQ(*pointer, 42);
}

TEST(CommChannelTest, SpscAliasUsesMpscChannelImplementation) {
    SpscChannel<int> channel(options(1));
    EXPECT_TRUE(channel.try_send(5));

    int value = 0;
    EXPECT_TRUE(channel.try_receive(value));
    EXPECT_EQ(value, 5);
}

} // namespace
