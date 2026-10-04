#include <kairo/comm.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <concepts>
#include <string>
#include <type_traits>

namespace {

using namespace kairo::comm;

template <class Primitive>
void expect_lock_free_contract(const Primitive& primitive) {
    static_assert(requires(const Primitive& candidate) {
        { candidate.is_synchronization_lock_free() } -> std::convertible_to<bool>;
    });
    EXPECT_TRUE(primitive.is_synchronization_lock_free());
}

TEST(CommTypesTest, CommResultDefaultsToSuccess) {
    CommResult result;

    EXPECT_TRUE(result);
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.error_code, CommErrorCode::Ok);
    EXPECT_TRUE(result.message.empty());
}

TEST(CommTypesTest, CommResultFactoriesSetBoolAndDiagnostics) {
    const CommResult success = CommResult::success("closed cleanly");
    EXPECT_TRUE(success);
    EXPECT_EQ(success.error_code, CommErrorCode::Ok);
    EXPECT_EQ(success.message, "closed cleanly");

    const CommResult failure = CommResult::failure(CommErrorCode::Timeout, "receive timed out");
    EXPECT_FALSE(failure);
    EXPECT_FALSE(failure.ok);
    EXPECT_EQ(failure.error_code, CommErrorCode::Timeout);
    EXPECT_EQ(failure.message, "receive timed out");
}

TEST(CommTypesTest, ErrorCodesStringifyToStableNames) {
    EXPECT_STREQ(comm_error_code_to_string(CommErrorCode::Ok), "Ok");
    EXPECT_STREQ(comm_error_code_to_string(CommErrorCode::Closed), "Closed");
    EXPECT_STREQ(comm_error_code_to_string(CommErrorCode::Full), "Full");
    EXPECT_STREQ(comm_error_code_to_string(CommErrorCode::Empty), "Empty");
    EXPECT_STREQ(comm_error_code_to_string(CommErrorCode::Timeout), "Timeout");
    EXPECT_STREQ(comm_error_code_to_string(CommErrorCode::Stale), "Stale");
    EXPECT_STREQ(comm_error_code_to_string(CommErrorCode::MissedPhase), "MissedPhase");
    EXPECT_STREQ(comm_error_code_to_string(CommErrorCode::InvalidArgument), "InvalidArgument");
    EXPECT_STREQ(comm_error_code_to_string(CommErrorCode::NotReady), "NotReady");
    EXPECT_STREQ(comm_error_code_to_string(CommErrorCode::Unknown), "Unknown");
}

TEST(CommTypesTest, StringifyUnknownEnumValues) {
    EXPECT_STREQ(comm_error_code_to_string(static_cast<CommErrorCode>(999)), "Unknown");
    EXPECT_STREQ(drop_policy_to_string(static_cast<DropPolicy>(999)), "Unknown");
    EXPECT_STREQ(comm_event_kind_to_string(static_cast<CommEventKind>(999)), "Unknown");
}

TEST(CommTypesTest, DropPolicyAndEventKindStringifyToStableNames) {
    EXPECT_STREQ(drop_policy_to_string(DropPolicy::RejectNewest), "RejectNewest");
    EXPECT_STREQ(drop_policy_to_string(DropPolicy::DropOldest), "DropOldest");
    EXPECT_STREQ(drop_policy_to_string(DropPolicy::KeepLatest), "KeepLatest");

    EXPECT_STREQ(comm_event_kind_to_string(CommEventKind::Dropped), "Dropped");
    EXPECT_STREQ(comm_event_kind_to_string(CommEventKind::Overwritten), "Overwritten");
    EXPECT_STREQ(comm_event_kind_to_string(CommEventKind::ClosedSend), "ClosedSend");
    EXPECT_STREQ(comm_event_kind_to_string(CommEventKind::Timeout), "Timeout");
    EXPECT_STREQ(comm_event_kind_to_string(CommEventKind::StaleRead), "StaleRead");
    EXPECT_STREQ(comm_event_kind_to_string(CommEventKind::MissedPhase), "MissedPhase");
    EXPECT_STREQ(comm_event_kind_to_string(CommEventKind::ProducerLag), "ProducerLag");
    EXPECT_STREQ(comm_event_kind_to_string(CommEventKind::ConsumerLag), "ConsumerLag");
    EXPECT_STREQ(comm_event_kind_to_string(CommEventKind::LatencyHigh), "LatencyHigh");
    EXPECT_STREQ(comm_event_kind_to_string(CommEventKind::HandlerException), "HandlerException");
}

