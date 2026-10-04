#include <kairo/comm.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {

using kairo::comm::CommErrorCode;
using kairo::comm::CommEventKind;
using kairo::comm::DropPolicy;
using kairo::comm::Topic;
using kairo::comm::TopicSubscription;
using kairo::comm::TopicSubscriptionOptions;

TopicSubscriptionOptions options(size_t capacity,
                                 DropPolicy policy = DropPolicy::RejectNewest,
                                 std::string name = {}) {
    TopicSubscriptionOptions result;
    result.capacity = capacity;
    result.drop_policy = policy;
    result.name = std::move(name);
    return result;
}

TEST(CommTopicTest, FansOutFifoWithoutReplayingPreSubscriptionMessages) {
    Topic<int> topic("frames");

    const auto before_subscribe = topic.publish(0);
    EXPECT_TRUE(before_subscribe);
    EXPECT_EQ(before_subscribe.matched_subscribers, 0U);

    auto planner = topic.subscribe(options(8, DropPolicy::RejectNewest, "planner"));
    ASSERT_TRUE(topic.publish(0));
    auto recorder = topic.subscribe(options(8, DropPolicy::RejectNewest, "recorder"));
    EXPECT_EQ(topic.subscriber_count(), 2U);

    int value = 0;
    ASSERT_TRUE(planner.try_receive(value));
    EXPECT_EQ(value, 0);
    EXPECT_FALSE(recorder.try_receive(value));

    for (int value = 1; value <= 4; ++value) {
        const auto result = topic.publish(value);
        EXPECT_TRUE(result);
        EXPECT_EQ(result.matched_subscribers, 2U);
        EXPECT_EQ(result.delivered_subscribers, 2U);
        EXPECT_EQ(result.rejected_subscribers, 0U);
    }

    for (auto* subscription : {&planner, &recorder}) {
        int value = 0;
        for (int expected = 1; expected <= 4; ++expected) {
            ASSERT_TRUE(subscription->try_receive(value));
            EXPECT_EQ(value, expected);
        }
        EXPECT_FALSE(subscription->try_receive(value));
    }
}

TEST(CommTopicTest, SlowSubscriberBackpressureDoesNotAffectFastSubscriber) {
    Topic<int> topic;
    auto fast = topic.subscribe(options(16));
    auto slow = topic.subscribe(options(1));

    int fast_value = 0;
    for (int value = 0; value < 8; ++value) {
        const auto result = topic.publish(value);
        EXPECT_EQ(result.matched_subscribers, 2U);
        EXPECT_EQ(result.delivered_subscribers, value == 0 ? 2U : 1U);
        EXPECT_EQ(result.rejected_subscribers, value == 0 ? 0U : 1U);
        ASSERT_TRUE(fast.try_receive(fast_value));
        EXPECT_EQ(fast_value, value);
    }

    int slow_value = -1;
    ASSERT_TRUE(slow.try_receive(slow_value));
    EXPECT_EQ(slow_value, 0);
    EXPECT_EQ(fast.stats().dropped_count, 0U);
    EXPECT_EQ(slow.stats().dropped_count, 7U);
}

TEST(CommTopicTest, DropPoliciesAndPublishResultArePerSubscription) {
    Topic<int> topic;
    auto reject = topic.subscribe(options(1, DropPolicy::RejectNewest, "reject"));
    auto oldest = topic.subscribe(options(1, DropPolicy::DropOldest, "oldest"));
    auto latest = topic.subscribe(options(1, DropPolicy::KeepLatest, "latest"));

    EXPECT_TRUE(topic.publish(1));
    const auto second = topic.publish(2);
    EXPECT_FALSE(second);
    EXPECT_EQ(second.matched_subscribers, 3U);
    EXPECT_EQ(second.delivered_subscribers, 2U);
    EXPECT_EQ(second.rejected_subscribers, 1U);

    int value = 0;
    ASSERT_TRUE(reject.try_receive(value));
    EXPECT_EQ(value, 1);
    ASSERT_TRUE(oldest.try_receive(value));
    EXPECT_EQ(value, 2);
    ASSERT_TRUE(latest.try_receive(value));
    EXPECT_EQ(value, 2);

    EXPECT_EQ(reject.stats().dropped_count, 1U);
    EXPECT_EQ(oldest.stats().dropped_count, 1U);
    EXPECT_EQ(latest.stats().overwritten_count, 1U);
}

