---
title: Fan Out Camera Frames with Topic
description: Give robot planning and preview archiving independent camera-frame contracts.
---

# Fan Out Camera Frames with Topic

The robot pipeline now has one camera and two downstream jobs. The navigation planner must inspect every frame that enters its bounded queue, in order. A preview archive is deliberately slower: it only needs the newest pending frame for an operator preview. It must not delay planning or make the camera producer wait.

This is a different problem from the earlier task tutorials. It is a continuous data edge, not a finite calculation with a `future`. The runnable program for this chapter is [`examples/topic_subscriptions.cpp`](https://github.com/Linductor-alkaid/kairo/blob/master/examples/topic_subscriptions.cpp).

## Start from the delivery contract

| Role | Needs | Capacity and overload choice |
| --- | --- | --- |
| Camera capture | Publish each newly captured frame | Records the result instead of assuming delivery |
| Navigation planner | Ordered frames for obstacle planning | FIFO capacity 4, `RejectNewest` |
| Preview archive | Only the latest pending preview | Capacity 1, `KeepLatest` |

A single `MpscChannel<CameraFrame>` is wrong here: planner and archive would compete, so each frame would go to only one role. Blocking capture until archive catches up is also wrong because it turns a noncritical display concern into camera latency. `Topic<CameraFrame>` gives each role a separate bounded FIFO.

## Create the two independent paths

The first part of the example creates the camera-owned Topic and both RAII subscription handles.

```cpp
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
```

`planner_frames` stays alive with the planning role; `preview_frames` stays alive with the archive role. Topic does not replay earlier frames, so create required subscriptions before enabling capture. The planner's `RejectNewest` policy makes its overload visible. The preview's `KeepLatest` policy replaces stale preview work without affecting planner delivery.

## Publish a short camera burst

The example uses two frames without consuming the preview queue between them. That makes the overload choice observable without relying on timing.

```cpp
for (const CameraFrame frame : std::array<CameraFrame, 2>{{{101, 2}, {102, 1}}}) {
    print_publish_result(frame, camera_frames.publish(frame));
}
```

Each result counts this publish snapshot: `matched_subscribers`, `delivered_subscribers`, and `rejected_subscribers`. Both publishes report two deliveries: `KeepLatest` accepts frame 102 by overwriting frame 101, while `RejectNewest` would report a rejection only when the planner queue is full. A false publish result is not an instruction to retry blindly, because a retry could duplicate a frame for subscriptions that already accepted it.

## Consume according to each job's value rule

After capture stops, the example closes Topic and drains both role queues.

```cpp
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
```

Run it with:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release \
  -DKAIRO_BUILD_EXAMPLES=ON \
  -DKAIRO_ENABLE_GPU=OFF
cmake --build build --target topic_subscriptions
./build/examples/topic_subscriptions
```

Expected output:

```text
capture frame=101 matched=2 delivered=2 rejected=0
capture frame=102 matched=2 delivered=2 rejected=0
planner plans: 101 102
preview archives: 102
planner dropped=0, preview overwritten=1
```

The planner sees both frames in FIFO order. The archive sees only 102 because its one-entry queue replaces a stale 101. The final statistic proves that this was an intentional overwrite, not an unexplained missing frame.

## Own shutdown with the producer

The capture owner closes `camera_frames` after it stops producing. Close wakes waiting receivers, but accepted frames remain available for draining first. A subscription's destructor or `close()` unregisters that role; concurrent publish and unsubscribe are lifetime-safe, though a publish snapshot may still deliver or reject for the subscription being removed. Do not destroy a subscription handle while another thread is calling a member on that same handle.

In a long-running robot, stop camera acquisition, close the Topic, let planner and archive drain within their own budgets, then join their threads. Do not use Topic close as a substitute for stopping the external camera driver.

## Know the boundary before deployment

`Topic<T>` is an in-process, best-effort fan-out control-plane primitive. Its subscription registry and every publish snapshot use a mutex and dynamic allocation; copying and fan-out time grow with subscriber count. It has no replay, persistence, acknowledgement, networking, reconnect, cross-subscriber atomic delivery, or hard-real-time guarantee.

For large immutable images, use explicit shared ownership such as `Topic<std::shared_ptr<const Image>>`; this avoids copying image bytes but does not make Topic real-time. A fixed-cycle control edge needs a separate preallocated component and an explicit consumer budget.

## Change the design when the task changes

| New requirement | Better direction |
| --- | --- |
| Only planner needs each frame | Use one bounded `MpscChannel` |
| A monitor needs current camera calibration | Use `LatestMailbox` or `DoubleBuffer` |
| Preview must retain every image durably | Hand work to a bounded archival pipeline with persistence and acknowledgements |
| A control cycle must consume data within a fixed budget | Use a dedicated real-time path, not Topic |

Next, [Complete Robot Pipeline](/en/tutorial/complete-robot-pipeline) combines this camera edge with configuration, control commands, snapshots, startup dependencies, and a whole-system shutdown order.
