---
title: Interoperate with an External Event Loop
description: Host an io_context or strand-like loop as a worker, understand which dispatches executor cannot see, and finalize batches with PhaseGate.
---

# Interoperate with an External Event Loop

Your application may already own an event loop — typically asio's `io_context` with `strand`s. Kairo's core library does not depend on asio or any third-party loop; this guide describes how the two cooperate correctly, which dispatches executor cannot observe, and the discipline that keeps the blind spot safe.

The full guide lives in [`docs/external_event_loop_interop.md`](https://github.com/Linductor-alkaid/kairo/blob/master/docs/external_event_loop_interop.md). The compilable companion example (a mutex-and-condvar serial loop that reproduces strand semantics without any SDK dependency) is [`examples/event_loop_interop.cpp`](https://github.com/Linductor-alkaid/kairo/blob/master/examples/event_loop_interop.cpp):

<<< @/../examples/event_loop_interop.cpp{14-40}

## Pattern 1: host the loop as a Blocking I/O worker

Implement `IBlockingIoWorker::run(StopToken)` around `io_context::run()` and `wakeup()` so the stop path can wake the loop. `Executor::start_worker()` then owns the thread lifecycle, naming, and stop ordering, and the loop becomes visible in `BlockingIoExecutorStatus` and `get_snapshot()`.

<<< @/../examples/event_loop_interop.cpp{118-128}

## Pattern 2: strand continuations are legal but invisible

A pool task may post a continuation back to the strand. Know exactly what that means:

- `asio::post(strand, ...)` never passes through an executor submission path — there is no admission decision and no queue accounting.
- Posted callbacks do not appear in `TaskStatistics`, in-flight diagnostics, or `ExecutorSnapshot`.
- Exceptions thrown inside posted callbacks do not enter executor's failure events; asio swallows or terminates on them.

Discipline inside the blind spot: hand state ownership across with a `shared_ptr` and stop touching it from the posting thread; catch exceptions inside the continuation; keep work that needs admission, backpressure, or failure metering on executor submission APIs, posting only lightweight continuations.

## Pattern 3: finalize batches with PhaseGate

Do not poll-and-sleep to detect batch completion. The serial side advances a `comm::PhaseGate` phase after each step; any thread can wait with a timeout:

<<< @/../examples/event_loop_interop.cpp{164-178}

## Cancellation and timers at the boundary

- Tasks that need cooperative cancellation must run through executor APIs (`submit_cancellable` + `StopToken`). Kairo cancellation does not reach inside asio's internal waits.
- Homemade `sleep_until` loops that do not depend on strand ownership can migrate to `submit_delayed_with_handle()` / `submit_periodic_with_handle()` (see [Cancellation and Timers](/en/realtime-and-communication/cancellation-and-timers) and `docs/MIGRATION.md`).
- A timer whose callback and destruction must happen on one strand stays application-managed until external-context timer binding passes review (T2). `SerialExecutionContext` provides FIFO task admission, but does not change a facade timer's execution or destruction context.

## Related reading

- [Cancellation and Timers](/en/realtime-and-communication/cancellation-and-timers) for the cancellation semantics themselves.
- [Blocking I/O Workers](/en/realtime-and-communication/blocking-io-workers) for the full worker contract.
- `docs/external_event_loop_interop.md` for the complete guide with the asio-mapping table.
