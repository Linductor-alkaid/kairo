---
title: Prioritize Control Commands
description: Use submit_auto(TaskBuilder) priority() to queue a small amount of control work ahead of ordinary analysis.
---

# Prioritize Control Commands

## Goal

Queue urgent `ControlCommand` work before ordinary frame analysis in the same robot pipeline, while retaining result and exception observation through futures.

## Recommended approach

Priority is a scheduling dimension, expressed through `submit_auto(TaskBuilder)` with `priority()` (`submit_priority` was removed in 0.7.2). Priority takes the `TaskPriority` enum: `LOW`, `NORMAL`, `HIGH`, `CRITICAL`; ordinary work uses the default priority.

<<< @/../examples/tutorial/02_priority.cpp{6-7,9-26,28-30}

```bash
./build/examples/tutorial/tutorial_02_priority
```

Expected output:

```text
priority tasks=analysis,control
```

`submit_auto(TaskBuilder)` still returns a future. Priority only changes how waiting work is selected: it cannot preempt a low-priority task already running. An explicit `priority()` is never overridden by the scheduler; the QoS-based default only applies when priority is not set. Multiple workers, running work, and same-priority FIFO all affect observed completion order, so this output is not a scheduling proof.

## Inputs, ownership, and failure

`TaskBuilder` carries a single callable. Bind parameters by capturing them in a lambda or with `std::bind`:

```cpp
ControlCommand command = read_command();
auto applied = executor.submit_auto(
    kairo::TaskBuilder([command] { apply_control_command(command); })
        .priority(kairo::TaskPriority::CRITICAL));
```

`command` is saved by value. Increasing priority does not make a raw `this` pointer, a borrowed pointer, or shared mutable state safe. Define one application-wide priority vocabulary using the four enum values (`LOW`, `NORMAL`, `HIGH`, `CRITICAL` — integer values 0-3, matching the old `submit_priority` integers, which 0.7.2 removed). A constant stream of critical work can starve ordinary work.

Keep the returned futures. Empty work and submission after shutdown produce observable rejection or exception results; handle an execution failure at that task's `future.get()`.

## Test the boundary

1. Start a blocking LOW task, then submit CRITICAL work: the latter cannot preempt work already running.
2. Throw from the control task: its future and failure status observe it independently of analysis success.
3. Fill a small queue with critical work: observe rejection, queue depth, and ordinary-task wait time.
4. During shutdown, stop producers, consume held futures, then drain Kairo within a budget.

If a command must take effect within a fixed time, measure queue latency and define timeout/degradation behavior. Critical priority alone is not an acceptance criterion. For fixed periods or jitter budgets, use the dedicated real-time path rather than pool priority.

## Migrating from submit_priority (removed in 0.7.2)

Replace `executor.submit_priority(P, fn)` with
`executor.submit_auto(kairo::TaskBuilder(fn).priority(P'))`; the scheduling order (priority → EDF → FIFO) is bit-for-bit identical. Old calls with bound arguments follow the capture/`std::bind` pattern above. See [version and migration](/en/reference/version-and-migration) and `docs/MIGRATION.md` (0.7.2 section).

Next: [delayed retry and health checks](/en/tutorial/delayed-and-periodic).
