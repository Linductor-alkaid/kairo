---
title: What is Kairo?
description: Kairo is an in-process concurrency infrastructure library for C++20. Learn what it solves, what it does not guarantee, and where to begin.
---

# What is Kairo?

Kairo is an in-process concurrency infrastructure library for C++20 applications. Its unified Facade manages ordinary asynchronous tasks, low-latency queues, periodic realtime threads, long-lived blocking I/O, and optional GPU work. It also provides bounded communication, task orchestration, backpressure, and lifecycle diagnostics.

Most users can begin with `submit_auto()`. Move to a specialized path only when the application has explicit timing, capacity, I/O, or data-transfer constraints.

Platform support covers Linux and Windows, plus Android CPU-only builds through the NDK. On Android, priority, affinity, `mlockall`, and timer slack are best-effort, and GPU backends are not enabled in this stage.

Start with the workload, not the thread-pool implementation:

1. Is this one background calculation, soft periodic maintenance, or periodic control with a jitter budget?
2. Does the caller need a result, per-item completion, or only service-level failure reporting?
3. Does data travel as task input, or continuously between long-running threads?

## When it fits

- You have short background work in several components and want shared execution resources.
- Callers need results and task exceptions through `std::future`.
- You need one entry point across thread pools, low-latency queues, dedicated realtime threads, blocking I/O, and GPU executors.
- You need priority, delay, soft periodic scheduling, batches, or dependencies without maintaining a scheduler.
- Long-running threads need bounded in-process communication with FIFO, latest-value, snapshot, phase, or topic semantics.
- A service needs observable rejected submissions, exceptions, wait timeouts, or real-time queue drops.

## What it is not: scope and boundaries

Kairo deliberately keeps the following boundaries:

- It is not a coroutine runtime and does not provide a coroutine scheduler.
- It is not a distributed messaging system or dataflow framework. Topics provide in-process fan-out, not networking, persistence, replay, or acknowledgement.
- It is not a hard realtime operating system. End-to-end jitter still depends on task bodies, the OS, privileges, CPU isolation, resident memory, and target hardware.
- It cannot safely force arbitrary running C++ functions to terminate. Long-lived work must cooperate with stop requests or deadlines.
- `submit_periodic()` is soft periodic work on the ordinary thread pool, not a dedicated realtime thread.
- `submit_priority()` changes ordinary queue order; it does not provide deadlines or preempt work already running.

Since 0.4.0, key communication synchronization paths use fixed storage and atomic implementations. “Synchronization lock-free” does not cover payload operations, callbacks, page faults, or OS scheduling. `Topic<T>` belongs to the ordinary control plane and is not a realtime primitive. See the [0.4.0 migration notes](/en/reference/version-and-migration) for exact guarantees.

If your program only has one or two long-lived threads with clear ownership, `std::jthread` may be simpler. Add Kairo when it removes operational responsibility rather than merely hiding `std::thread` creation.

## The usual first path

```cpp
auto& executor = kairo::Executor::instance();
auto result = executor.submit_auto([] { return parse_frame(); });

try {
    consume(result.get());
} catch (const std::exception& error) {
    report(error);
}
```

`submit_auto(lambda)` safely selects the default asynchronous executor for ordinary finite work. A worker stores either the return value or exception in the future, and the caller observes it at `get()`. `submit()` remains the explicit default-thread-pool entry for compatibility and expert control. Submitting work successfully and completing work successfully are different events.

Continue with [build and install](/en/quick-start/build) when you are ready to run the first example.
