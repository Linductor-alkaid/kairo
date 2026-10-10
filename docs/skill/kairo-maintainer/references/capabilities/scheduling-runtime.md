# Scheduling Runtime

## Use It When

Search terms: `IScheduler`, `DefaultScheduler`, `set_scheduler`, `SchedulingSpec`, `TaskSchedulingMeta`, `QosClass`, `AffinityHint`, `ResourceRequirements`, EDF, deadline admission, `DeadlineMissed`, `RoutingStatus`, `SchedulingMetrics`, `SchedulingFeedback`, `wants_feedback`, `AdaptiveScheduler`, `FeedbackAggregator`, `effective_priority_for`, `LoadShedding`, `AdaptiveHistory`, scheduling model, scheduling.hpp, scheduler.hpp, scheduling_pipeline.hpp, feedback_aggregator.hpp, adaptive_scheduler.hpp.

Select this card when a change touches how admission/routing decisions are produced, how the deadline/QoS/affinity/resource model enters scheduling, how the facade delegates to a scheduler, or how scheduling observability (metrics/feedback) is wired.

## Public Boundary

- `include/kairo/scheduling.hpp`: `QosClass`, `AffinityHint`, `ResourceRequirements`, `SchedulingSpec`, `TaskSchedulingMeta`, `qos_class_to_string`.
- `include/kairo/scheduler.hpp`: `IScheduler` (route + `on_task_completed` + `wants_feedback` + 0.7.0 `effective_priority_for` with a pass-through default), `DefaultScheduler`, `SchedulingFeedback` (0.6.1: backend/executor_name/queue_wait_ns/execution_duration_ns/had_deadline/deadline_missed/failure_kind).
- `include/kairo/executor.hpp`: `set_scheduler(std::unique_ptr<IScheduler>)`, `get_scheduler()`, `get_scheduling_metrics()` (0.6.1), `get_feedback_snapshot()` (0.7.0 diagnostics).
- `include/kairo/task_options.hpp`: `TaskOptions` model fields (`deadline`, `qos`, `affinity`, `resources`, `priority_set`), `default_priority_for_qos`, `RoutingStatus`, `RoutingDiagnostics` (0.7.0 adds `AdaptiveHistory` `1u<<2` / `LoadShedding` `1u<<3`), `RoutingReason::LoadShedding` (0.7.0), `routing_status_to_string`, `routing_reason_to_string`.
- `include/kairo/types.hpp`: `SchedulingMetrics` (0.6.1; 0.7.0 adds `adaptive_history_count` / `load_shedding_rejected_count` / `priority_promoted_count`), `FailureKind::None` (0.6.1).
- `include/kairo/interfaces.hpp`: `try_submit_priority_task(priority, task, on_timeout, meta)` overload.
- `include/kairo/scheduling_pipeline.hpp` (0.7.0 M1): composable route pipeline stages (`DeadlineConstraintFilter`, `GpuResourceConstraintFilter`, `ConstraintFilterChain<>`, `IntentCandidateGenerator`, `IdentityScoring`, `select_first`, `apply_affinity_advisory`).
- `include/kairo/feedback_aggregator.hpp` (0.7.0 M2): `FeedbackAggregator` / `FeedbackAggregatorConfig` / `FeedbackSnapshot`, helpers `find_entry` / `histogram_quantile_ns`.
- `include/kairo/adaptive_scheduler.hpp` (0.7.0 M3): `AdaptiveScheduler` / `AdaptiveSchedulerConfig` (opt-in; `load_shed_active`, `priority_promoted`, `feedback_snapshot`, `format_state_text` diagnostics).

## Implementation Trail

- `src/kairo/scheduler.cpp`: `DefaultScheduler::route` - deadline-expiry rejection (`DeadlineExpired`), GPU device/memory feasibility (`ResourceInfeasible` diagnostics bit), `TaskRouter` delegation, structured `AffinityMismatch` degradation; 0.7.0 M1 delegates to `SchedulingPipeline<IdentityScoring>` (decisions unchanged, contract test passes unmodified).
- `src/kairo/scheduling.cpp`: QoS stable names.
- `src/kairo/scheduling_pipeline.cpp` (0.7.0 M1): out-of-line pipeline stage functions; hot path folds into the explicitly instantiated `SchedulingPipeline<IdentityScoring>`.
- `src/kairo/feedback_aggregator.cpp` (0.7.0 M2): per-worker 32-shard lock-free record, periodic merge into immutable snapshots published by atomic `shared_ptr` swap (RCU-style).
- `src/kairo/adaptive_scheduler.cpp` (0.7.0 M3): baseline pipeline + embedded aggregator; CPU/GPU history override, window-differential load-shed / promotion state machines (symmetric hysteresis, fail-open on insufficient evidence), derived state published as two atomic masks.
- `src/kairo/task/task.cpp`: `operator<` EDF ordering (`priority -> EDF -> FIFO`).
- `src/kairo/executor*.cpp`: `route_task` lazy capability collection (CR-106, extended for affinity) + 0.6.0-style rejection normalization (0.7.0 adds `LoadShedding`), `submit_auto(TaskBuilder)` QoS mapping + deadline-miss wrapper + optional feedback measurement wrapper + `effective_priority_for` consultation (0.7.0), `submit_auto(CpuGpuTask)` dual-side measurement wrappers + AdaptiveHistory exemption from the heuristic CPU-rejection rule (0.7.0), `submit_priority_scheduled` shared body, `record_failure` DeadlineMissed counter, `record_routing_decision` unconditional `count_routing_metrics`, `set_scheduler` (caches `wants_feedback`), `report_scheduling_feedback`, `get_scheduling_metrics`, `get_feedback_snapshot` (0.7.0).
- `src/kairo/thread_pool/thread_pool.cpp`: `try_submit_priority(..., meta)` writes `Task::deadline_ns`/`Task::qos`.
- `include/kairo/executor_manager.hpp` / `src/kairo/executor_manager.cpp`: capability snapshot dimensions (`bound_cpus`, `gpu_device`, `gpu_memory_*`).

