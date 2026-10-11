---
title: Versions and Migration
description: Entry points for the development snapshot, releases, and API migration.
---

# Versions and Migration

## Current scope

The latest release record is `v0.7.0`. This site uses that stable version as its baseline while following later `master` development; capabilities without a stable tag are not version promises. This first English edition does not maintain historical versioned sites.

| What to check | Source of truth |
| --- | --- |
| Released versions and breaking changes | [CHANGELOG.md](https://github.com/Linductor-alkaid/kairo/blob/master/CHANGELOG.md) |
| Recommended migrations from older APIs | [MIGRATION.md](https://github.com/Linductor-alkaid/kairo/blob/master/docs/MIGRATION.md) |
| Build options, compilers, and backends | [BUILD.md](https://github.com/Linductor-alkaid/kairo/blob/master/docs/BUILD.md) |
| Complete current signatures | [API.md](https://github.com/Linductor-alkaid/kairo/blob/master/docs/API.md) |

## 0.7.0: Runtime-aware Scheduling

0.7.0 (released 2026-10-10) lands per `docs/design/roadmap_v0.7.md`; everything below is additive:

- **Default path unchanged**: `DefaultScheduler` matches 0.6.1 field by field (the contract test passes unmodified); **no migration is required**, and nothing is paid unless an adaptive scheduler is injected.
- **Routing pipeline**: `DefaultScheduler::route()` is internally restructured into `constraint filter → candidate generation → scoring/selection` stages, exposed as composable components (`<kairo/scheduling_pipeline.hpp>`); the external interface and decisions are unchanged.
- **Feedback aggregation**: `FeedbackAggregator` (`<kairo/feedback_aggregator.hpp>`) accumulates execution-time samples lock-free on worker threads (EWMA, bucketed histograms, failure rate) and periodically merges them into immutable snapshots; `Executor::get_feedback_snapshot()` is the diagnostics entry, and `get_snapshot_text()` gains a `scheduling_feedback.*` section.
- **AdaptiveScheduler (opt-in)**: `<kairo/adaptive_scheduler.hpp>` provides three explainable decisions once injected — history-based CPU/GPU selection (hysteresis + minimum samples), QoS-aware load shedding, and bounded QoS→priority promotion. When to use it and the risks (oscillation, cold start, non-reproducibility) are covered in [When to Use AdaptiveScheduler](/en/guides/adaptive-scheduling).
- **New structured diagnostics**: `RoutingReason::LoadShedding`, the `AdaptiveHistory` / `LoadShedding` diagnostics bits, and three new `SchedulingMetrics` counters (`adaptive_history_count` / `load_shedding_rejected_count` / `priority_promoted_count`).
- Full upgrade notes: the "upgrading from 0.6.x to 0.7.0" section of [MIGRATION.md](https://github.com/Linductor-alkaid/kairo/blob/master/docs/MIGRATION.md).

## 0.6.1: Scheduling Runtime observability and docs governance

v0.6.1 is an additive stabilization release - no breaking changes, no new scheduling strategies:

- **Structured routing decisions**: `RoutingDecision::status` (`Accepted` / `AcceptedDegraded` / `Rejected`) is the authoritative accept/reject check; new reason codes `DeadlineExpired` / `AffinityMismatch` and a `diagnostics` bitmask (`AffinityMismatch`, `ResourceInfeasible`) explain the cause; `detail` stays human-readable. Custom 0.6.0-style schedulers (rejection reason without status) are normalized automatically.
- **Scheduling metrics**: `get_scheduling_metrics()` exposes always-on counters (accepted / degraded / rejected, deadline rejections and misses, affinity mismatches, resource rejections, feedback reports) without touching scheduler internals.
- **Feedback measurement contract**: schedulers overriding `wants_feedback()` receive per-task completion measurements (`queue_wait_ns`, `execution_duration_ns`, backend, deadline miss, failure kind) via `on_task_completed()`. `DefaultScheduler` does not consume feedback; adaptive scheduling is v0.7.0+ scope.
- **Deadline admission locked**: strictly past deadlines reject (equality accepted); misses still execute and record `DeadlineMissed`.
- **Docs governance**: `scripts/check_docs_drift.sh` runs in CI to catch stale naming, dropped APIs, and broken links.

## 0.6.0: renamed to kairo, compatibility layer removed, Scheduling Runtime

v0.6.0 is the breaking-change window with three parts:

- **Rename**: Executor -> kairo (namespace `kairo::`, includes `<kairo/...>`, CMake `KAIRO_*`, package `libkairo`); `Executor` remains the domain class name.
- **Compatibility-layer removal**: the `_ex` variants take over the primary names (`initialize_ex` -> `initialize`) and the old bool/void entries are gone; the timer string-id era (`cancel_task(task_id)`) is replaced by `TimerHandle`; `IRealtimeExecutor::push_task()` now returns `ExecutorResult`. See the migration tables in [MIGRATION.md](https://github.com/Linductor-alkaid/kairo/blob/master/docs/MIGRATION.md).
- **Scheduling Runtime**: scheduling decisions moved behind an injectable `IScheduler`; tasks declare deadline (EDF within a priority class, expired-at-submission rejection, missed -> `DeadlineMissed`), QoS (`BestEffort/Standard/Interactive/Critical` queue-priority preset), affinity (advisory) and resource requirements (feasibility check). Start with [Declare Deadlines, Priorities, and Resources](/en/tutorial/scheduling-runtime); design in `docs/design/scheduling_runtime.md`.


## 0.5.3: review fixes and the event-driven timer

v0.5.3 is a stability-and-performance maintenance release with unchanged public API signatures. It lands all four phases of the 2026-09-30 full code review (9 P0 memory-safety/hang/data-race defects, 24 P1 correctness defects, 8 build/packaging defects, 10 hot-path performance items) and converts the timer thread from 1 kHz polling to event-driven condition waits (idle wait CPU down ~34×, periodic jitter improved 10-27× via grid anchoring). See the "upgrading from 0.5.2 to 0.5.3" section of [MIGRATION.md](https://github.com/Linductor-alkaid/kairo/blob/master/docs/MIGRATION.md) for the two observable timer behavior changes; no code changes are required.

## 0.5.2: dependency-driven scheduling

v0.5.2 moves task-graph dependency waiting to dependency-driven scheduling: `submit_after` dependents no longer enter the pool (and occupy workers) while prerequisites are unresolved. See the "upgrading from 0.5.0 to 0.5.2" section of [MIGRATION.md](https://github.com/Linductor-alkaid/kairo/blob/master/docs/MIGRATION.md) for the parked timeout and shutdown settlement semantics. Public API signatures are unchanged (one new diagnostic, `closure_graveyard_size()`).

## Diagnostic result APIs (endpoint of the `bool` -> `_ex` migration)

Through 0.2.x-0.5.x, diagnostic results shipped as `_ex`-suffixed APIs beside the old `bool`/`void` entries. 0.6.0 completed the convergence: **the `_ex` variants took over the primary names** (`initialize_ex` -> `initialize`) and both the weak versions and the `_ex` spellings were removed; key boundaries now return `ExecutorResult` / `WaitResult` directly. When migrating from 0.5.x, rename per the MIGRATION.md tables; older callers should first follow the per-version sections to reach the 0.5.x shape, then apply the 0.6.0 tables.
## 0.3.1: from backend-first to intent-first

New code begins with `submit_auto(lambda)`, then enters a specialist path only when the business explicitly requires independent CPU/GPU implementations, bounded admission, or a long-lived worker lifecycle:

| Existing style or requirement | 0.3.1 recommended entry | Boundary that remains unchanged |
| --- | --- | --- |
| Ordinary `submit(lambda)` | Gradually adopt `submit_auto(lambda)` | Both return futures; `submit()` remains the explicit default-pool entry. |
| One callable branches on a null CPU/GPU stream | `cpu_gpu_task(cpu, gpu)` plus `submit_auto()` | The legacy four-argument overload remains available in `0.3.x` without implicit fallback. |
| Direct lock-free `push_task()` | Register, start, then use `dispatch_auto(LowLatency)` | `accepted` means admission only; single-consumer and backpressure semantics remain. |
| Direct real-time `push_task()` | Use `dispatch_auto(RealtimeQueue)` after start | `accepted` does not mean a later cycle completed and never falls back to the pool. |
| Register and start an I/O worker separately | `start_worker(BlockingWorkerSpec)` | `WorkerHandle` retains wakeup, stop token, startup timeout, and exit reason. |

Automatic routing does not infer callable real-time safety, thread safety, GPU-memory ownership, or I/O interruptibility. `get_executor_capabilities()` is only an advisory snapshot; each actual submission must still handle stop races and backpressure.

## 0.5.0: task lifecycle semantics, Android phase one, and hot-path performance

0.5.0 keeps the existing public submission API compatible while promoting task-level
cooperative cancellation, cancellable and reschedulable timer handles, the serial
execution context, and total bounded admission; Android CPU-only cross-compilation
lands in phase one; the P1/P2 stages of the 2026-09 performance audit significantly
improve submission throughput and realtime jitter. Release artifacts now include
CI-built Linux amd64 debs (full build inside a CUDA devel container) and a Windows
x64 static library.

| Need | 0.5.0 entry | Boundary you still own |
| --- | --- | --- |
| Cancel a queued or running task | `submit_cancellable*` + `request_task_cancel()` | Cancellation is a cooperative request, not preemption; running tasks must check the injected `StopToken` and return promptly. |
| Cancellable, reschedulable timers | `submit_delayed/periodic_*_with_handle` + `TimerHandle` | Expiry work dispatches to the ordinary pool and does not bind to external event loops (see the interop guide for asio strands). |
| Strict submit-order settlement on one context | `submit_on` / `submit_on_with_handle` | Order only; one long task on a context still delays later tasks. |
| Structured overload rejection | `ExecutorConfig::max_in_flight_tasks` | Defaults to `0` (disabled); at the bound the future completes with `CapacityExhaustedException` and must be handled. |
| Parse status snapshot text | `ExecutorSnapshot` schema 3 | `cancellation`/`timers` fields are additive; parsers asserting column counts must relax. |
| Android CPU-only cross-compilation | NDK r26c/r28b scripts and CI | Thread priority, affinity, `mlockall`, and timer slack stay best-effort; no hard realtime promise. |

Migration notes for `ExecutorSnapshot` schema 2 → 3, the process memory-lock lease,
and the shutdown cleanup of pending delayed tasks are in the
["0.4.0 → 0.5.0"](https://github.com/Linductor-alkaid/kairo/blob/master/docs/MIGRATION.md)
section of MIGRATION.md.

## 0.4.0: fixed synchronization boundaries and communication observability

0.4.0 moves communication synchronization to construction-time fixed storage and atomic state while retaining the main existing call patterns. New code can choose `Topic<T>`, phase-bound LET communication, latency percentiles, and real-time allocation diagnostics by data semantics; none of them proves that an application's whole path is real-time safe.

| Need | 0.4.0 entry | Boundary you still own |
| --- | --- | --- |
| Fan out events independently to ordinary consumers | `comm::Topic<T>` and `TopicSubscription<T>` | Topic uses a mutex and dynamic allocation; it is not a real-time or lock-free data plane. |
| Exchange consistent data only at phase boundaries | Bind `PhaseGate`, `DoubleBuffer`, and `LatestMailbox` to LET phases | One publish is allowed per phase; reads and writes during a transition, or without prior-phase data, are rejected. |
| Assess communication latency trends | Approximate `p50_latency` and `p99_latency` in `CommStats` | Percentiles use a fixed histogram and do not replace end-to-end latency measurement. |
| Detect allocations on a guarded real-time path | `RealtimeAllocationGuard` and `RealtimeThreadConfig::enable_allocation_guard` | Recording requires an enabled Linux build and guarded path; payload work, clocks, page faults, and scheduling still need whole-path measurement. |
| Bound completed task-graph handle retention | `task_graph_retention_capacity` | Active dependencies are not evicted early; an evicted handle explicitly rejects as expired. |
| Adjust thread-pool worker count online | `ThreadPool::resize()` / `ThreadPoolResizer` | Resizing stays inside the initialized range; validate throughput and convergence latency under target load. |

The synchronization core of `MpscChannel`, `RealtimeChannel`, unbound `DoubleBuffer`, `PhaseGate`, and `Sequencer` can be checked with `is_synchronization_lock_free()`. That result covers only component synchronization atomics and fixed storage, not operations on `T`, callbacks, clocks, page faults, caller allocation, or OS scheduling. Prefer non-waiting APIs, disable high-frequency callbacks, and validate the complete path on target hardware when migrating a real-time path.

## Upgrade checklist

1. Read the target version's CHANGELOG and verify that each used capability exists in that tag.
2. Reconfigure and build with the target compiler, operating system, and any GPU or real-time permissions.
3. Keep observation paths for futures, return values, and status counters; use `_ex` at setup boundaries that need diagnosis.
4. Recheck real-time affinity, memory locking, timer slack, GPU backend, driver, and device status.
5. Run tests and tutorial smoke tests, then retest timeout, backpressure, and performance behavior under target load.

Chinese and English guides share the published information architecture. Check [translation status](/translation-status) whenever a new public page or language counterpart is added.
