#include <kairo/comm.hpp>

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

using kairo::comm::CommEventKind;
using kairo::comm::CommErrorCode;
using kairo::comm::DoubleBuffer;
using kairo::comm::PhaseGate;
using kairo::comm::Snapshot;

struct State {
    int version = 0;
    int checksum = 0;
    std::string label;
};

struct LetState {
    uint64_t version = 0;
    uint64_t checksum = 0;
    std::array<uint64_t, 16> lanes{};
};

struct NonDefaultState {
    explicit NonDefaultState(int value) noexcept : value(value) {}
    NonDefaultState() = delete;
    NonDefaultState(const NonDefaultState&) noexcept = default;
    NonDefaultState& operator=(const NonDefaultState&) noexcept = default;

    int value;
};

LetState make_let_state(uint64_t version) noexcept {
    LetState state;
    state.version = version;
    state.checksum = version * 17U;
    for (size_t index = 0; index < state.lanes.size(); ++index) {
        state.lanes[index] = version ^ (0x9e3779b97f4a7c15ULL + index);
    }
    return state;
}

bool is_consistent(const LetState& state) noexcept {
    if (state.checksum != state.version * 17U) return false;
    for (size_t index = 0; index < state.lanes.size(); ++index) {
        if (state.lanes[index] !=
            (state.version ^ (0x9e3779b97f4a7c15ULL + index))) {
            return false;
        }
    }
    return true;
}

State make_state(int version) {
    return State{
        .version = version,
        .checksum = version * 17,
        .label = "state-" + std::to_string(version),
    };
}

void expect_consistent(const State& state) {
    EXPECT_EQ(state.checksum, state.version * 17);
    EXPECT_EQ(state.label, "state-" + std::to_string(state.version));
}

TEST(CommDoubleBufferTest, SupportsNonDefaultConstructibleValues) {
    DoubleBuffer<NonDefaultState> buffer(NonDefaultState{1});
    EXPECT_EQ(buffer.load().value.value, 1);

    EXPECT_EQ(buffer.publish(NonDefaultState{2}), 1U);
    Snapshot<NonDefaultState> snapshot{NonDefaultState{0}};
    ASSERT_TRUE(buffer.try_load(snapshot));
    EXPECT_EQ(snapshot.value.value, 2);
}

TEST(CommDoubleBufferTest, LoadReturnsInitialSnapshot) {
    DoubleBuffer<State> buffer(make_state(1), "state");

    const Snapshot<State> snapshot = buffer.load();
    EXPECT_EQ(snapshot.sequence, 0U);
    expect_consistent(snapshot.value);
    EXPECT_EQ(snapshot.value.version, 1);
    EXPECT_EQ(buffer.sequence(), 0U);
    EXPECT_EQ(buffer.stats().received_count, 1U);
}

TEST(CommDoubleBufferTest, PublishReturnsCompleteSnapshots) {
    DoubleBuffer<State> buffer(make_state(1));

    const uint64_t sequence = buffer.publish(make_state(2));
    EXPECT_EQ(sequence, 1U);

    const auto snapshot = buffer.load();
    EXPECT_EQ(snapshot.sequence, 1U);
    EXPECT_EQ(snapshot.value.version, 2);
    expect_consistent(snapshot.value);

    const auto stats = buffer.stats();
    EXPECT_EQ(stats.sent_count, 1U);
    EXPECT_EQ(stats.received_count, 1U);
    EXPECT_EQ(stats.capacity, 2U);
    EXPECT_EQ(stats.current_depth, 1U);
}

TEST(CommDoubleBufferTest, UpdateMutatesInactiveCopyThenPublishesOnce) {
    DoubleBuffer<State> buffer(make_state(3));

    const uint64_t sequence = buffer.update([](State& state) {
        state.version = 4;
        state.checksum = state.version * 17;
        state.label = "state-4";
    });

    EXPECT_EQ(sequence, 1U);
    const auto snapshot = buffer.load();
    EXPECT_EQ(snapshot.value.version, 4);
    expect_consistent(snapshot.value);
}

