/**
 * @brief Camera-frame fan-out for a robot planner and preview archive.
 *
 * The planner needs every accepted frame in FIFO order. The preview archive
 * may skip stale frames, so it retains only its most recent pending frame.
 */

#include <kairo/comm.hpp>
#include <array>

#include <iostream>

namespace {

struct CameraFrame {
    int sequence = 0;
    int detected_obstacles = 0;
};

void print_publish_result(const CameraFrame& frame,
                          const kairo::comm::TopicPublishResult& result) {
    std::cout << "capture frame=" << frame.sequence
              << " matched=" << result.matched_subscribers
              << " delivered=" << result.delivered_subscribers
              << " rejected=" << result.rejected_subscribers << "\n";
}

} // namespace

int main() {
    using kairo::comm::DropPolicy;
    using kairo::comm::Topic;
    using kairo::comm::TopicSubscriptionOptions;

    Topic<CameraFrame> camera_frames("camera_frames");

    TopicSubscriptionOptions planner_options;
    planner_options.capacity = 4;
    planner_options.drop_policy = DropPolicy::RejectNewest;
    planner_options.name = "planner";
    auto planner_frames = camera_frames.subscribe(planner_options);

    TopicSubscriptionOptions preview_options;
    preview_options.capacity = 1;
    preview_options.drop_policy = DropPolicy::KeepLatest;
    preview_options.name = "preview_archive";
    auto preview_frames = camera_frames.subscribe(preview_options);

    for (const CameraFrame frame : std::array<CameraFrame, 2>{{{101, 2}, {102, 1}}}) {
        print_publish_result(frame, camera_frames.publish(frame));
    }

    // The capture owner stops first; accepted frames remain available to drain.
    camera_frames.close();

    std::cout << "planner plans:";
    CameraFrame frame;
    while (planner_frames.try_receive(frame)) {
        std::cout << ' ' << frame.sequence;
    }

    std::cout << "\npreview archives:";
    while (preview_frames.try_receive(frame)) {
        std::cout << ' ' << frame.sequence;
    }
    std::cout << "\n";

    const auto planner_stats = planner_frames.stats();
    const auto preview_stats = preview_frames.stats();
    std::cout << "planner dropped=" << planner_stats.dropped_count
              << ", preview overwritten=" << preview_stats.overwritten_count << "\n";
    return 0;
}
