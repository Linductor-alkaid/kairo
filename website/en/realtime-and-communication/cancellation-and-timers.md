---
title: Cancellation and Timers
description: Cooperative task cancellation with StopToken and cancellable, reschedulable timer handles — and what cancellation never promises.
---

# Cancellation and Timers

Two capabilities were added to the Facade for long-running work: **cooperative task cancellation** (`submit_cancellable` + `request_task_cancel`) and **timer handles** (`TimerHandle` / `ScopedTimerHandle`). Both are requests, never interruptions.

The runnable walkthrough is [`examples/tutorial/13_cancellation_and_timers.cpp`](https://github.com/Linductor-alkaid/kairo/blob/master/examples/tutorial/13_cancellation_and_timers.cpp):

<<< @/../examples/tutorial/13_cancellation_and_timers.cpp{1-17}

## Three different promises: queued timeout, deadline, cancel request

These three mechanisms are frequently confused. They make different promises:

| Mechanism | What triggers it | What it does | What it never does |
| --- | --- | --- | --- |
| Queued soft timeout (`task_timeout_ms`) | Elapsed time **before a worker starts the task** | The task is skipped; the future receives `TimedOutException`; counted in timeout diagnostics | Interrupt a task that has already started running |
| `TaskOptions::deadline` | Advisory routing hint | Influences routing and diagnostics only | Trigger cancellation or interruption by itself |
| Explicit cancel (`request_task_cancel`) | Your code asks | Queued: the task never runs and the future receives `TaskCancelled(Explicit)`. Running: the task's `StopToken` is set | Preempt a running task or unblock a blocking call that has no wakeup mechanism |

Rule of thumb: the timeout is a pool policy, the deadline is a label, and cancellation is an explicit request that the task must cooperate with.

## Cooperative cancellation semantics

- `submit_cancellable(f)` injects an `kairo::StopToken` as the **first argument** of your callable. The task polls `token.stop_requested()` between work steps.
- Queued cancellation wins a single arbitration point: the task does not execute, the future is satisfied with `TaskCancelled(Explicit)`, dependents see `TaskCancelled(DependencyCancelled)`, and **no failure event is recorded** — cancellation is a lifecycle event, counted separately in `get_cancellation_status()`.
- Cancellation of a running task only sets the token. A task that returns normally afterwards keeps its result; a task that throws `TaskCancelled` after observing the request is classified as cancelled. Throwing `TaskCancelled` *without* a request is still counted as a task failure, so the exception type cannot bypass failure statistics.
- Repeated or stale handles are idempotent: `AlreadyRequested` while running, `AlreadyCompleted` after a terminal state, `NotFound` for unknown handles. None of these write failures.

## Timer handles

`submit_delayed()` and `submit_periodic()` (plus `*_cancellable_*` variants that inject a `StopToken`) return a copyable `TimerHandle`:

- `cancel()` before expiry: `CancelledBeforeDispatch`, the task never runs, the future receives `TaskCancelled(Explicit)`.
- `cancel()` after dispatch: `CancellationRequestedAfterDispatch` — the cancellation continues into the queued or running task instead of pretending it was never dispatched.
- `reschedule_after(ms)` moves the next expiry (for periodic timers it changes the next fire time, not the period); `delay_ms <= 0` returns `InvalidDuration`.
- Destruction does **not** cancel. Wrap the handle in a move-only `ScopedTimerHandle` when you want destructor-cancels.
- On shutdown, pending delayed timers resolve their futures with `TaskCancelled(Shutdown)`; counts are visible in `get_timer_status_summary()`.

## Serialized context dispatch

Use `SerialExecutionContext` with `submit_on(context, fn)` when FIFO work should remain
visible to Kairo admission and monitoring. Dispatch and settlement are split: pool
workers only perform a bounded, non-blocking ticket publish, and the business future is
settled directly on the serial thread — so small multi-worker pools keep making bounded
progress in ticket FIFO order under bursts instead of starving each other. Queued
cancellations, timeouts, and rejections all release the ticket without blocking later
submissions. Shutdown rejects new submissions and drains
accepted work. This adapter does not bind to an asio strand; objects that require strand-
affine execution and destruction remain application-managed.

## What is not promised

- **No preemption**: a task blocked in a syscall or library call without a wakeup mechanism is not interrupted. Cancellation cannot force it to stop.
- **No strand ownership**: facade timers dispatch expiry work to the ordinary thread pool. A timer whose callback and destruction must happen on one external event-loop strand (for example an asio `steady_timer`) stays application-managed — see [Interoperate with an External Event Loop](/en/guides/event-loop-interop).
- **No timer precision contract**: expiry is dispatched to a thread pool; latency depends on load. Measure with `benchmark_timer_precision` rather than assuming a bound.

## Where to read more

- API reference: `docs/API.md` §3.8–3.9 (timers and cancellation).
- Design and arbitration internals: `docs/design/task_cancellation_and_timers.md`.
- Migration guidance, including which homemade timers may move to `TimerHandle`: `docs/MIGRATION.md`.