TEST(CommDoubleBufferTest, LoadNewerThanAvoidsDuplicateConsumption) {
    DoubleBuffer<State> buffer(make_state(1), "state_buffer");
    int stale_events = 0;
    buffer.set_event_callback([&](const kairo::comm::CommEvent& event) noexcept {
        if (event.kind == CommEventKind::StaleRead) {
            ++stale_events;
            EXPECT_EQ(event.component_name, "state_buffer");
        }
    });

    Snapshot<State> snapshot;
    EXPECT_FALSE(buffer.load_newer_than(0, snapshot));
    EXPECT_EQ(buffer.stats().stale_read_count, 1U);
    EXPECT_EQ(stale_events, 1);

    const uint64_t sequence = buffer.publish(make_state(2));
    ASSERT_TRUE(buffer.load_newer_than(0, snapshot));
    EXPECT_EQ(snapshot.sequence, sequence);
    EXPECT_EQ(snapshot.value.version, 2);
    expect_consistent(snapshot.value);

    EXPECT_FALSE(buffer.load_newer_than(snapshot.sequence, snapshot));
    EXPECT_EQ(buffer.stats().stale_read_count, 2U);
}

TEST(CommDoubleBufferTest, MultipleReadersSeeConsistentSnapshots) {
    DoubleBuffer<State> buffer(make_state(0));
    std::atomic<bool> stop{false};
    std::atomic<int> readers_started{0};
    std::atomic<int> checked{0};
    std::vector<std::thread> readers;

    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([&] {
            readers_started.fetch_add(1, std::memory_order_release);
            while (!stop.load(std::memory_order_acquire)) {
                const auto snapshot = buffer.load();
                EXPECT_EQ(snapshot.value.checksum, snapshot.value.version * 17);
                EXPECT_EQ(snapshot.value.label,
                          "state-" + std::to_string(snapshot.value.version));
                checked.fetch_add(1, std::memory_order_acq_rel);
            }
        });
    }

    while (readers_started.load(std::memory_order_acquire) < 4) {
        std::this_thread::yield();
    }
    while (checked.load(std::memory_order_acquire) == 0) {
        std::this_thread::yield();
    }

    for (int version = 1; version <= 200; ++version) {
        buffer.publish(make_state(version));
    }

    stop.store(true, std::memory_order_release);
    for (auto& reader : readers) {
        reader.join();
    }

    EXPECT_GT(checked.load(std::memory_order_acquire), 0);
    const auto latest = buffer.load();
    EXPECT_EQ(latest.sequence, 200U);
    EXPECT_EQ(latest.value.version, 200);
    expect_consistent(latest.value);
}

TEST(CommDoubleBufferTest, HighFrequencyWriterNeverExposesHalfUpdatedState) {
    DoubleBuffer<State> buffer(make_state(0));
    std::atomic<bool> stop{false};
    std::atomic<int> inconsistent{0};

    std::thread reader([&] {
        while (!stop.load(std::memory_order_acquire)) {
            const auto snapshot = buffer.load();
            if (snapshot.value.checksum != snapshot.value.version * 17 ||
                snapshot.value.label != "state-" + std::to_string(snapshot.value.version)) {
                inconsistent.fetch_add(1, std::memory_order_acq_rel);
            }
        }
    });

    for (int version = 1; version <= 500; ++version) {
        buffer.update([version](State& state) {
            state.version = version;
            state.checksum = version * 17;
            state.label = "state-" + std::to_string(version);
        });
    }

    stop.store(true, std::memory_order_release);
    reader.join();

    EXPECT_EQ(inconsistent.load(std::memory_order_acquire), 0);
    EXPECT_EQ(buffer.sequence(), 500U);
}

