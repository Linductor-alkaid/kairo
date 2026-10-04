---
title: Capacity and Alerts
description: Derive window rates, queue utilization, consumption margin, and cycle overruns from communication and real-time cumulative statistics, then connect alerts to action.
---

# Capacity and Alerts

This page is for experts who have already chosen bounded communication or a real-time queue. Ordinary background tasks need not choose a backend from these metrics; first use [Execution Models and Routing Boundaries](/en/guides/execution-models-and-routing) to determine whether the business needs completion or admission. `dispatch_auto(RealtimeQueue).accepted` is one enqueue result, not a replacement for cycle, drop, or capacity monitoring.

## Answer three capacity questions first

Capacity is more than “is the queue large enough?” Before deployment, answer:

1. **Is steady state sustainable?** Can consumers process faster than producers over time?
2. **Can bursts be absorbed?** Does capacity cover the permitted burst duration while retaining data freshness?
3. **What is lost under overload?** Do rejection, overwrite, timeout, or missed phases match the business contract?

Every alert is semantic. The same `overwritten_count` is normal for a latest-control-target mailbox and proof of a wrong component for audit events.

## Convert cumulative counters into window signals

Most `CommStats` and `RealtimeExecutorStatus` counters accumulate from component creation. Alerting on “cumulative count > 100” inevitably becomes false-positive with runtime age. Store a timestamp and prior snapshot, then calculate deltas:

```cpp
struct CommWindow {
    double accepted_per_second = 0.0;
    double received_per_second = 0.0;
    double drop_events_per_second = 0.0;
    double depth_ratio = 0.0;
};

CommWindow calculate_window(const kairo::comm::CommStats& before,
                            const kairo::comm::CommStats& after,
                            std::chrono::duration<double> elapsed) {
    const double seconds = elapsed.count();
    const auto delta = [](uint64_t old_value, uint64_t new_value) {
        return new_value >= old_value ? new_value - old_value : 0;
    };

    return {
        delta(before.sent_count, after.sent_count) / seconds,
        delta(before.received_count, after.received_count) / seconds,
        delta(before.dropped_count, after.dropped_count) / seconds,
        after.capacity == 0 ? 0.0 : static_cast<double>(after.current_depth) / after.capacity,
    };
}
```

Production code also handles `seconds <= 0`, component recreation, and counter wrap. After recreation counters restart at zero, so monitoring labels need a component name plus instance/process-start identity.

`sent_count` is successful admission, not necessarily producer attempts. Under `RejectNewest`, rejected inputs increment drops but not sends; `DropOldest` admits the new value while discarding an old one; `KeepLatest` clears old backlog and increments overwrites before admitting the new value. If exact attempt/rejection rates matter, count attempts, accepts, and rejects at the `try_send()`/`send_for()` boundary.

## Total admission for default async submits

Beyond communication and realtime components, ordinary `submit()` / `submit_batch()` submissions have **no** total bound by default: `queue_capacity` only sizes per-worker local queues, and tasks fall back to the global scheduler queue when a local queue is full — overload shows up as unbounded backlog. Configure `max_in_flight_tasks` explicitly when structured overload rejection is required:

```cpp
kairo::ExecutorConfig config;
config.max_in_flight_tasks = 256;   // total accepted-but-unsettled submissions
```

- At the bound, submission does not throw: the future settles immediately with `CapacityExhaustedException` and a `FailureKind::CapacityExhausted` event is recorded (`capacity_exhausted_count` can be windowed for alerting), distinguishable from executor stop and invalid input.
- Every admitted submission releases its capacity slot exactly once — on completion, exception, queued cancellation, pre-execution timeout, rejection, or drop; counters are finalized before the future becomes ready.
- Coverage includes ordinary/priority/batch/dependency/cancellable/serial (`submit_on*`) submits; timer firing (`submit_delayed*`/`submit_periodic*`) and realtime/GPU are out of scope — their backpressure semantics are the rest of this page.
- `get_in_flight_submissions()` is an instantaneous watermark; combine it with windowed `capacity_exhausted_count` growth: watermark at the bound plus rising rejections means sustained overload; watermark at the bound with zero rejections means the bound simply fits.

Alert handling follows the same discipline as communication components: determine whether the cause is excess input, slow consumption, or a leak (in-flight count that never drops), then throttle, scale, or shed load. Do not hide consumer stalls by raising `max_in_flight_tasks`.

## `MpscChannel`: every message matters

For a FIFO flow, calculate within stable windows:

```text
accepted_rate = Δsent_count / Δt
receive_rate  = Δreceived_count / Δt
net_rate      = accepted_rate - receive_rate
depth_ratio   = current_depth / capacity
drain_time    ≈ current_depth / receive_rate
```

Repeated `net_rate > 0` means backlog is unsustainable. `drain_time` is meaningful only while recent receive rate is stable and nonzero. An initial capacity estimate is:

```text
capacity >= peak net arrival rate × permitted burst duration + safety margin
```

It must also satisfy oldest-message deadline, not merely hold ten minutes of stale work.

