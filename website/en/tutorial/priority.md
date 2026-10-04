---
title: Prioritize Control Commands
description: Use submit_priority to queue a small amount of control work ahead of ordinary analysis.
---

# Prioritize Control Commands

## Goal

Queue urgent `ControlCommand` work before ordinary frame analysis in the same robot pipeline, while retaining result and exception observation through futures.

## Recommended approach

Use `submit_priority(priority, task)`. Priority ranges from `0` (low) to `3` (critical); ordinary work uses the default priority.

<<< @/../examples/tutorial/02_priority.cpp{4,7-19,21-24}

```bash
./build/examples/tutorial/tutorial_02_priority
```

Expected output:

```text
priority tasks=analysis,control
```

`submit_priority()` still returns a future. Priority only changes how waiting work is selected: it cannot preempt a low-priority task already running. Multiple workers, running work, and same-priority FIFO all affect observed completion order, so this output is not a scheduling proof.

## Inputs, ownership, and failure

The full form is `submit_priority(priority, fn, args...)`; only the first argument is scheduling metadata. Callable and argument lifetime rules are identical to `submit(fn, args...)`.

```cpp
ControlCommand command = read_command();
auto applied = executor.submit_priority(3, apply_control_command, command);
```

`command` is saved by value. Increasing priority does not make a raw `this` pointer, a borrowed pointer, or shared mutable state safe. Define one application-wide priority vocabulary: `<= 0` is LOW, `1` NORMAL, `2` HIGH, and `>= 3` CRITICAL. A constant stream of critical work can starve ordinary work.

Keep the returned futures. Empty work and submission after shutdown produce observable rejection or exception results; handle an execution failure at that task's `future.get()`.

## Test the boundary

1. Start a blocking LOW task, then submit CRITICAL work: the latter cannot preempt work already running.
2. Throw from the control task: its future and failure status observe it independently of analysis success.
3. Fill a small queue with critical work: observe rejection, queue depth, and ordinary-task wait time.
4. During shutdown, stop producers, consume held futures, then drain Kairo within a budget.

If a command must take effect within a fixed time, measure queue latency and define timeout/degradation behavior. Critical priority alone is not an acceptance criterion. For fixed periods or jitter budgets, use the dedicated real-time path rather than pool priority.

Next: [delayed retry and health checks](/en/tutorial/delayed-and-periodic).