## Observable Contract

- Ordering hierarchy is `priority -> EDF -> FIFO`; deadline never crosses priority classes (CR-024 strict priority, no aging).
- `RoutingDecision::status` (0.6.1: Accepted/AcceptedDegraded/Rejected) is the authoritative accept/reject check; `reason` explains the cause, `diagnostics` carries secondary flags, `detail` stays human-readable.
- Expired-at-submission deadline (strictly past, equality accepted): `status = Rejected` + `reason = RoutingReason::DeadlineExpired` (0.6.1; was generic `Rejected`), detail "deadline already missed at submission"; failure surfaces through the future.
- Started-after-deadline tasks still run and record `FailureKind::DeadlineMissed` (`deadline_missed_count` in both failure status and `SchedulingMetrics`); cancellation remains a request, never an interrupt.
- QoS is a queue-priority preset (BestEffort/Standard/Interactive/Critical -> LOW/NORMAL/HIGH/CRITICAL) applied only when the user did not set an explicit priority; no preemption, latency bound, or bandwidth guarantee.
- Affinity hints are advisory: non-intersection accepts the task with `status = AcceptedDegraded`, `reason = AffinityMismatch` (first degradation cause; an earlier fallback keeps `FallbackPolicy`), and the `AffinityMismatch` diagnostics bit; OS threads are never rebound per task. No exclusive-core field exists until a real reservation mechanism ships.
- Resource declarations are a feasibility filter against capability snapshots: wrong `gpu_device` -> `BackendUnavailable`; memory above availability -> `CapacityPressure`; both carry `ResourceInfeasible`; unknown totals skip the check (permissive fallback). TOCTOU is accepted - no reservation layer; admission feasibility is not an execution guarantee.
- `SchedulingMetrics` counts routing decisions (not tasks; a fallback re-route appends a decision) via relaxed atomics in `record_routing_decision`, unaffected by the CR-106 observation switch; counters share one cache line.
- Feedback (0.6.1): only schedulers with `wants_feedback() == true` get `on_task_completed(SchedulingFeedback)` on the worker thread (exceptions isolated, still counted); covers tasks that actually start executing on the default async pool - pre-execution rejections/timeouts are covered by decisions and metrics instead. `DefaultScheduler` does not consume feedback; 0.6.x had no adaptive behavior.
- Adaptive scheduling (0.7.0 development snapshot, opt-in): only an injected `AdaptiveScheduler` learns. CPU/GPU selection needs both sides at `min_samples` and a `(1 - hysteresis_margin)` edge; shedding/promotion evaluate window *differentials* of cumulative histograms (fail-open on insufficient samples, symmetric breach/close hysteresis); `RequireRequestedBackend` and explicit `priority_set` are never overridden. Every adaptive decision is recoverable from `RoutingDecision` (reason + diagnostics + detail), the three new `SchedulingMetrics` counters, and the scheduler's diagnostic accessors.
- `set_scheduler` must complete before the first submission; `nullptr` restores `DefaultScheduler`. `route()` is called concurrently from submitting threads.

## Change Safeguards

Preserve the hot-path invariants: admission counting precedes the future, queued diagnostics precede enqueue, lazy capability collection (CR-106), parked-generation wakeups (PA-2). Metrics counting must stay allocation/lock-free (relaxed atomics only) and must not be gated by the routing-observation switch. Keep the decision/execution boundary: schedulers produce decisions, the facade executes through the per-backend submission protocols. Do not add adaptive scheduling or reservation semantics in 0.6.x patch releases; on 0.7.0+, keep adaptive decisions opt-in, explainable (`RoutingReason`/diagnostics/metrics, never `detail`-only), and budgeted before implementation (`tests/benchmark_adaptive_scheduler.cpp` acceptance lines). Run `test_scheduling_runtime` + `test_scheduling_contract_v061` (must pass unmodified) plus `test_adaptive_scheduler`, `test_feedback_aggregator`, `test_scheduling_pipeline`, `test_executor_facade`, `test_task_dependency_manager`, timer suites, and `scripts/check_docs_drift.sh`.

## Related Material

`docs/design/scheduling_runtime.md` (model, boundaries, 0.6.1 observability contract, §7 feedback aggregation, §8 AdaptiveScheduler semantics, evolution roadmap), `docs/API.md` §3.8, `website/en/tutorial/scheduling-runtime.md`, `website/en/guides/adaptive-scheduling.md` (selection guide), `tests/benchmark_scheduling_paths.cpp` and `tests/benchmark_adaptive_scheduler.cpp` (scheduling-path baselines), `docs/performance/m3_adaptive_scheduler_results.md`.
