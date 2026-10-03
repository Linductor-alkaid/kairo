# Blocking I/O Workers

## Use It For

One long-lived, interruptible socket, serial, CAN, or transport wait. Do not run such a loop inside a pool task or realtime callback.

## Minimal Usage

```cpp
executor::BlockingWorkerSpec spec;
spec.name = "transport";
spec.config.thread_name = "transport";
spec.worker = std::make_unique<MyInterruptibleWorker>();

auto worker = executor.start_worker(std::move(spec));
if (!worker.started()) return 1;
worker.stop();
```

Implement `IBlockingIoWorker::run(std::stop_token)` and `wakeup()`. `wakeup()` must release the current external wait and must not throw.

## Integration Pitfalls

- A stop token alone cannot interrupt an arbitrary third-party `read`, `poll`, or SDK call. Make `wakeup()` close/signal the underlying wait or use a bounded transport timeout.
- `WorkerHandle::started()` reports startup admission, not connection, protocol, or first-message readiness. Use `status()` and application health checks for those states.
- `request_stop()` wakes without joining; `stop()` requests stop, wakes, and joins. Do not retain worker references after Executor shutdown.

## Related Guide

`website/en/realtime-and-communication/blocking-io-workers.md`.