| Level | Window condition | Action |
| --- | --- | --- |
| Notice | `depth_ratio > 0.5` for several windows or `net_rate > 0` | Inspect input changes, consumer time, downstream dependencies |
| Warning | `depth_ratio > 0.8` or estimated `drain_time` approaches freshness budget | Rate-limit, add consumers, or degrade noncritical input |
| Critical | A must-not-drop stream has `Δdropped_count > 0`, or `Δtimeout_count > 0` affects requests | Protect intake, preserve failed input, recover with idempotency protocol |
| Lifecycle fault | `Δclosed_send_count > 0` while running | Inspect producer/channel close order |

These are initial templates, not library-wide thresholds. Calibrate using traffic distribution, deadlines, and recovery objectives.

## `RealtimeChannel`: cycle budget limits consumption

`drain_for_cycle()` consumes at most `max_items_per_cycle`; configured `0` means unlimited, which can destroy period determinism. Ignoring handler cost, the theoretical upper bound is:

```text
theoretical_items_per_second = max_items_per_cycle × 1,000,000,000 / cycle_period_ns
```

This is only an entry filter. Target-hardware measurement must show input below sustainable consumption, handler plus callback tail latency inside the cycle budget, depth falling after bursts, and no unacceptable `dropped_count`/`handler_exception_count` deltas. Raising the item budget to reduce drops while raising `cycle_timeout_count` merely trades data loss for missed cycles; reduce per-item work, preprocess outside real time, lower input rate, or redefine which data may overwrite.

## Keep real-time queue pressure separate from cycle pressure

Derive two-window signals from `RealtimeExecutorStatus`:

```text
cycle_timeout_ratio = Δcycle_timeout_count / Δcycle_count
drop_rate           = Δdropped_task_count / Δt
queue_full_rate     = Δqueue_full_count / Δt
pool_exhausted_rate = Δpool_exhausted_count / Δt
cycle_load_ema      = avg_cycle_time_ns / cycle_period_ns
max_cycle_load      = max_cycle_time_ns / cycle_period_ns
```

`avg_cycle_time_ns` is an exponential moving average for recent trend; `max_cycle_time_ns` is a lifetime maximum preserved after recovery. Neither is p99.

| Signal | Meaning | First action |
| --- | --- | --- |
| `Δrejected_not_running_count > 0` | Push before start, after stop, or during shutdown race | Fix producer lifecycle, do not add capacity |
| `Δrejected_empty_task_count > 0` | Empty task pushed | Fix caller validation |
| `Δqueue_full_count > 0` | Bounded queue lacks admission margin | Lower producer rate/work or assess capacity |
| `Δpool_exhausted_count > 0` | Preallocated wrappers exhausted | Check in-flight peak and consumption budget |
| Rising `cycle_timeout_ratio` | Cycle work exceeds period | Shorten callback/task; remove locks, allocations, unbounded I/O |
| `cycle_load_ema` persistently near 1 | Insufficient steady margin | Act before timeouts; margin follows jitter SLO |

`dropped_task_count` is the inclusive total of realtime rejections—useful for service health—while detailed counters route actions. Do not add it to `queue_full_count`. `failed_pushes` and `peak_queue_size` require underlying queue statistics; peak/capacity shows lifetime historical saturation, not current congestion.

Immediately critical: any new safety-critical drop or active control producer while `is_running=false`. Warning: timeout ratio exceeds business tolerance or load approaches reserved margin repeatedly. Treat deployment settings such as missing `priority_applied` or `cpu_affinity_applied` as a separate deployment fault. A 1 kHz control loop and a 1 Hz sampler cannot share a hard-coded percentage threshold.

## Latest state, phase, and latency

`LatestMailbox` and `DoubleBuffer` express current state, not a queue; `current_depth / capacity == 1` is not congestion. For a mailbox, overwrite can be intended, stale reads can occur when polling outpaces publishing, latency is age from publish to successful read, and `consumer_lag` is skipped sequences—not queue length. Alert when current age exceeds deadline, sequence stops advancing, or skipped versions lose business actions.

For `DoubleBuffer`, observe publication sequence progress, readers stuck on old values, and read latency. If every intermediate state matters, use `MpscChannel` instead.

For `PhaseGate`/`Sequencer`, lag fields are protocol-specific rather than depth. Alert `Δmissed_phase_count > 0` as a likely protocol violation, classify `Δtimeout_count > 0` by business deadline and producer health, inspect `Δclosed_send_count > 0` as shutdown race, and alert a growing waiter count with no phase/ticket progress.

`CommStats::avg_latency` is a lifetime cumulative average and `max_latency` a lifetime maximum; short degradation can be hidden by history and recovery does not lower maximum. For a latency SLO, record per-item or sampled histograms at the business boundary, report p50/p95/p99/max in fixed windows with sample/drop counts, and record missing data as a separate availability event.

## Connect alerts to action

Every rule names data semantics, window/threshold, user impact, automatic action, and human investigation. For example: camera frames may drop old data but the newest age must remain below 100 ms for three 30-second windows; impact is unreliable control, automatic action is suppressing control output into a safe state, and investigation checks producer sequence, handler latency, real-time period, and device health.

Run an idle baseline, long target-rate steady state, a short burst, persistent overload, slow/throwing consumer, and shutdown race. Save before/after snapshots, configuration, input distribution, window formula, alert time, automated action, and recovery time. “The process did not crash” is not capacity validation.

For field sources, revisit [communication observability](/en/realtime-and-communication/observability), [troubleshooting](/en/reliability/troubleshooting), and [Linux, Windows, and Android deployment](/en/reliability/platform-deployment).
