---
title: Blocking I/O Workers
description: Own, wake, and join a long-lived blocking worker through the Kairo Facade.
---

# Blocking I/O Workers

## Use this path for a long-lived wait

Use `start_worker(BlockingWorkerSpec)` when a component owns one long-lived loop that can block and must still stop cleanly. It returns a `WorkerHandle` for startup and lifecycle management; it is not a task queue, a real-time control loop, a protocol adapter, or a device-integration framework. The explicit `BlockingIoExecutor` APIs remain available for incremental migration and diagnostics.

Use the thread pool for finite queueable work. Use a dedicated real-time thread for fixed-period control. The worker's protocol, inputs, outputs, retry policy, and safety behavior remain the library consumer's responsibility.

## The worker contract

Implement `IBlockingIoWorker::run(stop_token)` and `wakeup()`.

- `run()` may wait, but must return after a stop request becomes observable.
- `wakeup()` must release the current wait, may be called repeatedly, and must not throw.
- A stop token alone does not interrupt an arbitrary external wait. If the wait primitive cannot be awakened directly, use a bounded timeout and check the token after every return.

The Facade owns the worker after registration. It starts the dedicated thread, calls `wakeup()` during stopping, and joins before releasing the worker.

## Runnable mock worker

The tutorial uses a condition variable only to demonstrate the lifecycle without a protocol or hardware dependency:

<<< @/../examples/tutorial/12_blocking_io_worker.cpp{1-78}

```bash
./build/examples/tutorial/tutorial_12_blocking_io_worker
```

```text
blocking worker started=yes, stopped=yes, wakeups=1
```

## Lifecycle and status

1. Configure a nonempty `BlockingIoConfig::thread_name` and pass a `std::unique_ptr<IBlockingIoWorker>` in `BlockingWorkerSpec` to `start_worker()`.
2. Inspect `WorkerHandle::start_result()` when startup diagnostics are needed; `WorkerHandle::started()` is its convenience success check. The explicit register/start APIs remain available for incremental migration.
3. Observe `WorkerHandle::status()` (or `get_blocking_io_worker_status(name)`). `ready` describes executor-thread setup only; it does not mean a protocol, device, or first input is ready.
4. Call `WorkerHandle::request_stop()` to wake a blocked worker without joining, or `WorkerHandle::stop()` to request stop, wake it, and join. Repeated calls are safe.

`Executor::shutdown()` applies the same stop/wake/join rule to every registered I/O worker, including `shutdown(false)`. Do not detach a worker or retain references to it after shutdown.

## What remains outside Kairo

This library deliberately does not decide message ownership, queue policy, data freshness, reconnect behavior, device safety actions, or deployment tuning. Define and test those concerns in the application that implements the worker.

For complete signatures and status fields, see the [API reference](https://github.com/Linductor-alkaid/kairo/blob/master/docs/API.md#45-blocking-io-worker-api). Next: return to [real-time control](/en/realtime-and-communication/realtime-control) when the work instead has a fixed-period budget.
