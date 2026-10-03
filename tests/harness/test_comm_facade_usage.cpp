#include <kairo/comm.hpp>
#include <kairo/executor.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace {

struct SensorFrame {
    int sequence = 0;
    double value = 0.0;
};

TEST(FacadeCommUsage, SensorProducerPlannerConsumer) {
    kairo::comm::ChannelOptions options;
    options.capacity = 16;
    kairo::comm::MpscChannel<SensorFrame> frames(options);

    std::thread sensor([&] {
        for (int i = 0; i < 8; ++i) {
            while (!frames.try_send(SensorFrame{.sequence = i, .value = i * 0.5})) {
                std::this_thread::yield();
            }
        }
        frames.close();
    });

    std::vector<int> planned_sequences;
    SensorFrame frame;
    while (true) {
        const auto result = frames.receive_for(frame, std::chrono::milliseconds(500));
        if (!result) {
            EXPECT_EQ(result.error_code, kairo::comm::CommErrorCode::Closed);
            break;
        }
        planned_sequences.push_back(frame.sequence);
    }

    sensor.join();

    ASSERT_EQ(planned_sequences.size(), 8U);
    for (int i = 0; i < 8; ++i) {
        EXPECT_EQ(planned_sequences[static_cast<size_t>(i)], i);
    }
    EXPECT_EQ(frames.stats().sent_count, 8U);
    EXPECT_EQ(frames.stats().received_count, 8U);
}

TEST(FacadeCommUsage, SensorFramesFanOutToPlannerAndRecorder) {
    kairo::comm::Topic<SensorFrame> frames("sensor_frames");
    auto planner = frames.subscribe({.capacity = 8, .name = "planner"});
    auto recorder = frames.subscribe({.capacity = 2,
                                      .drop_policy = kairo::comm::DropPolicy::KeepLatest,
                                      .name = "recorder"});

    for (int sequence = 0; sequence < 4; ++sequence) {
        const auto result = frames.publish(SensorFrame{.sequence = sequence,
                                                       .value = sequence * 0.5});
        EXPECT_EQ(result.matched_subscribers, 2U);
        EXPECT_EQ(result.rejected_subscribers, 0U);
    }

    SensorFrame frame;
    for (int sequence = 0; sequence < 4; ++sequence) {
        ASSERT_TRUE(planner.try_receive(frame));
        EXPECT_EQ(frame.sequence, sequence);
    }
    ASSERT_TRUE(recorder.try_receive(frame));
    EXPECT_EQ(frame.sequence, 2);
    ASSERT_TRUE(recorder.try_receive(frame));
    EXPECT_EQ(frame.sequence, 3);
    EXPECT_FALSE(recorder.try_receive(frame));
    EXPECT_EQ(recorder.stats().overwritten_count, 1U);
}

TEST(FacadeCommUsage, ConfigThreadRealtimeControlThread) {
    struct ControlConfig {
        int gain = 0;
        bool enabled = false;
    };

    kairo::comm::LatestMailbox<ControlConfig> config_box("control_config");
    config_box.publish(ControlConfig{.gain = 1, .enabled = true});
    config_box.publish(ControlConfig{.gain = 3, .enabled = true});

    uint64_t seen_sequence = 0;
    ControlConfig active_config;
    ASSERT_TRUE(config_box.try_load_newer_than(seen_sequence, active_config, seen_sequence));

    EXPECT_EQ(active_config.gain, 3);
    EXPECT_TRUE(active_config.enabled);
    EXPECT_EQ(seen_sequence, 2U);
    EXPECT_FALSE(config_box.try_load_newer_than(seen_sequence, active_config, seen_sequence));
    EXPECT_EQ(config_box.stats().overwritten_count, 1U);
}

TEST(FacadeCommUsage, InitThreadWorkerThread) {
    kairo::comm::PhaseGate startup_gate("startup");
    std::atomic<bool> worker_started{false};
    std::atomic<bool> worker_completed{false};

    std::thread worker([&] {
        worker_started.store(true, std::memory_order_release);
        const auto result = startup_gate.wait_for(1, std::chrono::milliseconds(500));
        ASSERT_TRUE(result);
        worker_completed.store(true, std::memory_order_release);
    });

    while (!worker_started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    EXPECT_FALSE(worker_completed.load(std::memory_order_acquire));
    EXPECT_TRUE(startup_gate.advance());

    worker.join();
    EXPECT_TRUE(worker_completed.load(std::memory_order_acquire));
    EXPECT_EQ(startup_gate.current_phase(), 1U);
    EXPECT_EQ(startup_gate.stats().received_count, 1U);
}

TEST(FacadeCommUsage, StateWriterMonitorReader) {
    struct SystemState {
        int tick = 0;
        int checksum = 0;
    };

    kairo::comm::DoubleBuffer<SystemState> states(SystemState{.tick = 0, .checksum = 0});

    states.publish(SystemState{.tick = 1, .checksum = 17});
    states.update([](SystemState& state) {
        state.tick = 2;
        state.checksum = 34;
    });

    kairo::comm::Snapshot<SystemState> snapshot;
    ASSERT_TRUE(states.load_newer_than(0, snapshot));
    EXPECT_EQ(snapshot.sequence, 2U);
    EXPECT_EQ(snapshot.value.tick, 2);
    EXPECT_EQ(snapshot.value.checksum, snapshot.value.tick * 17);

    EXPECT_FALSE(states.load_newer_than(snapshot.sequence, snapshot));
    EXPECT_EQ(states.stats().stale_read_count, 1U);
}

TEST(FacadeCommUsage, RealtimeCycleDrainsMessages) {
    kairo::comm::RealtimeChannelOptions options;
    options.capacity = 8;
    options.max_items_per_cycle = 2;

    kairo::comm::RealtimeChannel<int> commands(options);
    ASSERT_TRUE(commands.try_send(10));
    ASSERT_TRUE(commands.try_send(20));
    ASSERT_TRUE(commands.try_send(30));

    int applied_sum = 0;
    const size_t first_cycle = commands.drain_for_cycle([&](int command) {
        applied_sum += command;
    });

    EXPECT_EQ(first_cycle, 2U);
    EXPECT_EQ(applied_sum, 30);
    EXPECT_EQ(commands.stats().current_depth, 1U);

    const size_t second_cycle = commands.drain_for_cycle([&](int command) {
        applied_sum += command;
    });
    EXPECT_EQ(second_cycle, 1U);
    EXPECT_EQ(applied_sum, 60);
    EXPECT_TRUE(commands.empty());
}

TEST(FacadeCommUsage, TaskGraphSubmitAfter) {
    kairo::Executor executor;
    kairo::ExecutorConfig config;
    config.min_threads = 2;
    config.max_threads = 2;
    ASSERT_TRUE(executor.initialize(config));

    std::atomic<int> stage{0};
    auto init = executor.submit_with_handle([&] {
        stage.store(1, std::memory_order_release);
        return 10;
    });

    auto planned = executor.submit_after(init.handle, [&] {
        EXPECT_EQ(stage.load(std::memory_order_acquire), 1);
        stage.store(2, std::memory_order_release);
        return 20;
    });

    EXPECT_EQ(init.future.get(), 10);
    EXPECT_EQ(planned.get(), 20);
    EXPECT_EQ(stage.load(std::memory_order_acquire), 2);

    executor.shutdown();
}

} // namespace
