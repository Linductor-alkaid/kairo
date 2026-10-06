# Scheduling

## Use It For

Priority, delayed retry, periodic maintenance, batches, dependencies between finite tasks, and per-task scheduling declarations (deadline, QoS, affinity, resource requirements) with structured, observable routing decisions (0.6.0/0.6.1).

## Minimal Usage

```cpp
auto urgent = executor.submit_priority(3, [] { apply_command(); });
auto retry = executor.submit_delayed(250, [] { retry_request(); });
auto health = executor.submit_periodic(1000, [] { check_health(); });

urgent.get();
retry.future.get();
health.cancel();
```

Use `submit_with_handle()`, `submit_after()`, and `when_all()` only when a task must wait for dependencies from the same `Executor` instance. Use `submit_batch()` when results for each item matter, otherwise benchmark `submit_batch_no_future()` before assuming a benefit.

## Scheduling Runtime (0.6.0; observability since 0.6.1)

```cpp
#include <kairo/scheduling.hpp>
#include <kairo/scheduler.hpp>

auto done = std::chrono::steady_clock::now() + std::chrono::seconds(1);
auto result = executor.submit_auto(
    kairo::task([] { return infer(); })
        .name("inference")
        .qos(kairo::QosClass::Interactive)     // queue-priority preset (no explicit priority set)
        .deadline(done)                        // EDF within same priority; miss -> DeadlineMissed
        .affinity(kairo::AffinityHint{{0, 1}}) // advisory; mismatch -> AcceptedDegraded + diagnostic
        .resources(kairo::ResourceRequirements{.memory_bytes = 1u << 28,
                                               .gpu_device = 0}));

// Inject a custom scheduler before the first submission (nullptr restores default):
executor.set_scheduler(std::make_unique<MyScheduler>());
```

Semantic boundaries:

- Ordering hierarchy is `priority -> EDF -> FIFO`. Deadline never crosses priority classes.
- `RoutingDecision::status` (0.6.1: `Accepted`/`AcceptedDegraded`/`Rejected`) is the authoritative accept/reject check - do not enumerate `reason` values or parse `detail`; `reason` (`DeadlineExpired`, `BackendUnavailable`, ...) plus the `diagnostics` bitmask (`AffinityMismatch`, `ResourceInfeasible`) explain why.
- Strictly expired-at-submission deadlines reject (`DeadlineExpired`); a task starting after its deadline still runs and records `FailureKind::DeadlineMissed` (visible via `get_failure_status().deadline_missed_count` and `get_scheduling_metrics().deadline_missed_count`).
- QoS is a priority preset: `BestEffort/Standard/Interactive/Critical -> LOW/NORMAL/HIGH/CRITICAL`. No preemption, latency bound, or bandwidth guarantee; deterministic cycles need the RealtimeQueue intent.
- Affinity hints are advisory (no OS thread rebinding): mismatch still accepts the task as `AcceptedDegraded`. Resource declarations are a feasibility check against capability snapshots (TOCTOU possible) - unfit requests reject with `BackendUnavailable`/`CapacityPressure` plus the `ResourceInfeasible` bit instead of degrading silently.
- Health at a glance: `executor.get_scheduling_metrics()` counts decisions (accepted/degraded/rejected, deadline and resource rejections, affinity mismatches). Schedulers overriding `wants_feedback()` receive per-task completion measurements via `on_task_completed()` - `DefaultScheduler` does not consume feedback, and 0.6.x has no adaptive behavior.

## Integration Pitfalls

- Priority chooses waiting work first; it cannot preempt a task already running. Sustained critical work can starve lower priorities.
- Delayed and periodic APIs are soft scheduling, not realtime deadlines. Periodic callbacks may overlap when execution exceeds the period.
- Keep each periodic TimerHandle and cancel it during service shutdown. Cancellation prevents future ticks but does not erase queued/running callbacks.
- Dependency handles are local to one Executor and terminal handles can expire according to the configured retention capacity.

## Related Guide

`website/en/tutorial/priority.md`, `website/en/tutorial/delayed-and-periodic.md`, `website/en/tutorial/batch.md`, `website/en/tutorial/dependencies.md`, and `website/en/tutorial/scheduling-runtime.md`. Design details: `docs/design/scheduling_runtime.md`.
