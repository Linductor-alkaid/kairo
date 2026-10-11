---
title: Execution Models and Routing Boundaries
description: Understand the difference between task completion, queue admission, and worker lifecycle when using automatic routing.
---

# Execution Models and Routing Boundaries

Kairo provides one Facade so ordinary developers can start with `submit_auto(lambda)`. The backends do not all report the same kind of result, though. Before using a real-time queue or a long-lived worker, decide what the caller actually needs to know.

## Completion, admission, and worker startup are different

| Model | Entry | What the caller confirms | What it does not confirm |
| --- | --- | --- | --- |
| Completion | `submit_auto()`, `submit()`, CPU/GPU dual-path task | Work completed, or its future contains an exception | A real-time cycle, worker, or other backend has ended |
| Admission | `dispatch_auto()` | The named bounded queue accepted this task | The task ran, no item dropped, or business effect occurred |
| Lifecycle | `start_worker()` | A worker registered/started, or startup failed | Protocol handshake, device availability, or first input |

This keeps callers from waiting on the wrong thing: `wait_for_completion()` waits for ordinary asynchronous futures, not real-time cycles, lock-free queues, long-lived workers, or GPU activity.

## What default automatic routing does

For ordinary work:

```cpp
auto future = executor.submit_auto([] { return transform(); });
auto value = future.get();
```

`Auto` chooses only the default asynchronous backend. It removes the need to find or register a separate executor before a first task, while each choice remains inspectable:

```cpp
const auto decision = executor.get_last_routing_decision();
```

`RoutingDecision` explains what was selected and why from declared intent and a capability snapshot. It is neither a reservation nor a replacement for a future, `DispatchResult`, or status counter.

## Routing does not guess: entry point first, fields second

“Auto” does not inspect a lambda, find the fastest machine, or choose arbitrarily among registered executors. Routing uses the **entry point** and only the `TaskOptions` fields you explicitly declare:

| Call form | Looks up a specialist executor? | Routing inputs | How the target is determined | What never happens |
| --- | --- | --- | --- | --- |
| `submit_auto(lambda)` | No | None; equivalent to `intent = Auto` | Always the default async executor | GPU, deadline, priority, or load never moves it elsewhere |
| `submit_auto(task(lambda).intent(GeneralCpu))` | No | `GeneralCpu`, optional name/priority | Always the default async executor | `preferred_executor` cannot send an ordinary callable to GPU, realtime, or lock-free |
| `submit_auto(cpu_gpu_task(cpu, gpu))` | Yes, GPU only | Dual CPU/GPU paths, GPU name, characteristics, fallback | Explicit GPU name; without one, a candidate exists only when **exactly one** GPU is registered | An ordinary lambda is not treated as GPU work; multiple GPUs are not chosen arbitrarily |
| `dispatch_auto(options, task)` | Yes, bounded backend only | `LowLatency`/`RealtimeQueue` plus `preferred_executor` | Backend category and name must both match exactly | It never scans for any available queue or falls back to the default pool |
| `start_worker(spec)` | No | Worker-spec name, configuration, implementation | Creates and starts that worker | It is not task routing or one-shot completion |

So a target named `control` is not sufficient by itself: use `dispatch_auto()` and declare `RealtimeQueue`. Conversely, adding a name or priority to `submit_auto(lambda)` never moves it to `control`.

## Matching each call step by step

### 1. Ordinary `submit_auto`: no target selection

```cpp
auto future = executor.submit_auto([frame] { return decode(frame); });
```

It records a `RoutingDecision` for `default` with `DefaultPolicy`, then uses the ordinary future path. To name a task or set default-pool priority, use the builder; only `Auto` and `GeneralCpu` are accepted:

```cpp
auto future = executor.submit_auto(
    kairo::task([frame] { return decode(frame); })
        .name("decode-frame")
        .priority(kairo::TaskPriority::HIGH)
        .intent(kairo::ExecutionIntent::GeneralCpu));
```

Changing that intent to `LowLatency`, `RealtimeQueue`, or `BlockingWorker` makes the future ready with an exception. Those protocols need the typed APIs below, rather than guessing from an ordinary callable.

### 2. CPU/GPU `submit_auto`: GPU name, candidate checks, then policy

First provide independent implementations, then declare the GPU target and acceptable fallback:

```cpp
auto work = kairo::cpu_gpu_task(
    [input] { run_cpu(input); },
    [input](void* stream) { run_gpu(input, stream); })
    .name("segmentation")
    .preferred_executor("cuda0")
    .data_size(input.bytes())
    .compute_intensity(3.5F)
    .fallback(kairo::FallbackPolicy::AllowCpu);

auto future = executor.submit_auto(std::move(work));
```

Matching proceeds as follows:

1. With `preferred_executor("cuda0")`, only the GPU named `cuda0` is queried. A missing, unregistered, stopped, GPU-incapable, or known-full target never changes to another GPU.
2. Without a name, a candidate exists only when the registry has **exactly one** GPU; zero or multiple GPUs cannot be resolved automatically.
3. For a submit-capable candidate, `prefer_gpu(true)` chooses GPU first; the current Facade does not record performance samples, so it otherwise uses data-size and compute-intensity thresholds. `GpuScheduler` adaptive history applies only to a scheduler the caller maintains and populates.
4. `RequireRequestedBackend` skips these heuristics and requires the named GPU to be submit-capable; a missing name rejects.
5. `AllowCpu` uses default CPU when GPU is unavailable, stopped, known-full, or rejects the actual submission, recording `fell_back = true`. `NoFallback` makes the future ready with an exception.