TEST(CommDoubleBufferTest, PhaseBoundModeMakesOutputVisibleAtNextPhase) {
    PhaseGate gate("control");
    DoubleBuffer<int> buffer(0, "command");
    ASSERT_TRUE(buffer.bind_to_phase_gate(gate));

    Snapshot<int> snapshot;
    EXPECT_FALSE(buffer.load_for_current_phase(snapshot));
    EXPECT_TRUE(buffer.publish_for_current_phase(10));
    const auto duplicate = buffer.publish_for_current_phase(11);
    EXPECT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error_code, CommErrorCode::MissedPhase);
    EXPECT_FALSE(buffer.load_for_current_phase(snapshot));

    ASSERT_TRUE(gate.advance());
    ASSERT_TRUE(buffer.load_for_current_phase(snapshot));
    EXPECT_EQ(snapshot.sequence, 0U);
    EXPECT_EQ(snapshot.value, 10);

    ASSERT_TRUE(buffer.publish_for_current_phase(20));
    ASSERT_TRUE(buffer.load_for_current_phase(snapshot));
    EXPECT_EQ(snapshot.value, 10);

    ASSERT_TRUE(gate.advance());
    ASSERT_TRUE(buffer.load_for_current_phase(snapshot));
    EXPECT_EQ(snapshot.sequence, 1U);
    EXPECT_EQ(snapshot.value, 20);
}

TEST(CommDoubleBufferTest, PhaseBoundModeDiagnosesMissingPriorPhase) {
    PhaseGate gate;
    DoubleBuffer<int> buffer;
    ASSERT_TRUE(buffer.bind_to_phase_gate(gate));
    ASSERT_TRUE(gate.advance_to(3));

    Snapshot<int> snapshot;
    const auto result = buffer.load_for_current_phase(snapshot);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error_code, CommErrorCode::NotReady);
}

TEST(CommDoubleBufferTest, ConcurrentLetWritersPublishOneCompleteSnapshotPerPhase) {
    constexpr size_t kWriterCount = 8;
    constexpr uint64_t kPhaseCount = 64;

    PhaseGate gate("multi_writer_let");
    DoubleBuffer<LetState> buffer(make_let_state(0));
    ASSERT_TRUE(buffer.bind_to_phase_gate(gate));
    ASSERT_TRUE(buffer.is_synchronization_lock_free());

    for (uint64_t phase = 0; phase < kPhaseCount; ++phase) {
        std::atomic<size_t> ready{0};
        std::atomic<bool> start{false};
        std::array<CommErrorCode, kWriterCount> results{};
        std::vector<std::thread> writers;
        writers.reserve(kWriterCount);

        for (size_t writer = 0; writer < kWriterCount; ++writer) {
            writers.emplace_back([&, writer] {
                const LetState candidate =
                    make_let_state((phase + 1U) * 1000U + writer);
                ready.fetch_add(1, std::memory_order_release);
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                const auto result = buffer.publish_for_current_phase(candidate);
                results[writer] = result.error_code;
            });
        }

        while (ready.load(std::memory_order_acquire) != kWriterCount) {
            std::this_thread::yield();
        }
        start.store(true, std::memory_order_release);
        for (auto& writer : writers) writer.join();

        size_t successes = 0;
        for (const CommErrorCode result : results) {
            if (result == CommErrorCode::Ok) {
                ++successes;
            } else {
                EXPECT_TRUE(result == CommErrorCode::NotReady ||
                            result == CommErrorCode::MissedPhase);
            }
        }
        ASSERT_EQ(successes, 1U) << "phase=" << phase;

        ASSERT_TRUE(gate.advance());
        Snapshot<LetState> snapshot;
        ASSERT_TRUE(buffer.load_for_current_phase(snapshot));
        EXPECT_EQ(snapshot.sequence, phase);
        EXPECT_TRUE(is_consistent(snapshot.value));
        EXPECT_EQ(snapshot.value.version / 1000U, phase + 1U);
    }
}

} // namespace
