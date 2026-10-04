---
title: Production Readiness Checklist
description: Define Kairo ownership, failure observation, capacity, waiting, and shutdown policy for a long-running service.
---

# Production Readiness Checklist

A passing tutorial proves compilation, linking, and basic semantics—not production integration. Put Kairo into the application's lifecycle, capacity, and failure model before deployment. If you still use detached threads, `std::async`, or a hand-written queue, start with [migrating existing thread code](/en/guides/migrating-existing-threads); use [antipatterns](/en/guides/concurrency-antipatterns) to inspect an existing design by symptom.

## 1. Name the runtime owner

The singleton fits a shared process pool; an independent `Executor` fits test/plugin isolation or a component that needs separate shutdown. Do not construct an Kairo per request, and do not let several modules believe they may shut down the shared singleton.

Document who initializes before first submission, who updates failure/monitoring configuration, who begins draining and shutdown after intake stops, and which components only borrow the runtime.

## 2. Set capacity from real load

Default configuration is a validation starting point, not a production recommendation. Estimate peak arrival rate, task duration, and acceptable queue delay before setting `min_threads`, `max_threads`, and `queue_capacity`.

A larger queue can turn fast overload rejection into long processing of stale work. When old data has no value, merge, overwrite, or reject at the business boundary. `task_timeout_ms` observes pre-execution queue time; it does not terminate arbitrary functions. Give network, file, and device I/O their own timeouts.

## 3. Choose a completion boundary per work class

| Work | Completion boundary | Exit policy |
| --- | --- | --- |
| Request calculation | Retained future, `get()` within request budget | Return success or explicit error |
| Background batch | All futures or batch state | Bounded wait; record unfinished count |
| Fire-and-forget | Callback/status plus business ID | Explicitly allow completion or discard |
| Soft periodic maintenance | Task ID plus periodic status | Cancel first, then wait for in-flight callbacks |
| Real-time control | Realtime status plus drop/timeout counters | Stop producers before real-time thread |

If no one can name the owner of a future, task ID, or realtime task name, lifecycle design is incomplete.

## 4. Use three failure-observation layers

```text
Single operation      future.get()
Service trend         get_failure_status() + monitoring
Recent context        set_failure_callback() + get_recent_failures()
```

Callbacks should only transfer a short nonblocking event. Put formatting, remote reporting, and retries in application logging/alert threads. The recent-failure buffer is diagnostic, not an audit log. Set separate thresholds for task exceptions, rejection, wait timeout, real-time drops, GPU failures, and tuning fallback. Communication drops/overwrites/stale/missed phases remain component-local by default and also need monitoring integration.

## 5. Budget every wait

Do not make a request wait indefinitely for unknown work. Bound individual results with the future; drain a service with `wait_for_completion(timeout)` and inspect active, queued, and pending counts on timeout.

Choose the post-timeout action in advance: continue briefly with an escalated alert, stop intake and only finish accepted work, choose `shutdown(false)` and accept its business consequences, or persist unfinished input for recovery. The library cannot choose data-consistency policy for you.

## 6. Shut down in order

1. Mark external intake as draining.
2. Stop timers, device callbacks, and other producers.
3. Cancel ordinary periodic tasks.
4. Stop upstream real-time pushes, then real-time threads.
5. Bounded-wait ordinary asynchronous tasks and record a snapshot.
6. Call `shutdown(true)` from the wait result, or explicitly select fast shutdown.
7. Finally destroy data captured by tasks, logging, and communication infrastructure.

Destroying business objects before waiting for tasks that capture them is a common use-after-free source.

## 7. Prove the design under overload

Before release, create temporary input faster than consumption, task and callback exceptions, a request wait timeout, submission during shutdown, full real-time queues/object-pool exhaustion, unavailable GPU backend/device/runtime, and unavailable Linux real-time permissions/affinity. Passing means callers receive explicit results, counters change as expected, logs identify executor/task, and shutdown remains within budget—not merely that the process does not crash.

## Minimum review record

```text
Kairo owner:
Initialization configuration and rationale:
Work classes and submission APIs:
Future / task-ID owners:
Overload policy:
Failure-alert sources and thresholds:
Per-operation wait budget / drain budget:
Shutdown order:
Real-time or GPU degradation:
Failure scenarios validated:
```

Use this record with the tutorial's [bounded waiting and status](/en/tutorial/waiting-and-status), [Failure Observability](/en/reliability/failure-observability), and [Real-Time and Communication](/en/realtime-and-communication/).
