# Blocking I/O Workers

## Use It When

Search terms: long-running worker, transport, socket, read loop, `IBlockingIoWorker`, wakeup, stop token, `WorkerHandle`.

Use a blocking I/O worker when progress depends on an externally blocked read/poll/handle operation. Do not put that loop in a periodic callback or ordinary finite task.

## Public Boundary

- `include/executor/blocking_io.hpp`: `IBlockingIoWorker`, `BlockingWorkerSpec`, `WorkerHandle`.
- `include/executor/config.hpp` and `types.hpp`: worker configuration/status.
- `include/executor/executor.hpp`: `start_worker` facade.

## Implementation Trail

Read `src/executor/blocking_io_executor.*` and manager registration/teardown paths. The worker implementation belongs to the application, while Executor owns the executor thread lifecycle.

## Observable Contract

- `WorkerHandle::started()` reports startup admission, not finite work completion.
- `request_stop()` requires an application-provided `wakeup()` that unblocks the transport promptly; a stop token alone cannot interrupt arbitrary third-party I/O.
- Check worker status and explicit stop results during shutdown.

## Change Safeguards

Retain wakeup-before-join ordering, exception isolation, and no dangling manager access after owner teardown. Validate `tests/test_blocking_io_executor.cpp` and `tests/test_blocking_io_types.cpp`.

## Related Material

`website/en/realtime-and-communication/blocking-io-workers.md`.
