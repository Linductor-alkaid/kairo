---
title: Communication Observability
description: Use CommStats and CommEventCallback to observe communication backpressure, overwrites, stale reads, latency, and ordering errors.
---

# Communication Observability

## Goal

Treat communication failures as component-local protocol state and observe them through `stats()` and `set_event_callback()`. Do not assume they appear in `ExecutorFailureStatus`.

## Two observation paths

```cpp
channel.set_event_callback([](const kairo::comm::CommEvent& event) {
    report_comm_event(event.component_name, event.message);
});

const auto stats = channel.stats();
if (stats.dropped_count != 0 || stats.timeout_count != 0) {
    raise_backpressure_alert(stats);
}
```

`CommStats` is a local cumulative snapshot, including sends, receives, drops, overwrites, stale reads, sends after close, timeouts, handler exceptions, missed phases, current/peak depth, producer/consumer lag, and latency. It includes fixed logarithmic latency buckets and approximate `p50_latency` / `p99_latency`; `CommEventCallback` is appropriate for low-rate diagnostics. Callback exceptions are isolated and do not change the communication operation's result or component state. Installing/replacing a callback may allocate, and invoking it runs arbitrary application code after internal synchronization; both are control-plane operations, not part of a real-time guarantee.

Component latency is an age, wait duration, or publish-to-consume duration defined by that component. It is not end-to-end pipeline latency. Carry a source timestamp in the business message and compute the duration at the final consumer, as `comm_robot_pipeline` does for sensor-to-control latency. Report the component, measurement boundary, and sample count with any latency claim.

## Alert by semantic contract

| Component | Fields/events to focus on | Meaning |
| --- | --- | --- |
| `MpscChannel` | `dropped_count`, `current_depth`, `timeout_count` | Backpressure, backlog, or waiting failure |
| `RealtimeChannel` | `dropped_count`, `handler_exception_count`, lag | Insufficient cycle budget, full queue, or handler failure |
| `LatestMailbox` | `overwritten_count`, `stale_read_count` | Fast updates or consumer has not obtained a newer version |
| `DoubleBuffer` | Sequence, `stale_read_count`, latency | Reader misses a new snapshot or state publishing is delayed |
| `PhaseGate` / `Sequencer` | `timeout_count`, `missed_phase_count` | Stage is absent/closed or ordering was skipped |

Thresholds come from the business period and data importance. Mailbox overwrite is normal when only the latest target matters; it indicates the wrong component when every audit event must be retained.

## Boundary with Kairo failures

`CommStats` and `CommEventCallback` do not aggregate into `ExecutorFailureStatus` and do not call `Executor::set_failure_callback()` by default. Bridge low-frequency component events to your monitoring system for unified alerts. Do not install callbacks on high-frequency paths merely because `is_synchronization_lock_free()` is true: that query covers internal atomics, not event/string construction, callback allocation or user code. Poll counters from an ordinary monitoring thread when the real-time path must remain bounded.

Continue with [capacity and alerts](/en/realtime-and-communication/capacity-and-alerting), or use the [complete robot pipeline](/en/tutorial/complete-robot-pipeline) for fault-injection checks.
