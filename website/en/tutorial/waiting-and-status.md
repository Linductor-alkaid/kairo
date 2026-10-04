---
title: Bounded Waiting and Status
description: Finish a pipeline safely using bounded waits, WaitResult, and state snapshots.
---

# Bounded Waiting and Status

## Goal

Before a phase transition or shutdown, wait for asynchronous work within a budget and decide what to do next using `WaitResult`, completion state, and executor state.

```cpp
auto result = executor.wait_for_completion(std::chrono::milliseconds{200});
if (!result.completed) {
    // result.timed_out is true; status is a snapshot captured at timeout.
    std::cerr << "pending=" << result.status.pending_tasks << '\n';
}
```

`WaitResult` contains `completed`, `timed_out`, the requested timeout, and a `CompletionStatus`. On timeout, the Facade records `WaitTimeout`, allowing central handling through failure status and callbacks.

## Choose a wait API

| Purpose | API | Result |
| --- | --- | --- |
| Compatibility with older callers | `wait_for_completion()` | Waits up to the default duration; timeout does not throw. |
| Only need complete/timeout | `try_wait_for_completion(timeout)` | `bool` |
| Express any chrono duration | `wait_for_completion_for(timeout)` | `bool` |
| Diagnose timeout | `wait_for_completion(timeout)` | `WaitResult` plus state snapshot |

`is_idle()` quickly checks whether the default asynchronous executor is currently idle. `get_completion_status()` snapshots initialization, queued, active, and pending counts; use `get_async_executor_status()` when lifecycle state also matters.

## Snapshot scope and correct exit order

`CompletionStatus` covers only this Kairo's default asynchronous executor. It excludes application-created threads, data in communication channels, real-time queues, and external I/O. An entire robot pipeline must aggregate its own status.

Snapshots are momentary: another producer may submit immediately after an idle result. Stop submission first, then wait; never reverse that order.

1. Stop new work, including unneeded periodic tasks.
2. Call `wait_for_completion()` with a business-acceptable budget.
3. If complete, call `shutdown(true)`; otherwise record the snapshot and follow the chosen retry, degradation, or `shutdown(false)` policy.

Timeout does not cancel work. Individual results and exceptions still belong to their futures, while `wait_timeout_count` records wait-timeout trends.

When the timeout may involve realtime, Blocking I/O, or GPU backends as well as
default async work, capture the full Kairo scene before choosing the policy:

```cpp
const auto result = executor.wait_for_completion(std::chrono::milliseconds{200});
if (!result.completed) {
    const auto snapshot = executor.get_snapshot();
    // Persist lifecycle, backend state, failures, and snapshot.partial.
}
```

`get_snapshot()` is best-effort and does not cancel or reserve work. It complements
`WaitResult::status`, whose completion fields remain scoped to default async work.

## Test the decision

Run a task longer than the budget; run one blocking task followed by short tasks on a single worker; leave a producer active while draining; then practice continued waiting, persisting unfinished input, and fast shutdown. Define both a per-request wait budget and a service-wide drain budget before an incident.

For one result, use that future's bounded wait. For dependency groups, retain the graph's final future. For communication or real-time shutdown, query and close the corresponding components: ordinary completion does not include them.

Next: [complete robot pipeline](/en/tutorial/complete-robot-pipeline).
