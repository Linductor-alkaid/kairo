---
title: Concurrency Architecture Antipatterns
description: Diagnose common Kairo integration errors from symptoms such as queue growth, stalled shutdown, dangling references, and silent failure.
---

# Concurrency Architecture Antipatterns

An antipattern is not an API that is always forbidden. It is locally plausible code that moves capacity, ownership, or failure responsibility to an unmanaged location. Diagnose the model before changing thread counts or queue size.

## Symptom index

| Symptom | Check first |
| --- | --- |
| Queue grows while CPU is low | Permanent blocking work, worker-side waits, I/O without timeout |
| Urgent work remains slow | Priority treated as preemption or real-time guarantee |
| Request succeeds but work is missing | Discarded future, fire-and-forget without failure observation |
| Shutdown intermittently hangs | Shutdown before producers stop, tasks without stop points, destruction order |
| Crashes or corrupted values | Reference capture, `this`, or cross-instance resources |
| Memory and latency rise steadily | Large queues absorbing persistent overload |
| Graph stalls with few threads | Dependent wrappers wait; too many in-flight chains |
| Real-time cycle spikes | Blocking callback, runtime allocation, unbounded drain |
| Monitoring looks healthy but users fail | Throughput average only; no futures/rejection/timeout/business outcome |

## 1. Permanent loop in the shared pool

```cpp
executor.submit_auto([&] {
    while (running) consume(socket.read());
});
```

It occupies a worker indefinitely, and blocking I/O may ignore application stop. Active stays full and queued rises even when CPU is low; `shutdown(true)` can wait but cannot safely kill arbitrary C++ code. Use `start_worker()` through the [Blocking I/O worker](/en/realtime-and-communication/blocking-io-workers) path when the loop needs Kairo-owned stop/wake/join lifecycle, `submit_periodic()` for soft maintenance, and a real-time task for jitter-budgeted loops. Verify that active count falls within budget after producers stop.

## 2. Synchronous pool wait inside a worker

```cpp
executor.submit_auto([&] {
    auto child = executor.submit_auto(load_part);
    return combine(child.get());
});
```

Parents consume workers while children wait for a worker, causing starvation when all workers repeat the pattern. Split work at the caller or use `TaskHandle` for bounded relationships, submit prerequisites first, and test with a small pool. A large dynamic DAG needs a dedicated graph scheduler.

## 3. Priority as preemption or deadline

`CRITICAL` changes selection from a waiting queue; it cannot preempt running work, guarantee a deadline, or determine completion order. Keep long work bounded, reserve high priority, use a real-time path for fixed periods/jitter, and express correctness with dependencies or a state machine. Measure queue and execution time separately under loaded workers.

## 4. Borrowed input after returning

```cpp
void Controller::update(const Command& command) {
    executor_.submit_auto([&] { apply(command, state_); });
}
```

`command` can die on return and `Controller` can die before execution. Capture small inputs by value or move them, use explicit shared ownership for shared state, stop new work before draining captured work, and never make `[&]`/`[this]` the asynchronous default. Test immediate owner destruction; use ASan/TSAN for lifetime/race evidence.

## 5. Discarded future with no second failure path

```cpp
executor.submit_auto(write_record);
return Accepted;
```

Acceptance does not prove eventual execution. Keep a future for request results; for genuine fire-and-forget work provide a callback/status plus business ID. Use an outbox, retry queue, or idempotency protocol for critical side effects: Kairo is not a delivery-guarantee system. Inject a throw and a rejection and confirm business metrics, alerts, and logs all identify the input.

## 6. A large queue hiding persistent overload

Increasing `queue_capacity` postpones failure but cannot make a sustained arrival rate below service capacity. It may process stale work, grow memory, and lengthen shutdown. Measure arrival rate, service time, queue depth, and end-to-end age; rate-limit/scale FIFO work, use `LatestMailbox` for latest-only state, and make intentional dropping observable. Under sustained overload, memory and latency must remain bounded.

## 7. Mixed Kairo instances and resources

Handles, registries, and direct executor pointers belong to the manager that created them. Do not pass an instance-A `TaskHandle` to instance-B, retain a manager-owned realtime/GPU pointer after shutdown, or let a component shut down a shared singleton. Pass the same Kairo explicitly, keep handles within their graph/runtime lifetime, and reserve direct pointers for protected advanced local use.

## 8. Reversed shutdown order

Wrong:

```text
Destroy business objects → shutdown Kairo → stop timers/device callbacks
```

Correct:

```text
Stop accepting external work
→ stop timers, device callbacks, and producers
→ cancel soft-periodic tasks
→ stop real-time upstream, then real-time tasks
→ bounded-drain ordinary work
→ shutdown Kairo
→ destroy task-owned data and logging
```

After shutdown, rebuild rather than reinitialize an Kairo. During shutdown-race testing, new submissions must receive explicit rejection and object destruction must follow relevant task completion.

## 9. Soft timeout mistaken for forced cancellation

`task_timeout_ms = 100` checks queue time before a task begins; it does not terminate a running task. A `wait_for_completion_ex()` timeout only says work remains. Bound I/O, let long CPU work check `stop_token`/atomic stop/deadline, decide whether post-timeout side effects may continue, and never treat `shutdown(false)` as safe thread killing.

## 10. Blocking, allocation, or unbounded drain in a real-time callback

Mutex waits, synchronous logging, container growth, and draining every queued command turn unbounded work into cycle jitter. Keep callbacks bounded, limit consumption with `max_tasks_per_cycle` or `RealtimeChannel::drain_for_cycle()`, preallocate, and hand logging/complex diagnostics to ordinary threads. Report p95/p99/max jitter, cycle timeout, and drops under backlog, insufficient permissions, and slow logging—not only average period.

## Eight review questions

1. Does this work complete within a bound? If not, who owns its stop protocol?
2. Who owns task input throughout asynchronous execution?
3. Who consumes each future, task ID, handle, or push result?
4. On overload, should data queue, reject, overwrite, or drop?
5. Does timeout mean queue delay, wait delay, or business cancellation?
6. Does correctness accidentally depend on priority, speed, or completion order?
7. Who stops first, how long is the wait budget, and what happens after expiry?
8. Can production distinguish task exception, rejection, backpressure, real-time drop, and tuning fallback from states/events?

If any answer is “we will inspect logs later,” the integration has not formed a reliable protocol. See [migrate existing thread code](/en/guides/migrating-existing-threads) or the [production readiness checklist](/en/guides/production-readiness).
