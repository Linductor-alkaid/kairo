#include <iostream>

#include <kairo/comm.hpp>

int main() {
    kairo::comm::ChannelOptions options;
    options.capacity = 2;
    options.name = "sensor_frames";
    kairo::comm::MpscChannel<int> frames(options);
    frames.try_send(7);
    int frame = 0;
    const bool received = frames.try_receive(frame);

    kairo::comm::LatestMailbox<int> config("control_config");
    config.publish(3);
    int gain = 0;
    uint64_t sequence = 0;
    const bool loaded = config.try_load_newer_than(0, gain, sequence);

    kairo::comm::DoubleBuffer<int> state(0, "system_state");
    state.publish(frame * gain);
    const auto snapshot = state.load();

    kairo::comm::PhaseGate startup("startup");
    const bool ready = startup.advance_to(1) && startup.wait_for(1, std::chrono::milliseconds(1));

    std::cout << "frame=" << frame << ", gain=" << gain
              << ", state=" << snapshot.value
              << ", phase=" << (ready ? "ready" : "blocked") << '\n';
    return received && loaded && ready && snapshot.value == 21 ? 0 : 1;
}