TEST(CommTopicTest, CloseWakesWaiterAndBufferedMessagesDrainBeforeClosed) {
    Topic<int> topic;
    auto buffered = topic.subscribe(options(2));
    auto waiting = topic.subscribe(options(1));

    ASSERT_TRUE(topic.publish(7));
    int discarded = 0;
    ASSERT_TRUE(waiting.try_receive(discarded));

    std::atomic<bool> waiter_started{false};
    std::atomic<bool> waiter_closed{false};
    std::thread waiter([&] {
        int value = 0;
        waiter_started.store(true, std::memory_order_release);
        const auto result = waiting.receive_for(value, 2s);
        waiter_closed.store(!result && result.error_code == CommErrorCode::Closed,
                            std::memory_order_release);
    });

    while (!waiter_started.load(std::memory_order_acquire)) std::this_thread::yield();
    topic.close();
    waiter.join();

    EXPECT_TRUE(waiter_closed.load(std::memory_order_acquire));
    EXPECT_TRUE(buffered.is_closed());
    EXPECT_EQ(topic.subscriber_count(), 0U);

    int value = 0;
    EXPECT_TRUE(buffered.try_receive(value));
    EXPECT_EQ(value, 7);
    const auto closed = buffered.receive_for(value, 1ms);
    EXPECT_FALSE(closed);
    EXPECT_EQ(closed.error_code, CommErrorCode::Closed);

    const auto after_close = topic.publish(8);
    EXPECT_TRUE(after_close);
    EXPECT_EQ(after_close.matched_subscribers, 0U);
}

TEST(CommTopicTest, CloseSynchronizesWithBlockedReceiver) {
    for (int iteration = 0; iteration < 100; ++iteration) {
        Topic<int> topic;
        auto subscription = topic.subscribe(options(1));
        std::atomic<bool> receiver_ready{false};
        std::atomic<bool> receiver_closed{false};

        std::thread receiver([&] {
            int value = 0;
            receiver_ready.store(true, std::memory_order_release);
            const auto result = subscription.receive_for(value, 2s);
            receiver_closed.store(!result && result.error_code == CommErrorCode::Closed,
                                  std::memory_order_release);
        });

        while (!receiver_ready.load(std::memory_order_acquire)) std::this_thread::yield();
        topic.close();
        receiver.join();
        EXPECT_TRUE(receiver_closed.load(std::memory_order_acquire));
    }
}

TEST(CommTopicTest, SubscriptionCloseIsIdempotentAndDrainsBufferedMessages) {
    Topic<int> topic;
    auto subscription = topic.subscribe(options(2));
    ASSERT_TRUE(topic.publish(3));

    subscription.close();
    subscription.close();
    EXPECT_TRUE(subscription.is_closed());
    EXPECT_EQ(topic.subscriber_count(), 0U);

    int value = 0;
    EXPECT_TRUE(subscription.try_receive(value));
    EXPECT_EQ(value, 3);
    const auto closed = subscription.receive_for(value, 1ms);
    EXPECT_FALSE(closed);
    EXPECT_EQ(closed.error_code, CommErrorCode::Closed);
}

TEST(CommTopicTest, SubscriptionCloseWakesWaiter) {
    Topic<int> topic;
    auto subscription = topic.subscribe(options(1));
    std::atomic<bool> waiter_started{false};
    std::atomic<bool> waiter_closed{false};

    std::thread waiter([&] {
        int value = 0;
        waiter_started.store(true, std::memory_order_release);
        const auto result = subscription.receive_for(value, 2s);
        waiter_closed.store(!result && result.error_code == CommErrorCode::Closed,
                            std::memory_order_release);
    });

    while (!waiter_started.load(std::memory_order_acquire)) std::this_thread::yield();
    subscription.close();
    waiter.join();

    EXPECT_TRUE(waiter_closed.load(std::memory_order_acquire));
    EXPECT_EQ(topic.subscriber_count(), 0U);
}

TEST(CommTopicTest, SubscriptionDestructionUnregisters) {
    Topic<int> topic;
    {
        auto subscription = topic.subscribe(options(1));
        EXPECT_EQ(topic.subscriber_count(), 1U);
    }
    EXPECT_EQ(topic.subscriber_count(), 0U);
}

