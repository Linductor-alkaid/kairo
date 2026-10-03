#include <kairo/comm.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

using kairo::comm::CommErrorCode;
using kairo::comm::CommEventKind;
using kairo::comm::PhaseGate;
using kairo::comm::Sequencer;

TEST(CommPhaseGateTest, WaitBeforeAdvanceWakesWhenPhaseReached) {
    PhaseGate gate("startup");
    std::atomic<bool> waiter_ready{false};
    std::atomic<bool> waiter_done{false};

    std::thread waiter([&] {
        waiter_ready.store(true, std::memory_order_release);
        const auto result = gate.wait_for(2, 1s);
        EXPECT_TRUE(result);
        waiter_done.store(true, std::memory_order_release);
    });

    while (!waiter_ready.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    EXPECT_TRUE(gate.advance());
    EXPECT_FALSE(waiter_done.load(std::memory_order_acquire));
    EXPECT_TRUE(gate.advance());

    waiter.join();
    EXPECT_TRUE(waiter_done.load(std::memory_order_acquire));
    EXPECT_EQ(gate.current_phase(), 2U);
    EXPECT_EQ(gate.stats().sent_count, 2U);
    EXPECT_EQ(gate.stats().received_count, 1U);
}

TEST(CommPhaseGateTest, WaitAfterAdvanceReturnsImmediately) {
    PhaseGate gate;
    EXPECT_TRUE(gate.advance_to(3));

    const auto result = gate.wait_for(2, 10ms);
    EXPECT_TRUE(result);
    EXPECT_TRUE(gate.has_reached(3));
    EXPECT_EQ(gate.stats().received_count, 1U);
}

TEST(CommPhaseGateTest, CloseWakesWaiter) {
    PhaseGate gate;
    std::atomic<bool> waiter_done{false};

    std::thread waiter([&] {
        const auto result = gate.wait_for(1, 1s);
        EXPECT_FALSE(result);
        EXPECT_EQ(result.error_code, CommErrorCode::Closed);
        waiter_done.store(true, std::memory_order_release);
    });

    std::this_thread::sleep_for(20ms);
    EXPECT_FALSE(waiter_done.load(std::memory_order_acquire));
    EXPECT_TRUE(gate.close());
    waiter.join();
    EXPECT_TRUE(waiter_done.load(std::memory_order_acquire));
    EXPECT_TRUE(gate.is_closed());
}

TEST(CommPhaseGateTest, AdvanceToRejectsRollbackAndCountsMissedPhase) {
    PhaseGate gate("phase");
    int missed_events = 0;
    gate.set_event_callback([&](const kairo::comm::CommEvent& event) noexcept {
        if (event.kind == CommEventKind::MissedPhase) {
            ++missed_events;
            EXPECT_EQ(event.component_name, "phase");
        }
    });

    EXPECT_TRUE(gate.advance_to(4));

    const auto result = gate.advance_to(3);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error_code, CommErrorCode::MissedPhase);
    EXPECT_EQ(gate.current_phase(), 4U);
    EXPECT_EQ(gate.stats().missed_phase_count, 1U);
    EXPECT_EQ(missed_events, 1);
}

TEST(CommPhaseGateTest, ExactWaitReportsMissedPhase) {
    PhaseGate gate;
    EXPECT_TRUE(gate.advance_to(5));

    const auto result = gate.wait_for_exact(3, 10ms);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error_code, CommErrorCode::MissedPhase);
    EXPECT_EQ(gate.stats().missed_phase_count, 1U);
}

TEST(CommPhaseGateTest, WaitRejectsReservedHighBitImmediately) {
    constexpr uint64_t kReservedHighBit = uint64_t{1} << 63U;
    PhaseGate gate;

    const auto at_least = gate.wait_for(kReservedHighBit, 0ns);
    const auto exact = gate.wait_for_exact(kReservedHighBit, 0ns);

    EXPECT_FALSE(at_least);
    EXPECT_EQ(at_least.error_code, CommErrorCode::InvalidArgument);
    EXPECT_FALSE(exact);
    EXPECT_EQ(exact.error_code, CommErrorCode::InvalidArgument);
    EXPECT_EQ(gate.stats().timeout_count, 0U);
}

TEST(CommPhaseGateTest, WaitTimeoutIsObservable) {
    PhaseGate gate;

    const auto result = gate.wait_for(1, 5ms);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error_code, CommErrorCode::Timeout);
    EXPECT_EQ(gate.stats().timeout_count, 1U);
}

TEST(CommPhaseGateTest, ConcurrentWaitersAllWake) {
    PhaseGate gate;
    constexpr int kWaiterCount = 8;
    std::atomic<int> completed{0};
    std::vector<std::thread> waiters;
    waiters.reserve(kWaiterCount);

    for (int i = 0; i < kWaiterCount; ++i) {
        waiters.emplace_back([&] {
            EXPECT_TRUE(gate.wait_for(1, 1s));
            completed.fetch_add(1, std::memory_order_acq_rel);
        });
    }

    std::this_thread::sleep_for(20ms);
    EXPECT_TRUE(gate.advance());

    for (auto& waiter : waiters) {
        waiter.join();
    }

    EXPECT_EQ(completed.load(std::memory_order_acquire), kWaiterCount);
    EXPECT_EQ(gate.stats().received_count, static_cast<uint64_t>(kWaiterCount));
}

