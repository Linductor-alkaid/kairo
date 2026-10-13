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
- Health at a glance: `executor.get_scheduling_metrics()` counts decisions (accepted/degraded/rejected, deadline and resource rejections, affinity mismatches). Schedulers overriding `wants_feedback()` receive per-task completion measurements via `on_task_completed()` - `DefaultScheduler` does not consume feedback.

## Adaptive Scheduling (0.7.0; opt-in)

`AdaptiveScheduler` (`include/kairo/adaptive_scheduler.hpp`) is an injectable `IScheduler` that reuses the 0.6.1 baseline routing and adds three explainable feedback-driven decisions: history-based CPU/GPU selection for `CpuOrGpu` intents (minimum samples + hysteresis; hit -> `reason = AdaptiveHistory`, `detail` carries both EWMA numbers), QoS-aware load shedding (a class's queue-wait p99 over target for `shed_breach_windows` consecutive windows rejects strictly lower QoS with `reason = LoadShedding`), and bounded QoS->priority promotion (+1 level, explicit priorities never overridden). Check `RoutingDecision::status` first - shedding rejections are structured, so callers should back off on `Rejected` + `LoadShedding`.

Use it only for long-lived, shape-stable workloads whose callers can respond to rejection; expect no benefit during the learning period (cold start), and treat decision sequences as non-reproducible (troubleshoot via `RoutingDecision`, the three new `SchedulingMetrics` counters, and `load_shed_active()` / `priority_promoted()` / `feedback_snapshot()` / `format_state_text()`). Selection guide: `website/en/guides/adaptive-scheduling.md`. The default path (no injection) is byte-for-byte 0.6.1 behavior.

## Routing Fast Path (0.7.1; dev snapshot)

- **Capability snapshot TTL cache (default on)**: the capability reads behind `submit_auto` CpuOrGpu/affinity routing and `dispatch_auto` go through a 100 ms TTL cache instead of taking the registry locks per submit. Backend registration/start/stop (including facade realtime / blocking-IO start and shutdown) invalidates the cache immediately via the manager `state_epoch`; the TTL only bounds drift of runtime numbers (queue depth, GPU free memory). Decisions are identical with the cache on or off; cache hits are marked with `RoutingDiagnostics::CapabilityCacheHit` (`1u<<7`) and `RoutingDecision::capability_from_cache`. Control: `set_capability_cache_ttl()` (0 disables), `invalidate_capability_cache()`; new `SchedulingMetrics` counters `capability_cache_hit_count` / `capability_cache_miss_count`. The public `get_executor_capabilities()` stays uncached (always fresh).
- **`submit_fast(f, args...)`**: zero-routing submission equivalent to `submit_priority(NORMAL, ...)` with no `RoutingDecision` (no metrics, no recent-decisions entry). Use only for certain CPU-only work with no deadline/QoS/affinity/resource constraints and no routing-observability requirement; otherwise prefer `submit_auto`.

## Deadline Policy (0.7.1; dev snapshot)

`deadline_policy()` (TaskBuilder / `TaskOptions::deadline_policy`) decides what happens when a task starts after its declared deadline - same decision point and strictly-past boundary as the 0.6.1 miss observation, never affecting `DeadlineExpired` admission or EDF ordering:

- `RunAnyway` (default): 0.6.x behavior (runs, records `DeadlineMissed`).
- `DropIfLate`: user code never runs; the future settles with `TaskCancelled(DeadlineMissed)` (counted in `deadline_dropped_count`, not in failure stats - cancellation convention); graph dependents receive `DependencyCancelled`. Only meaningful when the result is worthless once stale.
- `CancelIfLate`: a cooperative stop request is issued before the user code starts (no preemption); poll `kairo::get_stop_token()` (empty token outside task context) and exit promptly.
- Only applies when a `deadline` is declared; `CpuGpuTask` ignores it at this stage. Tracked submissions: `submit_after / submit_after_with_handle` accept a TaskBuilder (deadline/policy/QoS/priority), so a dropped upstream propagates `DependencyCancelled` downstream.

## Integration Pitfalls

- Priority chooses waiting work first; it cannot preempt a task already running. Sustained critical work can starve lower priorities.
- Delayed and periodic APIs are soft scheduling, not realtime deadlines. Periodic callbacks may overlap when execution exceeds the period.
- Keep each periodic TimerHandle and cancel it during service shutdown. Cancellation prevents future ticks but does not erase queued/running callbacks.
- Dependency handles are local to one Executor and terminal handles can expire according to the configured retention capacity.

## Related Guide

`website/en/tutorial/priority.md`, `website/en/tutorial/delayed-and-periodic.md`, `website/en/tutorial/batch.md`, `website/en/tutorial/dependencies.md`, and `website/en/tutorial/scheduling-runtime.md`. Design details: `docs/design/scheduling_runtime.md`.
