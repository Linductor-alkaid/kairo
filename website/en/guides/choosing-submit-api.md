---
title: Choose a Submission API
description: Choose by completion, bounded admission, or worker lifecycle before entering default or expert Kairo paths.
---

# Choose a Submission API

Do not start with an executor class name. First ask what the caller must confirm: a piece of work completed, a bounded queue accepted it, or a long-lived worker started and can be managed. That answer matters before a backend's speed. For the full boundary, read [Execution Models and Routing Boundaries](/en/guides/execution-models-and-routing).

## 30-second selection table

| Problem | Default API | Result means | When to read more |
| --- | --- | --- | --- |
| One finite background operation | `submit_auto(lambda)` | Future completion or exception | Priority, delay, batch, or dependencies |
| Independent CPU/GPU implementations | `submit_auto(cpu_gpu_task(...))` | Future completion or exception on selected path | Registration, diagnostics, or GPU tuning |
| Verified lock-free single-consumer path | `dispatch_auto(LowLatency)` | Bounded queue admission | Capacity, object pool, shutdown |
| Existing periodic real-time queue | `dispatch_auto(RealtimeQueue)` | Bounded queue admission | Cycle budget, drops, permissions |
| Long-lived wakeable I/O loop | `start_worker(BlockingWorkerSpec)` | Worker startup and lifecycle | Protocol, reconnect, device deployment |
| Soft periodic maintenance | `submit_periodic()` | Task ID and periodic status | Strict period or low jitter |
| Dependencies, delays, priorities, batches | Matching explicit Facade API | Future, handle, or task ID | Composite scheduling semantics |

`submit_auto(lambda)` is the ordinary developer's default entry. It safely uses the default asynchronous pool; it does not infer that pressure, a deadline, or priority should select GPU, lock-free, or real-time work.

Before choosing by scenario, read [how automatic routing matches a target](/en/guides/execution-models-and-routing) if names, intents, fallback, and capability snapshots are still unclear.

Complex policies should express correctness first. For “after two successful prerequisites, then urgent work,” build the dependency, then decide whether priority is actually needed; queue order is not a dependency mechanism.

```mermaid
flowchart TD
    A{What must the caller confirm?}
    A -- Completion or exception --> B[Future path]
    B --> C[Default: submit_auto(lambda)]
    C --> D{Explicit constraint?}
    D -- priority/delay/batch/dependency --> E[Matching explicit Facade API]
    D -- independent CPU/GPU paths --> F[cpu_gpu_task + submit_auto]
    A -- Queue admission --> G[dispatch_auto]
    G --> H[Name a running LowLatency or RealtimeQueue backend]
    A -- Long-lived worker lifecycle --> I[start_worker]
    I --> J[WorkerHandle lifecycle]
```

The paths are not interchangeable: a ready future does not prove a real-time cycle ran; `DispatchResult::accepted` does not prove execution; and `WorkerHandle::started()` does not prove a device, protocol, or first input is ready.

## Default: `submit_auto(lambda)`

For finite one-off work that needs a result or exception, begin here:

```cpp
auto future = executor.submit_auto([frame] { return decode(frame); });
auto decoded = future.get();
```

Value capture gives the task stable input. `future.get()` is the completion and exception boundary; `get_last_routing_decision()` explains why the default Facade selected its path. `submit()` remains a valid explicit entry when existing thread-pool semantics or compatibility require it. A reference, raw pointer, or `this` must be proven to outlive the task.

## Bounded admission: `dispatch_auto()` only for known constraints

`dispatch_auto()` is not an acceleration switch for ordinary work. Use it only when the application has already verified lock-free single-consumer or real-time periodic-queue semantics:

```cpp
TaskOptions options;
options.intent = ExecutionIntent::RealtimeQueue;
options.preferred_executor = "control";
const auto result = executor.dispatch_auto(options, [] { apply_control(); });
if (!result.accepted) {
    // Inspect result.decision, result.message, failure events, and status counters.
}
```

Stopped backends, full queues, exhausted object pools, and concurrent stopping can reject. There is no silent fallback to the default pool. `accepted` means admission, never completion.

## Long-lived work: `start_worker()`

Permanent listeners, blocking reads, polls, and protocol loops must not occupy the shared pool. When a loop responds to a stop token and `wakeup()` can release its current wait, use `start_worker(BlockingWorkerSpec{...})` and manage startup, `request_stop()`, `stop()`, and status through `WorkerHandle`. See [Blocking I/O workers](/en/realtime-and-communication/blocking-io-workers).

## Priority, delay, and periodic work

`submit_priority()` only changes selection among waiting work. It cannot preempt running low-priority work, guarantee a deadline or completion order, or prevent starvation caused by blocking work. Use a project-wide LOW/NORMAL/HIGH/CRITICAL mapping; if every caller uses critical, priority has no meaning.

`submit_delayed(delay_ms, ...)` means “submit to the ordinary executor no earlier than this relative delay.” It suits retry backoff, deferred cleanup, and debouncing, not precise timing. A busy pool can delay it further. Its future remains the result/exception boundary.

`submit_periodic()` suits health checks, cache refresh, and metrics. Retain its task ID; define cancellation, observation of execution/failure counts, a response to consecutive failures, and behavior when a callback approaches or exceeds its period. It is not a strict-period control API.

## Batch and dependency work

Batch APIs require independent tasks produced together with equivalent scheduling semantics. They can reduce repeated submission-path overhead, but gains depend on task count/body, worker count, hardware, and build; no fixed speedup is promised. Default to `submit_batch()` and consume every future. Consider `submit_batch_no_future()` only if per-item results are unnecessary, service-level failure observation exists, shutdown has a bounded or explicitly lossy policy, and failures can be associated with a business batch/input.

Use `submit_with_handle()`, `submit_after()`, and `when_all()` for “model load → parallel preprocessing → plan.” A failed prerequisite prevents ordinary dependent execution and appears in its future. Do not hide task relations behind `future.get()` inside arbitrary worker lambdas. Current dependent wrappers may wait in the pool, so submit prerequisites first, cap in-flight graphs, and test at the minimum worker count. Use a dedicated graph scheduler for large dynamic DAGs. Handles are valid only in their originating Kairo instance.

## Time budgets are not cancellation

`wait_for_completion_ex(timeout)` reports incomplete work and a snapshot; it does not safely kill a running C++ function. Make I/O bounded, let long work check a stop signal or deadline, and split interruptible work into steps. Likewise, `shutdown(true)` is an orderly-exit policy, not a guarantee that an arbitrary permanent task ends promptly.

Automatic routing does not prove callable real-time safety, thread safety, GPU memory ownership, or I/O interruptibility. `get_executor_capabilities()` is an advisory snapshot, not a backend reservation. For CPU/GPU fallback, declare separate callable paths and a `FallbackPolicy`; an allowed fallback is explained by `RoutingDecision`, not reported as a user-task exception.

Before production, assign an owner to every future/task ID, keep periodic cancellation and failure policies, avoid using priority for correctness, make batch work independent, ensure all tasks are bounded or cooperatively stoppable, and observe queue growth, rejection, and wait timeout. For continuous data between threads, use a [communication component](/en/guides/choosing-communication), not task submission semantics.