TEST(CommPhaseGateTest, DirectWriterLeaseBlocksAdvanceWithoutBinding) {
    PhaseGate gate;
    auto lease = gate.try_begin_let_write();
    ASSERT_TRUE(lease.has_value());

    EXPECT_EQ(gate.advance().error_code, CommErrorCode::NotReady);
    lease->release();
    EXPECT_TRUE(gate.advance());
}

TEST(CommPhaseGateTest, WriterLeaseBlocksAdvanceUntilReleased) {
    PhaseGate gate;
    kairo::comm::DoubleBuffer<int> buffer(0);
    ASSERT_TRUE(buffer.bind_to_phase_gate(gate));
    auto lease = gate.try_begin_let_write();
    ASSERT_TRUE(lease.has_value());
    EXPECT_EQ(gate.advance().error_code, CommErrorCode::NotReady);
    lease->release();
    EXPECT_TRUE(gate.advance());
}

TEST(CommPhaseGateTest, LeaseOutlivesBindingAndStillBlocksAdvance) {
    PhaseGate gate;
    std::optional<PhaseGate::LetWriteLease> lease;
    {
        kairo::comm::DoubleBuffer<int> buffer(0);
        ASSERT_TRUE(buffer.bind_to_phase_gate(gate));
        lease = gate.try_begin_let_read();
        ASSERT_TRUE(lease.has_value());
    }

    EXPECT_EQ(gate.advance().error_code, CommErrorCode::NotReady);
    lease->release();
    EXPECT_TRUE(gate.advance());
}

TEST(CommPhaseGateTest, ReaderLeaseBlocksAdvanceUntilReleased) {
    PhaseGate gate;
    kairo::comm::DoubleBuffer<int> buffer(0);
    ASSERT_TRUE(buffer.bind_to_phase_gate(gate));
    auto lease = gate.try_begin_let_read();
    ASSERT_TRUE(lease.has_value());
    EXPECT_EQ(gate.advance().error_code, CommErrorCode::NotReady);
    lease->release();
    EXPECT_TRUE(gate.advance());
}

TEST(CommPhaseGateTest, AdvanceAndAdvanceToShareLetTransitionPath) {
    PhaseGate gate;
    kairo::comm::DoubleBuffer<int> buffer(0);
    ASSERT_TRUE(buffer.bind_to_phase_gate(gate));
    EXPECT_TRUE(gate.advance());
    EXPECT_EQ(gate.current_phase(), 1U);
    EXPECT_TRUE(gate.advance_to(4));
    EXPECT_EQ(gate.current_phase(), 4U);
    EXPECT_EQ(gate.advance_to(4).error_code, CommErrorCode::MissedPhase);
}

TEST(CommPhaseGateTest, LetBindingRejectsSnapshotSentinelPhase) {
    constexpr uint64_t kLetSentinel = (uint64_t{1} << 63U) - 1U;
    PhaseGate gate;
    kairo::comm::LatestMailbox<int> mailbox;
    kairo::comm::DoubleBuffer<int> buffer(0);
    ASSERT_TRUE(mailbox.bind_to_phase_gate(gate));
    ASSERT_TRUE(buffer.bind_to_phase_gate(gate));

    const auto result = gate.advance_to(kLetSentinel);

    EXPECT_FALSE(result);
    EXPECT_EQ(result.error_code, CommErrorCode::MissedPhase);
    EXPECT_EQ(gate.current_phase(), 0U);
}

TEST(CommPhaseGateTest, ConcurrentLetAdvancesAllowOnlyOneSuccess) {
    PhaseGate gate;
    kairo::comm::DoubleBuffer<int> buffer(0);
    ASSERT_TRUE(buffer.bind_to_phase_gate(gate));
    constexpr int kThreads = 8;
    std::atomic<int> successes{0};
    std::vector<std::thread> workers;
    for (int i = 0; i < kThreads; ++i) {
        workers.emplace_back([&] {
            const auto result = gate.advance_to(1);
            if (result) successes.fetch_add(1, std::memory_order_relaxed);
            else EXPECT_TRUE(result.error_code == CommErrorCode::MissedPhase ||
                             result.error_code == CommErrorCode::NotReady);
        });
    }
    for (auto& worker : workers) worker.join();
    EXPECT_EQ(successes.load(), 1);
    EXPECT_EQ(gate.current_phase(), 1U);
}

