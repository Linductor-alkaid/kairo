---
title: Declare Deadlines, Priorities, and Resources
description: Use the 0.6.0 scheduling runtime to declare QoS, deadline, affinity, and resource requirements per task, and to inject a custom scheduler.
---

# Declare Deadlines, Priorities, and Resources

## Goal

Go beyond picking a queue slot manually: declare what a task needs (a priority class, a completion deadline, CPU preferences, GPU resources) and let the scheduling runtime turn those declarations into admission and routing decisions.

## Recommended approach

Build the task with `kairo::task(...)` and chain scheduling declarations, then submit through `submit_auto()`. Ordering hierarchy is fixed: `priority -> EDF -> FIFO`.

<<< @/../examples/tutorial/14_scheduling_runtime.cpp{1-11,13-31}

```bash
./build/examples/tutorial/tutorial_14_scheduling_runtime
```

Expected output:

```text
qos task result=42
expired deadline rejected=yes
feasible deadline result=7
custom scheduler routes=1
```

## What each declaration means

- `qos(QosClass::Interactive)` - a queue-priority preset (`BestEffort/Standard/Interactive/Critical` -> `LOW/NORMAL/HIGH/CRITICAL`) applied when no explicit `priority()` is set. It is not a latency or bandwidth guarantee; deterministic cycles still require the RealtimeQueue intent.
- `deadline(time_point)` - real scheduling input, not a timeout. Within the same priority class, tasks order by earliest deadline first (EDF). A deadline already strictly in the past is rejected at submission with the structured reason `RoutingReason::DeadlineExpired` (0.6.1). A task that starts after its deadline still runs and records `FailureKind::DeadlineMissed` - cancellation remains a request, never an interrupt.
- `affinity(AffinityHint{{0, 1}})` - advisory. The scheduler compares the request against each backend's bound CPU set; on mismatch the task is still accepted, but the decision is marked degraded (0.6.1): `status = AcceptedDegraded`, `reason = AffinityMismatch`, plus the `AffinityMismatch` diagnostics bit and a human-readable detail. OS threads are never rebound per task.
- `resources(ResourceRequirements{...})` - a feasibility check against capability snapshots: a wrong `gpu_device` rejects with `BackendUnavailable`, memory above availability rejects with `CapacityPressure` (both carry the `ResourceInfeasible` diagnostics bit, 0.6.1). It is not a reservation; concurrent submissions can still compete at execution time.

## Injecting a custom scheduler

Scheduling decisions live behind `IScheduler` (`<kairo/scheduler.hpp>`). `DefaultScheduler` composes intent routing with the model constraints above; `executor.set_scheduler(std::make_unique<MyScheduler>())` replaces it (before the first submission; pass `nullptr` to restore). Schedulers only produce decisions - the facade still executes submissions through each backend's own protocol.

Every decision carries a machine-readable outcome (0.6.1): `RoutingDecision::status` (`Accepted` / `AcceptedDegraded` / `Rejected`) is the authoritative accept/reject check, `reason` and the `diagnostics` bitmask explain why, and `detail` stays human-readable. Aggregate health is available without touching scheduler internals via `get_scheduling_metrics()` (accepted/degraded/rejected counts, deadline rejections, misses, affinity mismatches, resource rejections). A scheduler that overrides `wants_feedback()` to true additionally receives per-task completion measurements (`queue_wait_ns`, `execution_duration_ns`, backend used, deadline miss, failure kind) through `on_task_completed()`; `DefaultScheduler` does not consume feedback.

## Inputs, ownership, and failure

All declarations are plain values on `TaskOptions`; nothing keeps references to your objects. The expired-deadline rejection arrives through the task's future as a `std::runtime_error` containing "deadline", and `get_failure_status().submit_rejected_count` increases. A missed deadline (observed at execution start) increments `deadline_missed_count` instead of rejecting.

## Check your understanding

1. A CRITICAL-priority stream saturates the pool. Does a NORMAL task with a very near deadline still run on time?
   No - ordering is priority first, EDF second. Deadlines never cross priority classes.
2. Does `qos(QosClass::Critical)` make a pool task real-time?
   No. It only moves the task to the CRITICAL queue slot. Real-time cycles need `submit_realtime`-style dedicated threads (see the realtime tutorial).
3. Two tasks both declare 100 MB of GPU memory while 100 MB is free. Do both get it?
   The check is a feasibility snapshot, not a reservation - both may pass admission and compete later.

## Related pages

- [Prioritize Control Commands](/en/tutorial/priority) - manual priority slots
- [Delayed Retry and Health Checks](/en/tutorial/delayed-and-periodic) - timer handles
- [When to Use AdaptiveScheduler](/en/guides/adaptive-scheduling) - 0.7.0: feedback-driven CPU/GPU selection, shedding, and promotion
- [Versions and Migration](/en/reference/version-and-migration) - 0.6.0 changes
