# Scheduling Runtime

## Use It When

Search terms: `IScheduler`, `DefaultScheduler`, `set_scheduler`, `SchedulingSpec`, `TaskSchedulingMeta`, `QosClass`, `AffinityHint`, `ResourceRequirements`, EDF, deadline admission, `DeadlineMissed`, scheduling model, scheduling.hpp, scheduler.hpp.

Select this card when a change touches how admission/routing decisions are produced, how the deadline/QoS/affinity/resource model enters scheduling, or how the facade delegates to a scheduler.

## Public Boundary

- `include/kairo/scheduling.hpp`: `QosClass`, `AffinityHint`, `ResourceRequirements`, `SchedulingSpec`, `TaskSchedulingMeta`, `qos_class_to_string`.
- `include/kairo/scheduler.hpp`: `IScheduler` (route + feedback), `DefaultScheduler`, `SchedulingFeedback`.
- `include/kairo/executor.hpp`: `set_scheduler(std::unique_ptr<IScheduler>)`, `get_scheduler()`.
- `include/kairo/task_options.hpp`: `TaskOptions` model fields (`deadline`, `qos`, `affinity`, `resources`, `priority_set`), `default_priority_for_qos`.
- `include/kairo/interfaces.hpp`: `try_submit_priority_task(priority, task, on_timeout, meta)` overload.

## Implementation Trail

- `src/kairo/scheduler.cpp`: `DefaultScheduler::route` - deadline-expiry rejection, GPU device/memory feasibility, `TaskRouter` delegation, `AffinityMismatch` advisory detail.
- `src/kairo/scheduling.cpp`: QoS stable names.
- `src/kairo/task/task.cpp`: `operator<` EDF ordering (`priority -> EDF -> FIFO`).
- `src/kairo/executor.cpp`: `route_task` lazy capability collection (CR-106, extended for affinity), `submit_auto(TaskBuilder)` QoS mapping + deadline-miss wrapper, `submit_priority_scheduled` shared body, `record_failure` DeadlineMissed counter, `set_scheduler`.
- `src/kairo/thread_pool/thread_pool.cpp`: `try_submit_priority(..., meta)` writes `Task::deadline_ns`/`Task::qos`.
- `include/kairo/executor_manager.hpp` / `src/kairo/executor_manager.cpp`: capability snapshot dimensions (`bound_cpus`, `gpu_device`, `gpu_memory_*`).

## Observable Contract

- Ordering hierarchy is `priority -> EDF -> FIFO`; deadline never crosses priority classes (CR-024 strict priority, no aging).
- Expired-at-submission deadline: `RoutingReason::Rejected`, detail "deadline already missed at submission"; failure surfaces through the future.
- Started-after-deadline tasks still run and record `FailureKind::DeadlineMissed` (`deadline_missed_count`); cancellation remains a request, never an interrupt.
- QoS is a queue-priority preset (BestEffort/Standard/Interactive/Critical -> LOW/NORMAL/HIGH/CRITICAL) applied only when the user did not set an explicit priority; no preemption, latency bound, or bandwidth guarantee.
- Affinity hints are advisory: non-intersection lands `AffinityMismatch` in `RoutingDecision.detail`; OS threads are never rebound per task. No exclusive-core field exists until a real reservation mechanism ships.
- Resource declarations are a feasibility filter against capability snapshots: wrong `gpu_device` -> `BackendUnavailable`; memory above availability -> `CapacityPressure`; unknown totals skip the check. TOCTOU is accepted - no reservation in 0.6.0.
- `set_scheduler` must complete before the first submission; `nullptr` restores `DefaultScheduler`. `route()` is called concurrently from submitting threads.

## Change Safeguards

Preserve the hot-path invariants: admission counting precedes the future, queued diagnostics precede enqueue, lazy capability collection (CR-106), parked-generation wakeups (PA-2). Keep the decision/execution boundary: schedulers produce decisions, the facade executes through the per-backend submission protocols. Run `test_scheduling_runtime` (32 cases) plus `test_executor_facade`, `test_task_dependency_manager`, and timer suites.

## Related Material

`docs/design/scheduling_runtime.md` (model, boundaries, evolution roadmap), `docs/API.md` §3.8, `website/en/tutorial/scheduling-runtime.md`.
