---
title: Troubleshoot by Symptom
description: Diagnose work that does not run, growing queues, wait timeouts, stalled shutdown, real-time drops, and GPU unavailability in a fixed order.
---

# Troubleshoot by Symptom

## Preserve facts before changing configuration

Do not first add threads, enlarge a queue, or increase priority. Record lifecycle, workload, and failures from the same moment; configuration changes can erase the most useful evidence.

```cpp
const auto snapshot = executor.get_snapshot();
```

Log `snapshot.lifecycle`, `partial`, `consistency_note`, completion/async state,
named realtime/Blocking I/O/GPU states, aggregate counters, failure counts, and
recent executor/task/message/timestamp context. `CompletionStatus` answers accepted
default-async unfinished work; backend state answers running and backlog; failures
and recent events explain category/context. If `partial` is true, record that fact
rather than treating absent data as zero. An individual future remains the source
of truth for that task.

Also record version, configuration summary, process start, latest deploy, input rate, and downstream dependency health. One snapshot is only now; alerts compare deltas and trends.

## Symptom 1: submitted work does not execute

Retain the returned future and use bounded `wait_for()` to distinguish unfinished work from an exception. Check `is_initialized`, `is_running`, `submit_rejected_count`, recent `SubmitRejected`, dependency-handle validity/origin/prerequisites, and periodic task status.

| Observation | Likely cause | Next action |
| --- | --- | --- |
| `is_initialized=false` | No initialization or default executor failed to establish | Call `initialize_ex()` before first submission; inspect code/message |
| `is_running=false` | Shutdown or initialization failure | Do not reuse a shut-down instance; rebuild the isolated component |
| Rejection count rises | Empty task, post-stop submission, unavailable entry | Find the call site from recent failure; return explicit request failure |
| `queued_tasks>0`, active unchanged | Blocked worker or dependency that cannot complete | Inspect running I/O, locks, dependency ownership |
| Future ready; `get()` throws | Task ran and failed | Handle business exception; do not hide it by resubmission |

Never infer “did not run” from a discarded future and absent log line: it could have failed, timed out in queue, or simply not logged.

## Symptom 2: queue delay keeps growing

Sample `queue_size`, `active_tasks`, `completed_tasks`, `avg_task_time_ms`, and input submission rate across at least three windows. If input exceeds sustainable completion, queue grows, end-to-end wait and soft timeouts grow, and a larger queue merely delays rejection while increasing stale work.

- Queue rising, active near thread count, CPU high: work exceeds compute capacity or tasks are too small for scheduling overhead.
- Queue rising, active near thread count, CPU low: tasks likely wait on locks, network, files, or device I/O.
- Periodic spikes that fall: perhaps acceptable bursts; still verify max end-to-end latency and expiry.
- High priority progresses while ordinary work does not: inspect sustained high-priority starvation rather than marking more work CRITICAL.

Rate-limit intake, merge overly fine work, bound I/O, and remove permanent loops before changing threads/capacity. Recovery is proven when the queue returns to baseline within budget and rejection/timeout deltas stop growing.

## Symptom 3: wait timeout

Use `wait_for_completion_ex(timeout)`, not a bare `false`, then record its
message/status and `get_snapshot()` before the predetermined degradation policy.

| Timeout snapshot | Meaning | Direction |
| --- | --- | --- |
| `active_tasks>0`, `queued_tasks=0` | Started work exceeds budget | Check deadline, locks, blocking I/O, cooperative stop |
| `active_tasks>0`, `queued_tasks>0` | Long work plus backlog | Stop intake, then choose continued drain or persistence |
| `active_tasks=0`, `pending_tasks>0` | Work relationship remains unsettled | Inspect dependency chains, submission race, related future |
| `is_running=false` | Kairo stopped | Do not keep waiting/submitting; inspect lifecycle order |

Timeout neither kills arbitrary C++ work nor rolls back effects. Decide whether to keep waiting, abandon the response while allowing background completion, persist/retry input, or accept fast-shutdown consequences. Retriable effects need idempotency keys.

## Symptom 4: shutdown stalls

```mermaid
flowchart TD
    A[Stop new requests and external callbacks] --> B[Notify permanent loops and blocking I/O to stop]
    B --> C[Wait for business producers to exit]
    C --> D[Bounded wait for accepted tasks]
    D --> E[Choose shutdown(true) or shutdown(false) from snapshot]
    E --> F[Destroy business objects captured by tasks]
```

Common causes are permanently blocked business work, producers submitting during drain, or captured objects destroyed before tasks. Before `shutdown()`, record a short-budget wait snapshot; confirm HTTP/device/timer/message producers stopped; give I/O and condition-variable waits a business timeout/wakeup; check worker-side waits on a small shared pool; use a thread dump to identify the actual blocking function. `shutdown(false)` is not thread killing or a cure for dangling captures.

## Symptom 5: real-time drop or unstable period

Inspect the named `RealtimeExecutorStatus` rather than ordinary async state.

| Field | Meaning | Action |
| --- | --- | --- |
| `is_running` | Dedicated thread runs | If false, inspect `_ex` register/start result and lifecycle |
| `queue_full_count` | Bounded queue full | Limit production, reduce work, reassess capacity |
| `pool_exhausted_count` | Preallocated task wrappers insufficient | Inspect in-flight peak and consumption budget |
| `rejected_not_running_count` | Push before start/after stop | Correct producer/thread start-stop order |
| `cycle_timeout_count` | Callback exceeded period | Shorten callback; remove allocation, locks, uncontrolled I/O |
| `peak_queue_size / queue_capacity` | Historical peak utilization | Near 1 consumes burst margin even without drops |
| `priority_applied` etc. | Requested tuning took effect | Check platform permission; false does not mean thread is stopped |

Compute window deltas for cumulative counters. `dropped_task_count` is the inclusive total; detailed counters distinguish not running, empty work, full queue, or exhausted pool. Emergency stop needs a separate safety path, not eventual queue consumption.

## Symptom 6: GPU unavailable or submission fails

Check in order: CMake backend enabled, runtime/device visibility for the final service account, `register_gpu_executor_ex()` error code/message, `GpuExecutorStatus` fields after registration, then each `submit_gpu()` future via `get()`. `BackendUnavailable` often means no compiled/implemented backend, no runtime, or no usable device; correct `InvalidConfig` first and investigate device/driver after `StartFailed`.

After registration failure, stop submitting to that named executor. If GPU is optional, take an explicit CPU path and record degradation; if required, fail health checks and stop intake rather than returning an empty success.

## Verify recovery

After recovery, prove new requests have definite results, old effects are neither duplicated nor lost, queues/wait/realtime depth return to steady range, failure counts stop rising, shutdown completes in budget and rejects post-stop work, GPU fallback returns correct business results, and the captured snapshot/root cause/action/prevention gate enters the incident record.

Continue with [failure observability](/en/reliability/failure-observability), [bounded waiting and status](/en/tutorial/waiting-and-status), and [dedicated real-time control](/en/realtime-and-communication/realtime-control).
