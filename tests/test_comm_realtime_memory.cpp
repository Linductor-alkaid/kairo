#include <kairo/comm.hpp>
#include <kairo/realtime_thread_executor.hpp>

#include <atomic>
#include <chrono>
#include <new>
#include <gtest/gtest.h>

#include <string>
#include <thread>

namespace {

TEST(CommRealtimeMemoryTest, GuardIsExplicitAndReportsItsBuildMode) {
    kairo::comm::RealtimeAllocationGuard::reset_current_thread_stats();
    {
        kairo::comm::RealtimeAllocationGuard guard("control_loop", "drain");
        const auto stats = kairo::comm::RealtimeAllocationGuard::current_thread_stats();
        if (kairo::comm::RealtimeAllocationGuard::is_enabled()) {
            EXPECT_EQ(stats.component, "control_loop");
            EXPECT_EQ(stats.phase, "drain");
        } else {
            EXPECT_EQ(stats.allocation_count, 0U);
        }
    }
}

TEST(CommRealtimeMemoryTest, GuardedAllocationIsRecordedWhenEnabled) {
    kairo::comm::RealtimeAllocationGuard::reset_current_thread_stats();
    {
        kairo::comm::RealtimeAllocationGuard guard("let_mailbox", "publish");
        std::string allocation(128, 'x');
        (void)allocation;
    }
    const auto stats = kairo::comm::RealtimeAllocationGuard::current_thread_stats();
    if (kairo::comm::RealtimeAllocationGuard::is_enabled()) {
        EXPECT_GE(stats.allocation_count, 1U);
        EXPECT_GE(stats.allocated_bytes, 128U);
        EXPECT_EQ(stats.component, "let_mailbox");
        EXPECT_EQ(stats.phase, "publish");
    } else {
        EXPECT_EQ(stats.allocation_count, 0U);
    }
}

TEST(CommRealtimeMemoryTest, NestedGuardPreservesOuterContextAndCounts) {
    kairo::comm::RealtimeAllocationGuard::reset_current_thread_stats();
    {
        kairo::comm::RealtimeAllocationGuard outer("control_loop", "cycle");
        std::string outer_allocation(128, 'o');
        {
            kairo::comm::RealtimeAllocationGuard inner("mailbox", "publish");
            std::string inner_allocation(128, 'i');
            (void)inner_allocation;
        }
        (void)outer_allocation;
    }
    const auto stats = kairo::comm::RealtimeAllocationGuard::current_thread_stats();
    if (kairo::comm::RealtimeAllocationGuard::is_enabled()) {
        EXPECT_EQ(stats.component, "control_loop");
        EXPECT_EQ(stats.phase, "cycle");
        EXPECT_GE(stats.allocation_count, 2U);
    } else {
        EXPECT_EQ(stats.allocation_count, 0U);
    }
}

TEST(CommRealtimeMemoryTest, AbortPolicyTerminatesOnAllocationWhenEnabled) {
    if (!kairo::comm::RealtimeAllocationGuard::is_enabled()) {
        GTEST_SKIP() << "allocation guard is disabled in this build";
    }
#ifdef KAIRO_ENABLE_REALTIME_ALLOCATION_GUARD
    EXPECT_DEATH(
        {
            kairo::comm::RealtimeAllocationGuard guard(
                "control_loop", "cycle",
                kairo::comm::RealtimeAllocationViolationPolicy::Abort);
            void* allocation = ::operator new(sizeof(int));
            ::operator delete(allocation);
        },
        "");
#endif
}

TEST(CommRealtimeMemoryTest, CommunicationTryPathsAllocateNothingWhenEnabled) {
    if (!kairo::comm::RealtimeAllocationGuard::is_enabled()) {
        GTEST_SKIP() << "allocation guard is disabled in this build";
    }

    kairo::comm::ChannelOptions channel_options;
    channel_options.capacity = 8;
    kairo::comm::MpscChannel<int> channel(channel_options);

    kairo::comm::RealtimeChannelOptions realtime_options;
    realtime_options.capacity = 8;
    realtime_options.max_items_per_cycle = 2;
    kairo::comm::RealtimeChannel<int> realtime(realtime_options);

    kairo::comm::LatestMailbox<int> mailbox;
    kairo::comm::DoubleBuffer<int> snapshots(0);
    kairo::comm::PhaseGate gate;
    kairo::comm::DoubleBuffer<int> let_snapshots(0);
    ASSERT_TRUE(let_snapshots.bind_to_phase_gate(gate));

    kairo::comm::RealtimeAllocationGuard::reset_current_thread_stats();
    bool channel_sent = false;
    bool channel_received = false;
    bool realtime_sent = false;
    size_t realtime_drained = 0;
    bool mailbox_published = false;
    bool mailbox_loaded = false;
    bool snapshot_published = false;
    bool let_published = false;
    bool let_advanced = false;
    bool let_loaded = false;
    int value = 0;
    kairo::comm::Snapshot<int> snapshot;
    kairo::comm::Snapshot<int> let_snapshot;
    {
        kairo::comm::RealtimeAllocationGuard guard("comm", "try_paths");
        channel_sent = channel.try_send(1);
        channel_received = channel.try_receive(value);

        realtime_sent = realtime.try_send(2);
        realtime_drained = realtime.drain_for_cycle([&](int item) { value = item; });

        mailbox_published = mailbox.try_publish(3);
        mailbox_loaded = mailbox.try_load(value);

        snapshot_published = snapshots.try_publish(4);
        snapshot = snapshots.load();

        let_published = static_cast<bool>(let_snapshots.publish_for_current_phase(5));
        let_advanced = static_cast<bool>(gate.advance());
        let_loaded = static_cast<bool>(let_snapshots.load_for_current_phase(let_snapshot));
    }

    const auto stats =
        kairo::comm::RealtimeAllocationGuard::current_thread_stats();
    EXPECT_TRUE(channel_sent);
    EXPECT_TRUE(channel_received);
    EXPECT_TRUE(realtime_sent);
    EXPECT_EQ(realtime_drained, 1U);
    EXPECT_TRUE(mailbox_published);
    EXPECT_TRUE(mailbox_loaded);
    EXPECT_TRUE(snapshot_published);
    EXPECT_EQ(snapshot.value, 4);
    EXPECT_TRUE(let_published);
    EXPECT_TRUE(let_advanced);
    EXPECT_TRUE(let_loaded);
    EXPECT_EQ(let_snapshot.value, 5);
    EXPECT_EQ(stats.component, "comm");
    EXPECT_EQ(stats.phase, "try_paths");
    EXPECT_EQ(stats.allocation_count, 0U);
    EXPECT_EQ(stats.allocated_bytes, 0U);
}

TEST(CommRealtimeMemoryTest, FailureTryPathsWithoutCallbacksAllocateNothingWhenEnabled) {
    if (!kairo::comm::RealtimeAllocationGuard::is_enabled()) {
        GTEST_SKIP() << "allocation guard is disabled in this build";
    }

    kairo::comm::ChannelOptions channel_options;
    channel_options.capacity = 1;
    kairo::comm::MpscChannel<int> full_channel(channel_options);
    kairo::comm::MpscChannel<int> empty_channel(channel_options);
    kairo::comm::MpscChannel<int> closed_channel(channel_options);
    ASSERT_TRUE(full_channel.try_send(1));
    closed_channel.close();

    kairo::comm::RealtimeChannelOptions realtime_options;
    realtime_options.capacity = 1;
    realtime_options.max_items_per_cycle = 1;
    kairo::comm::RealtimeChannel<int> full_realtime(realtime_options);
    kairo::comm::RealtimeChannel<int> empty_realtime(realtime_options);
    kairo::comm::RealtimeChannel<int> closed_realtime(realtime_options);
    ASSERT_TRUE(full_realtime.try_send(1));
    closed_realtime.close();

    kairo::comm::LatestMailbox<int> empty_mailbox;
    kairo::comm::LatestMailbox<int> stale_mailbox;
    ASSERT_TRUE(stale_mailbox.try_publish(1));

    kairo::comm::DoubleBuffer<int> stale_snapshots(1);
    const uint64_t snapshot_sequence = stale_snapshots.sequence();

    kairo::comm::PhaseGate let_gate;
    kairo::comm::LatestMailbox<int> let_mailbox;
    kairo::comm::DoubleBuffer<int> let_snapshots(0);
    ASSERT_TRUE(let_mailbox.bind_to_phase_gate(let_gate));
    ASSERT_TRUE(let_snapshots.bind_to_phase_gate(let_gate));
    ASSERT_TRUE(let_mailbox.publish_for_current_phase(1));
    ASSERT_TRUE(let_snapshots.publish_for_current_phase(1));
    auto active_lease = let_gate.try_begin_let_read();
    ASSERT_TRUE(active_lease.has_value());

    kairo::comm::PhaseGate missed_let_gate;
    kairo::comm::DoubleBuffer<int> missed_let_snapshots(0);
    ASSERT_TRUE(missed_let_snapshots.bind_to_phase_gate(missed_let_gate));
    ASSERT_TRUE(missed_let_gate.advance());

    kairo::comm::PhaseGate closed_let_gate;
    kairo::comm::DoubleBuffer<int> closed_let_snapshots(0);
    ASSERT_TRUE(closed_let_snapshots.bind_to_phase_gate(closed_let_gate));
    ASSERT_TRUE(closed_let_gate.close());

    kairo::comm::RealtimeAllocationGuard::reset_current_thread_stats();
    int value = 0;
    uint64_t mailbox_sequence = stale_mailbox.sequence();
    kairo::comm::Snapshot<int> snapshot;
    bool channel_full = false;
    bool channel_empty = false;
    bool channel_closed = false;
    bool realtime_full = false;
    size_t realtime_empty = 0;
    bool realtime_closed = false;
    bool mailbox_empty = false;
    bool mailbox_stale = false;
    bool snapshot_stale = false;
    bool let_mailbox_duplicate = false;
    bool let_mailbox_not_visible = false;
    bool let_snapshot_duplicate = false;
    bool let_snapshot_not_visible = false;
    bool let_advance_blocked = false;
    kairo::comm::CommErrorCode let_advance_missed =
        kairo::comm::CommErrorCode::Ok;
    kairo::comm::CommErrorCode let_advance_closed =
        kairo::comm::CommErrorCode::Ok;
    {
        kairo::comm::RealtimeAllocationGuard guard("comm", "failure_try_paths");
        channel_full = full_channel.try_send(2);
        channel_empty = empty_channel.try_receive(value);
        channel_closed = closed_channel.try_send(2);

        realtime_full = full_realtime.try_send(2);
        realtime_empty = empty_realtime.drain_for_cycle([](int) {});
        realtime_closed = closed_realtime.try_send(2);

        mailbox_empty = empty_mailbox.try_load(value);
        mailbox_stale = stale_mailbox.try_load_newer_than(
            mailbox_sequence, value, mailbox_sequence);
        snapshot_stale = stale_snapshots.load_newer_than(
            snapshot_sequence, snapshot);

        let_mailbox_duplicate = static_cast<bool>(
            let_mailbox.publish_for_current_phase(2));
        let_mailbox_not_visible = static_cast<bool>(
            let_mailbox.load_for_current_phase(value));
        let_snapshot_duplicate = static_cast<bool>(
            let_snapshots.publish_for_current_phase(2));
        let_snapshot_not_visible = static_cast<bool>(
            let_snapshots.load_for_current_phase(snapshot));
        let_advance_blocked = static_cast<bool>(let_gate.advance());
        let_advance_missed = missed_let_gate.advance_to(1).error_code;
        let_advance_closed = closed_let_gate.advance().error_code;
    }

    const auto stats =
        kairo::comm::RealtimeAllocationGuard::current_thread_stats();
    EXPECT_FALSE(channel_full);
    EXPECT_FALSE(channel_empty);
    EXPECT_FALSE(channel_closed);
    EXPECT_FALSE(realtime_full);
    EXPECT_EQ(realtime_empty, 0U);
    EXPECT_FALSE(realtime_closed);
    EXPECT_FALSE(mailbox_empty);
    EXPECT_FALSE(mailbox_stale);
    EXPECT_FALSE(snapshot_stale);
    EXPECT_FALSE(let_mailbox_duplicate);
    EXPECT_FALSE(let_mailbox_not_visible);
    EXPECT_FALSE(let_snapshot_duplicate);
    EXPECT_FALSE(let_snapshot_not_visible);
    EXPECT_FALSE(let_advance_blocked);
    EXPECT_EQ(let_advance_missed, kairo::comm::CommErrorCode::MissedPhase);
    EXPECT_EQ(let_advance_closed, kairo::comm::CommErrorCode::Closed);
    EXPECT_EQ(stats.component, "comm");
    EXPECT_EQ(stats.phase, "failure_try_paths");
    EXPECT_EQ(stats.allocation_count, 0U);
    EXPECT_EQ(stats.allocated_bytes, 0U);
}

TEST(CommRealtimeMemoryTest, RealtimeThreadCanExplicitlyAttachGuardToCallback) {
    std::atomic<bool> callback_seen{false};
    std::atomic<bool> context_matches{false};
    kairo::RealtimeThreadConfig config;
    config.thread_name = "allocation_guard_rt";
    config.cycle_period_ns = 1'000'000;
    config.enable_allocation_guard = true;
    config.cycle_callback = [&] {
        const auto stats = kairo::comm::RealtimeAllocationGuard::current_thread_stats();
        if (kairo::comm::RealtimeAllocationGuard::is_enabled()) {
            context_matches.store(stats.component == "allocation_guard_rt" &&
                                      stats.phase == "cycle_callback",
                                  std::memory_order_release);
        }
        callback_seen.store(true, std::memory_order_release);
    };

    kairo::RealtimeThreadExecutor realtime("allocation_guard_rt", config);
    ASSERT_TRUE(realtime.start());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (!callback_seen.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    realtime.stop();
    ASSERT_TRUE(callback_seen.load(std::memory_order_acquire));
    if (kairo::comm::RealtimeAllocationGuard::is_enabled()) {
        EXPECT_TRUE(context_matches.load(std::memory_order_acquire));
    }
}

} // namespace
