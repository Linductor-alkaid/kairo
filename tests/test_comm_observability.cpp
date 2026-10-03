#include <kairo/comm.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;

namespace {

kairo::comm::ChannelOptions channel_options(size_t capacity) {
    kairo::comm::ChannelOptions options;
    options.capacity = capacity;
    return options;
}

kairo::comm::RealtimeChannelOptions realtime_options(size_t capacity) {
    kairo::comm::RealtimeChannelOptions options;
    options.capacity = capacity;
    options.max_items_per_cycle = 8;
    return options;
}

TEST(CommObservabilityTest, CountsDropOverwriteStaleMissedAndTimeout) {
    kairo::comm::MpscChannel<int> channel(channel_options(1));
    EXPECT_TRUE(channel.try_send(1));
    EXPECT_FALSE(channel.try_send(2));
    int value = 0;
    EXPECT_TRUE(channel.try_receive(value));
    EXPECT_FALSE(channel.receive_for(value, 1ms));
    EXPECT_EQ(channel.stats().dropped_count, 1U);
    EXPECT_EQ(channel.stats().timeout_count, 1U);

    kairo::comm::LatestMailbox<int> mailbox("mailbox");
    mailbox.publish(1);
    mailbox.publish(2);
    uint64_t sequence = mailbox.sequence();
    EXPECT_FALSE(mailbox.try_load_newer_than(sequence, value, sequence));
    EXPECT_EQ(mailbox.stats().overwritten_count, 1U);
    EXPECT_EQ(mailbox.stats().stale_read_count, 1U);

    kairo::comm::PhaseGate gate("phase");
    EXPECT_TRUE(gate.advance_to(2));
    EXPECT_FALSE(gate.advance_to(1));
    EXPECT_EQ(gate.stats().missed_phase_count, 1U);
}

TEST(CommObservabilityTest, CallbackExceptionsAreIsolatedFromDataPath) {
    auto throwing_callback = [](const kairo::comm::CommEvent&) {
        throw std::runtime_error("diagnostic sink failed");
    };

    kairo::comm::MpscChannel<int> channel(channel_options(1));
    channel.set_event_callback(throwing_callback);
    EXPECT_TRUE(channel.try_send(1));
    EXPECT_NO_THROW({
        EXPECT_FALSE(channel.try_send(2));
    });
    EXPECT_EQ(channel.stats().dropped_count, 1U);

    kairo::comm::LatestMailbox<int> mailbox("mailbox");
    mailbox.set_event_callback(throwing_callback);
    mailbox.publish(1);
    EXPECT_NO_THROW(mailbox.publish(2));
    EXPECT_EQ(mailbox.stats().overwritten_count, 1U);

    kairo::comm::PhaseGate gate("phase");
    gate.set_event_callback(throwing_callback);
    EXPECT_TRUE(gate.advance_to(2));
    EXPECT_NO_THROW({
        const auto result = gate.advance_to(1);
        EXPECT_FALSE(result);
    });
    EXPECT_EQ(gate.stats().missed_phase_count, 1U);

    kairo::comm::RealtimeChannel<int> realtime(realtime_options(1));
    realtime.set_event_callback(throwing_callback);
    EXPECT_TRUE(realtime.try_send(1));
    EXPECT_NO_THROW({
        EXPECT_FALSE(realtime.try_send(2));
    });
    EXPECT_EQ(realtime.stats().dropped_count, 1U);
}

TEST(CommObservabilityTest, LatencyAndLagStatsAreObservable) {
    kairo::comm::MpscChannel<int> channel(channel_options(4));
    EXPECT_TRUE(channel.try_send(1));
    std::this_thread::sleep_for(1ms);
    int value = 0;
    EXPECT_TRUE(channel.try_receive(value));
    auto channel_stats = channel.stats();
    EXPECT_EQ(channel_stats.producer_lag, 0U);
    EXPECT_EQ(channel_stats.consumer_lag, 0U);
    EXPECT_GT(channel_stats.max_latency.count(), 0);
    EXPECT_GT(channel_stats.avg_latency.count(), 0);

    kairo::comm::LatestMailbox<int> mailbox("mailbox");
    mailbox.publish(1);
    EXPECT_TRUE(mailbox.try_load(value));
    EXPECT_EQ(mailbox.stats().consumer_lag, 0U);
    mailbox.publish(2);
    mailbox.publish(3);
    mailbox.publish(4);
    std::this_thread::sleep_for(1ms);
    EXPECT_TRUE(mailbox.try_load(value));
    auto mailbox_stats = mailbox.stats();
    EXPECT_EQ(mailbox_stats.producer_lag, 4U);
    EXPECT_EQ(mailbox_stats.consumer_lag, 2U);
    EXPECT_GT(mailbox_stats.max_latency.count(), 0);

    kairo::comm::RealtimeChannel<int> realtime(realtime_options(4));
    EXPECT_TRUE(realtime.try_send(5));
    std::this_thread::sleep_for(1ms);
    EXPECT_EQ(realtime.drain_for_cycle([&](int item) { value = item; }), 1U);
    auto realtime_stats = realtime.stats();
    EXPECT_EQ(realtime_stats.current_depth, 0U);
    EXPECT_GT(realtime_stats.max_latency.count(), 0);

    kairo::comm::DoubleBuffer<int> buffer(0, "state");
    auto snapshot = buffer.load();
    EXPECT_EQ(snapshot.value, 0);
    EXPECT_EQ(buffer.stats().consumer_lag, 0U);
    buffer.publish(5);
    buffer.publish(6);
    buffer.publish(7);
    std::this_thread::sleep_for(1ms);
    snapshot = buffer.load();
    EXPECT_EQ(snapshot.value, 7);
    auto buffer_stats = buffer.stats();
    EXPECT_EQ(buffer_stats.producer_lag, 3U);
    EXPECT_EQ(buffer_stats.consumer_lag, 2U);
    EXPECT_GT(buffer_stats.max_latency.count(), 0);
}

} // namespace