TEST(CommPhaseGateTest, ConcurrentLeasePressurePreservesMonotonicPhases) {
    PhaseGate gate;
    kairo::comm::DoubleBuffer<int> buffer(0);
    ASSERT_TRUE(buffer.bind_to_phase_gate(gate));
    std::atomic<bool> stop{false};
    std::vector<std::thread> workers;
    for (int i = 0; i < 2; ++i) {
        workers.emplace_back([&] {
            while (!stop.load(std::memory_order_acquire)) {
                if (auto lease = gate.try_begin_let_write()) {
                    std::this_thread::yield();
                }
            }
        });
        workers.emplace_back([&] {
            while (!stop.load(std::memory_order_acquire)) {
                if (auto lease = gate.try_begin_let_read()) {
                    std::this_thread::yield();
                }
            }
        });
    }
    uint64_t successful_advances = 0;
    for (int attempt = 0; attempt < 200 && successful_advances < 20; ++attempt) {
        const auto result = gate.advance();
        if (result) {
            ++successful_advances;
        } else {
            EXPECT_EQ(result.error_code, CommErrorCode::NotReady);
        }
        std::this_thread::yield();
    }
    stop.store(true, std::memory_order_release);
    for (auto& worker : workers) worker.join();
    EXPECT_EQ(gate.current_phase(), successful_advances);
}

TEST(CommPhaseGateTest, LeaseLifecycleAndCloseAreObservable) {
    PhaseGate gate;
    kairo::comm::DoubleBuffer<int> buffer(0);
    ASSERT_TRUE(buffer.bind_to_phase_gate(gate));
    {
        auto reader = gate.try_begin_let_read();
        ASSERT_TRUE(reader.has_value());
        EXPECT_TRUE(gate.close());
        EXPECT_EQ(gate.advance().error_code, CommErrorCode::NotReady);
    }
    EXPECT_TRUE(gate.is_closed());
    EXPECT_FALSE(gate.try_begin_let_read().has_value());
    EXPECT_EQ(gate.advance().error_code, CommErrorCode::Closed);
}

TEST(CommSequencerTest, PublishesTicketsAndWaitsForExactTicket) {
    Sequencer sequencer("steps");
    const uint64_t first = sequencer.next_ticket();
    const uint64_t second = sequencer.next_ticket();
    EXPECT_EQ(first, 1U);
    EXPECT_EQ(second, 2U);

    std::atomic<bool> waiter_done{false};
    std::thread waiter([&] {
        const auto result = sequencer.wait_until_published(second, 1s);
        EXPECT_TRUE(result);
        waiter_done.store(true, std::memory_order_release);
    });

    EXPECT_TRUE(sequencer.publish(first));
    EXPECT_FALSE(waiter_done.load(std::memory_order_acquire));
    EXPECT_TRUE(sequencer.publish(second));

    waiter.join();
    EXPECT_TRUE(waiter_done.load(std::memory_order_acquire));
    EXPECT_TRUE(sequencer.is_published(second));
    EXPECT_EQ(sequencer.published_ticket(), second);
}

TEST(CommSequencerTest, MissedTicketReturnsMissedPhase) {
    Sequencer sequencer;
    EXPECT_TRUE(sequencer.publish(3));

    const auto result = sequencer.wait_until_published(2, 10ms);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error_code, CommErrorCode::MissedPhase);
    EXPECT_EQ(sequencer.stats().missed_phase_count, 1U);
}

TEST(CommSequencerTest, CloseWakesWaiter) {
    Sequencer sequencer;
    const uint64_t ticket = sequencer.next_ticket();
    std::atomic<bool> waiter_done{false};

    std::thread waiter([&] {
        const auto result = sequencer.wait_until_published(ticket, 1s);
        EXPECT_FALSE(result);
        EXPECT_EQ(result.error_code, CommErrorCode::Closed);
        waiter_done.store(true, std::memory_order_release);
    });

    std::this_thread::sleep_for(20ms);
    EXPECT_FALSE(waiter_done.load(std::memory_order_acquire));
    EXPECT_TRUE(sequencer.close());
    waiter.join();
    EXPECT_TRUE(waiter_done.load(std::memory_order_acquire));
}

TEST(CommSequencerTest, RejectsReservedHighBitAndStopsTicketingAfterClose) {
    constexpr uint64_t kReservedHighBit = uint64_t{1} << 63U;
    Sequencer sequencer;

    const auto invalid = sequencer.wait_until_published(kReservedHighBit, 0ns);
    EXPECT_FALSE(invalid);
    EXPECT_EQ(invalid.error_code, CommErrorCode::InvalidArgument);
    EXPECT_EQ(sequencer.stats().timeout_count, 0U);

    ASSERT_TRUE(sequencer.close());
    EXPECT_EQ(sequencer.next_ticket(), 0U);
}

TEST(CommSequencerTest, RejectsInvalidAndDuplicateTickets) {
    Sequencer sequencer;

    const auto invalid = sequencer.wait_until_published(0, 1ms);
    EXPECT_FALSE(invalid);
    EXPECT_EQ(invalid.error_code, CommErrorCode::InvalidArgument);

    EXPECT_TRUE(sequencer.publish(1));
    const auto duplicate = sequencer.publish(1);
    EXPECT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error_code, CommErrorCode::MissedPhase);
    EXPECT_EQ(sequencer.stats().missed_phase_count, 1U);
}

} // namespace
