---
title: Delayed Retry and Health Checks
description: Use delayed and soft-periodic Facade tasks for device retry and background health checks.
---

# Delayed Retry and Health Checks

## Goal

Schedule one device retry with `submit_delayed()`, run health checks with `submit_periodic()`, and stop periodic work through status queries and `cancel_task()`.

<<< @/../examples/tutorial/03_delayed_periodic.cpp{1-36}

```bash
./build/examples/tutorial/tutorial_03_delayed_periodic
```

Expected output:

```text
retry complete
health checks=1
periodic status=running, cancelled=yes
```

The count depends on scheduling. The stable assertion is that the status exists before cancellation and `execution_count > 0`.

## Status and input ownership

Keep the ID returned by `submit_periodic()` to call `cancel_task()`. `get_periodic_task_status(id)` returns a registered task's status; after cancellation it returns no result. `get_all_periodic_task_status()` is useful for monitoring and shutdown checks. A delayed task returns a future, so values, exceptions, and rejected submission remain observable through `get()`.

`submit_delayed(delay, fn, args...)` accepts separate functions and arguments like `submit()`. `submit_periodic(period, task)` accepts a repeatable `std::function<void()>`; bind business inputs in a lambda:

```cpp
auto device = std::make_shared<DeviceClient>(endpoint);
const auto id = executor.submit_periodic(1000, [device] { device->check_health(); });
```

The callback must be copyable. A `unique_ptr` capture generally cannot become `std::function<void()>`; use a stable owner or `shared_ptr`. A reference capture is valid only if its owner survives cancellation, timer stop, and every in-flight callback.

Each timer tick submits the same callback to the ordinary pool without waiting for the previous run. A callback longer than its period can overlap itself; protect mutable state with atomics, a mutex, or application-level single-flight logic.

## Failure and shutdown

Periodic exceptions do not return to the `submit_periodic()` call. Check periodic status and configure failure status or a callback in a long-running service. Successful cancellation stops future scheduling only: work already queued or running can still complete.

Stop creating delayed/periodic work, cancel every retained ID, wait within a budget for submitted callbacks, then shut down Kairo. Network or device I/O needs its own timeout.

Inject a retry exception, an overlong health check, repeated callback failures, and shutdown before delayed expiry. Do not use a larger queue to mask overlap. For exponential backoff, schedule the next delayed task after each failure with a capped attempt count; for strict phase or jitter requirements, use a dedicated real-time task instead.

Next: [batch sensor frames](/en/tutorial/batch).
