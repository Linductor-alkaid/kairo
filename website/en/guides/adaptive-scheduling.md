---
title: When to Use AdaptiveScheduler
description: "Development-snapshot capability of 0.7.0: when feedback-driven CPU/GPU selection, QoS load shedding, and priority promotion apply, and the boundaries of oscillation, cold start, and non-reproducibility."
---

# When to Use AdaptiveScheduler

> **0.7.0 development-snapshot capability (unreleased)**: this page describes behavior that exists on `master` but is not part of any stable tag yet. The stable baseline v0.6.1 has no adaptive scheduling — without injecting an `AdaptiveScheduler`, nothing on this page applies and `DefaultScheduler` matches 0.6.1 field by field.

`AdaptiveScheduler` (`include/kairo/adaptive_scheduler.hpp`) is an `IScheduler` implementation injected through `set_scheduler()`: it first produces the 0.6.1 baseline decision, then applies three explainable feedback-driven decisions on top. Injecting it means turning it on; every decision carries a structured explanation channel.

```cpp
#include <kairo/adaptive_scheduler.hpp>

kairo::AdaptiveSchedulerConfig config;
// queue wait p99 target per QoS class (BestEffort/Standard/Interactive/Critical
// enum order; 0 = class is not evaluated). Everything else keeps defaults.
config.queue_wait_p99_target_ns = {0, 100'000'000, 10'000'000, 2'000'000};
// Must be injected before the first submission (like every custom scheduler).
executor.set_scheduler(
    std::make_unique<kairo::AdaptiveScheduler>(std::move(config)));
```

## The three decisions

| Decision | Trigger | How to explain it |
| --- | --- | --- |
| CPU/GPU history-based selection | `CpuOrGpu` intent; both the CPU and GPU sides have reached `min_samples` (default 8), compared by end-to-end latency EWMA (queue wait + execution) | Decision carries `reason = AdaptiveHistory` plus a diagnostics bit; `detail` holds both numbers. With insufficient samples it falls back to the 0.6.1 heuristic and the decision is field-identical to the baseline |
| QoS-aware load shedding | A class's queue wait p99 exceeds its target for `shed_breach_windows` (default 3) consecutive merge windows | Submissions from **strictly lower** QoS classes get a structured `reason = LoadShedding` rejection; the breached class itself and higher classes are never shed |
| Bounded QoS → priority promotion | The same class stays over target for `promotion_windows` (default 6) consecutive windows | The class's default queue priority rises by one level (capped at CRITICAL); an explicit `priority()` is never overridden |

The default path's extra cost has an acceptance budget (route policy-only paired difference ≤150ns, measured 19-49ns; full data in `docs/performance/m3_adaptive_scheduler_results.md`). Derived state is re-evaluated at most once per `merge_interval` (default 100ms); the submission-side read path pays only atomic loads.

## When to use it

- **The workload is long-lived with persistent asymmetry**: CPU/GPU selection needs ≥8 samples per side before it starts learning, and shedding needs 3 consecutive windows (roughly 300ms at defaults) to confirm a breach. Only workloads that live for seconds or longer with a stable shape can amortize the learning cost.
- **The overload pattern is "low-value traffic crowding out high-value traffic"**: shedding is not extra capacity — it hands low-QoS submitters an early `LoadShedding` rejection so they back off, leaving capacity to high QoS. The precondition is that your callers **handle structured rejections** (back off, degrade, or reroute); otherwise rejection is just earlier failure.
- **QoS classes genuinely tier your business**: all three QoS decisions work along the `BestEffort < Standard < Interactive < Critical` class boundary. With a single QoS class in use, shedding and promotion have nothing to act on.
- **You want an exit hatch back to today**: `set_scheduler(nullptr)` restores `DefaultScheduler` with 0.6.1 behavior; every counter and decision channel is additive.

## When not to use it

- **Short-lived or bursty processes**: during the learning period (cold start) the CPU/GPU decision *is* the heuristic; the process ends before it learns anything.
- **Tests and replays where determinism beats average gains**: adaptive decisions depend on sample arrival ordering, see "Non-reproducibility" below.
- **Callers that cannot respond to rejection**: shedding actively rejects low-QoS submissions; callers without back-off logic will experience it as a new failure mode.
- **Expecting capacity management**: NUMA, cross-pool migration, and utilization-based thread-count adjustment are explicitly out of scope (a second control loop fighting `ThreadPoolResizer`). A genuinely undersized pool needs resizing or upstream shedding, not more learning.

## Three inherent risks

### Cold start

Insufficient samples fall back to the baseline: a feature, not a bug, but it means **the first few hundred tasks after startup see no benefit**. Do not draw A/B conclusions during cold start; the `benchmark_adaptive_scheduler` load-shedding scenario likewise excludes the first 200ms learning window. The right way to shorten learning is lowering `min_samples` / `merge_interval` (at a noise cost), not adding "warm-up special cases".

### Oscillation

A CPU/GPU switch requires the challenger's EWMA to beat the in-use side by more than `hysteresis_margin` (default 25%); opening and closing the shed valve take 3 and 5 consecutive windows respectively, and the closing threshold is 50% of the target (`recovery_ratio`). This hysteresis has "zero flips under alternating load" written into tests (`tests/test_adaptive_scheduler.cpp`), but it cannot prove your load never oscillates on longer horizons — after deployment, alert on `adaptive_history_count` in `get_scheduling_metrics()`; a spike means the policy is changing its mind frequently.

### Non-reproducibility

Two runs of the same program can produce different decision sequences: sample arrival ordering, worker scheduling, and external load all feed the feedback loop. The troubleshooting channels are therefore structured: per-decision causes via `RoutingDecision` (reason / diagnostics / detail), trends via the three new `SchedulingMetrics` counters, state machines via `load_shed_active()` / `priority_promoted()` / `feedback_snapshot()` / `format_state_text()`. When reproducing a bug, suspect the adaptive layer first: rerun with `DefaultScheduler`; if the symptom disappears, freeze the aggregated snapshot to localize it.

## Learning granularity and boundaries

- The learning key is `(backend, executor_name, qos)`: heterogeneous tasks within one QoS class share a key, so the history reflects the mix of their latencies. The MVP does not split by task type.
- Requests with `FallbackPolicy::RequireRequestedBackend` are never flipped by history, and an explicit `priority()` is never overridden by promotion — explicit user constraints always win over history.
- Shedding coexists with `max_in_flight_tasks`: the hard cap is the last total admission valve; shedding is the earlier, more discriminating one.
- The embedded aggregator is independent of the `Executor::get_feedback_snapshot()` diagnostics aggregator; for the diagnostics entry points see [Monitoring and Sampling](/en/reliability/monitoring) and the `scheduling_feedback.*` section of `get_snapshot_text()`.

## Next steps

For the scheduling model basics see [Declare Deadlines, Priorities, and Resources](/en/tutorial/scheduling-runtime); for CPU/GPU submission and fallback see [CPU/GPU Automatic Selection](/en/gpu/automatic-scheduling); for design semantics and acceptance data see `docs/design/scheduling_runtime.md` §8 and `docs/performance/m3_adaptive_scheduler_results.md`.