TEST(CommTopicTest, SubscribeAfterTopicCloseReturnsClosedSubscription) {
    Topic<int> topic;
    topic.close();

    auto subscription = topic.subscribe(options(1));
    EXPECT_TRUE(subscription.is_closed());
    EXPECT_EQ(topic.subscriber_count(), 0U);

    int value = 0;
    const auto result = subscription.receive_for(value, 1ms);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error_code, CommErrorCode::Closed);
}

TEST(CommTopicTest, SubscriptionIsMoveOnlyAndMoveTransfersRegistration) {
    static_assert(!std::is_copy_constructible_v<TopicSubscription<int>>);
    static_assert(!std::is_copy_assignable_v<TopicSubscription<int>>);
    static_assert(std::is_nothrow_move_constructible_v<TopicSubscription<int>>);
    static_assert(std::is_nothrow_move_assignable_v<TopicSubscription<int>>);

    Topic<int> topic;
    auto original = topic.subscribe(options(2));
    TopicSubscription<int> moved(std::move(original));
    EXPECT_TRUE(original.is_closed());
    EXPECT_EQ(topic.subscriber_count(), 1U);

    ASSERT_TRUE(topic.publish(11));
    int value = 0;
    EXPECT_TRUE(moved.try_receive(value));
    EXPECT_EQ(value, 11);

    TopicSubscription<int> assigned;
    assigned = std::move(moved);
    EXPECT_TRUE(moved.is_closed());
    EXPECT_EQ(topic.subscriber_count(), 1U);
    assigned.close();
    EXPECT_EQ(topic.subscriber_count(), 0U);
}

TEST(CommTopicTest, CallbackExceptionsDoNotChangePublishOutcome) {
    Topic<int> topic;
    auto subscription = topic.subscribe(options(1));
    std::atomic<int> callbacks{0};
    subscription.set_event_callback([&](const kairo::comm::CommEvent& event) {
        if (event.kind == CommEventKind::Dropped) {
            callbacks.fetch_add(1, std::memory_order_relaxed);
            throw std::runtime_error("diagnostic failure");
        }
    });

    EXPECT_TRUE(topic.publish(1));
    const auto result = topic.publish(2);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.rejected_subscribers, 1U);
    EXPECT_EQ(callbacks.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(subscription.stats().dropped_count, 1U);
}

TEST(CommTopicTest, SharedImmutablePayloadFansOutSameOwnership) {
    Topic<std::shared_ptr<const std::string>> topic;
    auto first = topic.subscribe(options(1));
    auto second = topic.subscribe(options(1));

    auto payload = std::make_shared<const std::string>("large-frame");
    ASSERT_TRUE(topic.publish(payload));

    std::shared_ptr<const std::string> first_value;
    std::shared_ptr<const std::string> second_value;
    ASSERT_TRUE(first.try_receive(first_value));
    ASSERT_TRUE(second.try_receive(second_value));
    EXPECT_EQ(first_value, payload);
    EXPECT_EQ(second_value, payload);
    EXPECT_EQ(*first_value, "large-frame");
}

TEST(CommTopicTest, ConcurrentPublishSubscribeAndUnsubscribeKeepsStableLifetime) {
    Topic<int> topic;
    auto permanent = topic.subscribe(options(8192));
    constexpr int kPublishers = 3;
    constexpr int kMessagesPerPublisher = 1000;
    std::atomic<bool> start{false};

    std::vector<std::thread> publishers;
    for (int publisher = 0; publisher < kPublishers; ++publisher) {
        publishers.emplace_back([&, publisher] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            for (int index = 0; index < kMessagesPerPublisher; ++index) {
                topic.publish(publisher * kMessagesPerPublisher + index);
            }
        });
    }

    std::thread churn([&] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (int index = 0; index < 500; ++index) {
            auto transient = topic.subscribe(options(2, DropPolicy::KeepLatest));
            int value = 0;
            transient.try_receive(value);
            if ((index % 2) == 0) transient.close();
        }
    });

    start.store(true, std::memory_order_release);
    for (auto& publisher : publishers) publisher.join();
    churn.join();

    int value = 0;
    size_t received = 0;
    while (permanent.try_receive(value)) ++received;
    EXPECT_EQ(received, static_cast<size_t>(kPublishers * kMessagesPerPublisher));
    EXPECT_EQ(permanent.stats().dropped_count, 0U);
    EXPECT_EQ(topic.subscriber_count(), 1U);
}

} // namespace