> 0.7.0: step 3 describes the default path. With an `AdaptiveScheduler` injected, the `CpuOrGpu` decision can be flipped by completion-latency history (with hysteresis and minimum-sample guards); see [When to Use AdaptiveScheduler](/en/guides/adaptive-scheduling) for applicability and risks.

`preferred_executor` pins a candidate name; it does not guarantee that a task will run on GPU. Stop and capacity may still race real submission, so inspect both the future and routing decision.

### 3. `dispatch_auto`: intent and name must both match

Register and start the target, then provide complete `TaskOptions`:

```cpp
TaskOptions options;
options.name = "publish-telemetry";
options.intent = ExecutionIntent::LowLatency;
options.preferred_executor = "telemetry";

const auto result = executor.dispatch_auto(options, [] { publish(); });
```

The router performs an exact lookup:

| Intent | Backend category | Required target name | Pre-dispatch snapshot checks |
| --- | --- | --- | --- |
| `LowLatency` | `LockFree` | A started lock-free executor | Registered, running, and below known capacity hint |
| `RealtimeQueue` | `Realtime` | A started realtime executor | Registered, running, and below known capacity hint |

Missing names, wrong categories, unregistered/stopped targets, or a full snapshot return `accepted == false` with the reason in `result.decision.reason` / `detail`. Passing snapshot checks still performs a real enqueue; a stop, full queue, or exhausted object pool in that interval also returns `accepted == false` with `Rejected`. Neither case tries another same-kind executor or the default pool.

### 4. `start_worker`: the name identifies lifecycle, not routing

```cpp
BlockingWorkerSpec spec{"serial-rx", config, std::move(worker)};
auto handle = executor.start_worker(std::move(spec));
```

`serial-rx` identifies registration, status, and stop/wake/join lifecycle. It is not a candidate for `submit_auto()` or `dispatch_auto()`. `WorkerHandle::started()` reports startup only, not device, protocol, or first-input readiness.

## Discover before submitting

The control plane can show currently nameable backends through a capability snapshot:

```cpp
for (const auto& capability : executor.get_executor_capabilities()) {
    std::cout << capability.name << " running=" << capability.running
              << " pending=" << capability.pending_work << '\n';
}
```

Use `backend`, `name`, `registered`, `running`, `supports_future_submission`, `supports_bounded_dispatch`, `supports_gpu_kernel`, `pending_work`, and `capacity_hint` for diagnostics or a configuration UI. `pending_work` is currently always `0` for realtime backends because their status API has no instantaneous queue-depth snapshot; assess realtime backlog through drop, queue-full, and cycle-timeout counters. The snapshot is not a reservation: a submission must still handle rejection after it reports availability.

## Read one routing decision

After submission or dispatch, inspect `RoutingDecision`:

```cpp
if (const auto decision = executor.get_last_routing_decision()) {
    log(decision->selected_executor_name, decision->detail);
}
```

- `selected_backend` / `selected_executor_name`: backend and name the router attempted;
- `reason`: default policy, explicit intent, preferred name, GPU heuristic/history, unavailable/stopped/capacity pressure, fallback, or rejection;
- `fell_back`: true only when `AllowCpu` actually used CPU; it is not a user-task failure;
- `detail`: diagnostic matching reason.

The routing decision explains why a path was attempted. A future, `DispatchResult`, and `WorkerHandle` respectively report the actual completion, admission, and lifecycle outcome.

## When to route GPU work

GPU is not implicit acceleration for an ordinary lambda. Use it only when the business operation has independent CPU and GPU implementations:

```cpp
auto future = executor.submit_auto(
    cpu_gpu_task([input] { run_cpu(input); },
                 [input](void* stream) { run_gpu(input, stream); })
        .preferred_executor("cuda0")
        .fallback(FallbackPolicy::AllowCpu));
```

`AllowCpu` records a CPU fallback through `RoutingDecision::fell_back`; `NoFallback` and `RequireRequestedBackend` reject through the future and failure events. Registration, devices, and stream details belong in the [GPU topic](/en/gpu/).

## When to route bounded queues

Lock-free and real-time paths are explicit opt-ins. Use `LowLatency` or `RealtimeQueue` only when a single consumer, bounded backpressure, or periodic consumption is already the business contract, and name a running backend:

```cpp
TaskOptions options;
options.intent = ExecutionIntent::LowLatency;
options.preferred_executor = "telemetry";
auto admission = executor.dispatch_auto(options, [] { publish(); });
```

`accepted == true` means queue admission only. A stopped backend, full queue, exhausted object pool, or shutdown race can reject; none silently falls back to the default thread pool.

## When to create a long-lived worker

Blocking I/O is a wakeable, stoppable loop rather than one callable:

```cpp
BlockingWorkerSpec spec{"serial-rx", config, std::move(worker)};
auto handle = executor.start_worker(std::move(spec));
if (!handle.started()) report(handle.start_result().message);
```

`WorkerHandle::request_stop()` requests stop and wakeup; `WorkerHandle::stop()` also joins. The worker must implement `run(stop_token)` and non-throwing `wakeup()`; see [Blocking I/O workers](/en/realtime-and-communication/blocking-io-workers).

## Capability snapshots are not reservations

`get_executor_capabilities()` is useful for control planes, monitoring, and submission diagnostics. It is advisory: a backend can stop or fill between the snapshot and submission. Use it to explain and preflight, never as a promise that a task will complete.

## Next steps

Return to [Choose a Submission API](/en/guides/choosing-submit-api) for scenario selection. Enter [real-time control](/en/realtime-and-communication/realtime-control), [GPU](/en/gpu/), [Blocking I/O workers](/en/realtime-and-communication/blocking-io-workers), or [Advanced](/en/advanced/) only when the corresponding constraint is explicit.