TEST(CommTypesTest, StatsDefaultToZeroLatencyAndCounts) {
    CommStats stats;

    EXPECT_EQ(stats.sent_count, 0U);
    EXPECT_EQ(stats.received_count, 0U);
    EXPECT_EQ(stats.dropped_count, 0U);
    EXPECT_EQ(stats.overwritten_count, 0U);
    EXPECT_EQ(stats.stale_read_count, 0U);
    EXPECT_EQ(stats.closed_send_count, 0U);
    EXPECT_EQ(stats.timeout_count, 0U);
    EXPECT_EQ(stats.handler_exception_count, 0U);
    EXPECT_EQ(stats.missed_phase_count, 0U);
    EXPECT_EQ(stats.current_depth, 0U);
    EXPECT_EQ(stats.peak_depth, 0U);
    EXPECT_EQ(stats.capacity, 0U);
    EXPECT_EQ(stats.producer_lag, 0U);
    EXPECT_EQ(stats.consumer_lag, 0U);
    EXPECT_EQ(stats.max_latency, std::chrono::nanoseconds{0});
    EXPECT_EQ(stats.avg_latency, std::chrono::nanoseconds{0});
    EXPECT_EQ(stats.p50_latency, std::chrono::nanoseconds{0});
    EXPECT_EQ(stats.p99_latency, std::chrono::nanoseconds{0});
    EXPECT_EQ(stats.latency_histogram[0], 0U);
}

TEST(CommTypesTest, LatencyHistogramComputesBoundedQuantiles) {
    CommStats stats;
    std::chrono::nanoseconds total_latency{0};
    for (const auto latency : {std::chrono::nanoseconds{10}, std::chrono::nanoseconds{20},
                               std::chrono::nanoseconds{40}, std::chrono::nanoseconds{80},
                               std::chrono::nanoseconds{160}}) {
        ++stats.received_count;
        update_latency_stats(stats, total_latency, latency);
    }

    EXPECT_EQ(stats.avg_latency, std::chrono::nanoseconds{62});
    EXPECT_GE(stats.p50_latency, std::chrono::nanoseconds{40});
    EXPECT_GE(stats.p99_latency, std::chrono::nanoseconds{160});
    EXPECT_LE(stats.p99_latency, std::chrono::nanoseconds{256});
}

TEST(CommTypesTest, EventDefaultsAreUsableAndCallbackIsInvocable) {
    CommEvent event;
    event.component_name = "channel";
    event.message = "drop";
    event.sequence = 42;

    bool called = false;
    CommEventCallback callback = [&](const CommEvent& observed) {
        called = true;
        EXPECT_EQ(observed.kind, CommEventKind::Dropped);
        EXPECT_EQ(observed.component_name, "channel");
        EXPECT_EQ(observed.message, "drop");
        EXPECT_EQ(observed.sequence, 42U);
    };

    callback(event);
    EXPECT_TRUE(called);
}

TEST(CommTypesTest, AggregatedHeaderExposesNamespaceTypes) {
    static_assert(std::is_same_v<decltype(CommStats{}.sent_count), uint64_t>);
    static_assert(std::is_same_v<CommEventCallback, std::function<void(const CommEvent&)>>);
}

TEST(CommTypesTest, RealtimePrimitivesReportLockFreeSynchronization) {
    ChannelOptions channel_options;
    channel_options.capacity = 8;
    MpscChannel<int> channel(channel_options);

    RealtimeChannelOptions realtime_options;
    realtime_options.capacity = 8;
    RealtimeChannel<int> realtime_channel(realtime_options);

    LatestMailbox<int> mailbox;
    DoubleBuffer<int> snapshots(0);
    PhaseGate gate;
    Sequencer sequencer;
    LatestMailbox<int> let_mailbox;
    DoubleBuffer<int> let_snapshots(0);
    ASSERT_TRUE(let_mailbox.bind_to_phase_gate(gate));
    ASSERT_TRUE(let_snapshots.bind_to_phase_gate(gate));

    expect_lock_free_contract(channel);
    expect_lock_free_contract(realtime_channel);
    expect_lock_free_contract(mailbox);
    expect_lock_free_contract(snapshots);
    expect_lock_free_contract(gate);
    expect_lock_free_contract(sequencer);
    expect_lock_free_contract(let_mailbox);
    expect_lock_free_contract(let_snapshots);
}

} // namespace
