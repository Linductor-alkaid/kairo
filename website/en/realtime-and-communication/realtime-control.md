---
title: Dedicated Real-Time Control Loop
description: Register, diagnose, start, push to, and stop a dedicated periodic thread through the Kairo Facade.
---

# Dedicated Real-Time Control Loop

## Goal

Starting from a fixed-period CAN or control-loop requirement, use `register_realtime_task_ex()`, `start_realtime_task_ex()`, `try_push_realtime_task()`, and status queries to establish a minimal diagnosable path.

This is an expert topic: ordinary finite work continues to use `submit_auto(lambda)`. Register a dedicated real-time thread only when fixed periods, a cycle budget, and bounded backpressure are already required. For one item sent to a running real-time queue through the unified control plane, use `dispatch_auto(RealtimeQueue)`; it reports admission, not completion.

## When a dedicated thread is needed

`submit_periodic()` fits health checks, refresh work, and background work that tolerates jitter. A control loop needing a fixed period, cycle budget, priority, or CPU affinity needs a dedicated real-time thread. It remains constrained by OS scheduling, permissions, and hardware; it is not an absolute deadline guarantee.

A long-lived blocking wait is neither of these paths. Keep it out of `cycle_callback` and use a [blocking I/O worker](/en/realtime-and-communication/blocking-io-workers) with an explicit wakeup contract.

## Recommended path

The tutorial disables memory-lock and timer-slack requests so a non-privileged environment can validate the basic path:

<<< @/../examples/tutorial/07_realtime.cpp{1-45}

```bash
./build/examples/tutorial/tutorial_07_realtime
```

```text
realtime started=yes, command=queued, cycles=observed, command ran=yes
```

## Lifecycle and queue

1. Create a minimal `RealtimeThreadConfig`: name, period, and `cycle_callback`.
2. Register and start with `_ex` APIs; inspect `ExecutorResult::error_code` and `message` on failure.
3. Submit ordinary control work with `push_realtime_task()` or `try_push_realtime_task()`; `false` means it was not queued.
4. Inspect `get_realtime_executor_status()` and `get_realtime_task_list()`, then call `stop_realtime_task()`.

A real-time queue is bounded. Successful enqueue only means a later cycle may process the item; it does not mean completion. `max_tasks_per_cycle` defaults to `64`, leaving excess work for later cycles to protect the period. After a cycle timeout, missed ticks are skipped and timing is rephased from the current time to avoid a catch-up jitter storm; inspect `cycle_timeout_count`. Emergency stop must use the application's safety/hardware bypass, not wait for this queue.

When submitting through the unified control plane, name the target and intent:

```cpp
TaskOptions options;
options.intent = ExecutionIntent::RealtimeQueue;
options.preferred_executor = "control";
auto admission = executor.dispatch_auto(options, [] { apply_control(); });
```

`admission.accepted` has the same meaning as `try_push_realtime_task()` returning `true`: queue admission only. A stopped backend, full queue, exhausted object pool, or shutdown race rejects; none falls back to the default pool.

`RealtimeQueue` and `preferred_executor` are both required; they match only the named, started realtime backend. See [how automatic routing matches a target](/en/guides/execution-models-and-routing) for every check and rejection branch.

## Bind inputs before the real-time path

Both paths accept parameterless, resultless `void()` callables:

| Entry | Invocation | Bind input | Observe completion |
| --- | --- | --- | --- |
| `config.cycle_callback` | Every fixed cycle | Capture long-lived state in a lambda before registration | `cycle_count`, timeout, application state |
| `try_push_realtime_task(name, task)` | Bounded consumption in a later cycle | Capture command input in a lambda at push | Return value only says enqueued; use status counters for execution |

```cpp
auto controller = std::make_shared<Controller>(config_snapshot);
config.cycle_callback = [controller] { controller->run_cycle(); };

ControlCommand command = read_command();
const bool queued = executor.try_push_realtime_task(
    "control", [controller, command] { controller->apply(command); });
```

There is no `try_push_realtime_task(name, fn, args...)` overload and no per-item future. Inputs must already be bound into a copyable `std::function<void()>`. Do not borrow the pushing thread's stack, allocate large objects, block, or lock an ordinary mutex in the callback. Prepare inputs off the real-time thread and pass small values, stable handles, or preallocated objects.

Objects captured by `cycle_callback` must outlive `stop_realtime_task()`. Dynamically queued task captures must outlive consumption or cleanup. `shared_ptr` solves only lifetime; reference-counting, destruction placement, and internal locks still require target-hardware measurement.

## Configuration and fallback

Defaults attempt real-time priority, CPU affinity, and low timer slack. Process-wide memory locking is disabled by default: Linux `mlockall` locks the whole process and future mappings, so enable `enable_process_memory_lock` only after sizing the process memory budget. Linux `SCHED_FIFO`, `mlockall`, container cpusets, and Windows scheduling capability can be limited by deployment permissions. The library continues safely, but that does not mean a requested setting took effect.

At deployment, inspect `RealtimeExecutorStatus`: `priority_applied`, `cpu_affinity_applied`, `process_memory_lock_applied`, `process_memory_lock_errno`, and `timer_slack_applied`; alert alongside `cycle_timeout_count`, `dropped_task_count`, `queue_full_count`, and `pool_exhausted_count`. Empty affinity enables adaptive choice; verify any explicit configuration in its target environment.

Next: [deliver every message](/en/realtime-and-communication/channels).
