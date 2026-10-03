---
title: Lock-Free and Performance Experiments
description: Understand LockFreeTaskExecutor, MPSC slot protocol, object pools, and backoff before using internal performance paths.
---

# Lock-Free and Performance Experiments

## Goal

Decide whether `LockFreeTaskExecutor` is appropriate, understand its current queue/object-lifetime mechanisms, and validate performance without promoting internal details to a public contract.

## When to use it

Use `submit_auto(lambda)` for ordinary finite work. Consider `LockFreeTaskExecutor` only when a single consumer, bounded queue, explicitly handled rejection, and measured producer-contention benefit outweigh the additional lifecycle/capacity responsibility. In the unified control plane, register and start a named backend, then use `dispatch_auto(LowLatency)`; its return is admission, not a future-completion promise. It does not replace task graph, future semantics, service failure protocol, or application backpressure design.

```cpp
TaskOptions options;
options.intent = ExecutionIntent::LowLatency;
options.preferred_executor = "telemetry";
auto admission = executor.dispatch_auto(options, [] { process_event(); });
if (!admission.accepted) {
    // stopped, queue full, pool exhausted, or shutdown competition
}
```

`LowLatency` and `preferred_executor` must match the named, started lock-free executor; the call never finds another available queue or falls back to the pool. See [how automatic routing matches a target](/en/guides/execution-models-and-routing).

## Current internal structure and “lock-free” scope

`LockFreeQueue` provides bounded enqueue/dequeue. `ObjectPool` preallocates task wrappers to avoid hot-path allocation, but uses a mutex for free-list correctness against ABA, foreign pointers, and double release. Exception handler access also uses a mutex. The queue is lock-free; the whole executor is not a lock-free progress guarantee.

## MPSC slot sequence protocol

Capacity rounds to a power of two and `index = position & (capacity - 1)`, but each slot sequence—not index—prevents half-written reads and overwrite:

```text
initialize slot i: sequence = i

producer reserves position p: require sequence == p
producer writes buffer[i]
producer release-stores sequence = p + 1

consumer reads position p: require sequence == p + 1
consumer reads buffer[i]
consumer release-stores sequence = p + capacity
```

`enqueue_pos_` CAS allocates a unique position, not readable data. Producer release after writing lets consumer acquire before reading; consumer release after reading lets the next producer acquire before reuse. Position CAS can be relaxed because sequence pairing provides visibility. Relaxing sequence stores can appear correct on x86 yet publish incomplete values on weak-order ARM; validate memory-order changes on target architecture with TSAN/stress, not a local benchmark alone.

The queue reserves an empty slot and rejects when `enqueue_pos - dequeue_pos >= capacity - 1`. `empty()` and `size()` are instantaneous approximations under concurrency. Decide synchronization through `push()`/`pop()` results; never use approximate size/empty as a strict predicate, allocation amount, or safe-close proof.

## Status snapshot and back-pressure diagnostics

A monitoring thread should sample `get_status_snapshot()`, which returns a value-copyable `QueueStats`:

```cpp
kairo::LockFreeTaskExecutor exec(4096, 2, /*enable_stats=*/true);
exec.start();

std::thread monitor([&] {
    while (auto s = exec.get_status_snapshot(); s.processed_count < kDone) {
        if (s.ready_count > s.queue_capacity / 2) {
            // Consumer is lagging; consider scaling, lowering sample rate, or shedding.
        }
        std::this_thread::sleep_for(100ms);
    }
});
```

The snapshot is composed of independent atomic loads, so **every field is approximate and unsynchronized**: treat it as a trend/alerting signal, never as a synchronization primitive or a correctness oracle.

`failed_pushes` is the aggregate of every failed underlying push attempt. In a quiescent snapshot, the three reason counters below sum to it and point to different remediation:

| Field | Source | Typical action |
| --- | --- | --- |
| `queue_full_rejections` | Queue full | Scale capacity or consumer throughput. |
| `contention_rejection` | CAS contention exhausted the retry budget | Reduce producer count or raise the backoff multiplier. |
| `reservation_cancelled_rejections` | Consumer cancelled the producer's reservation | Check producer preemption and the reservation window. |
| `cancelled_reservation_count` | Consumer cancelled a reservation after the bounded wait (default 64 yields) | Check whether producers get preempted or hold a lock in the `Writing` window. |
| `submission_rejection` | Kairo entry-side reject: empty task, post-stop submit, or object-pool exhaustion | Usually upstream logic or `stop()` race; small empty-task counts are caller bugs. |

A sustained rise in `reserved_count` while `ready_count` stays flat indicates a producer stuck in the reservation window. A sustained rise in `cancelled_reservation_count` indicates the consumer side is recovering reservations frequently. Rising `submission_rejection` with zero `contention_rejection` points the operator at call-site logic and lifecycle, not the queue itself. See the full field table and the default 64-yield budget in [`docs/API.md` §5.5](https://github.com/Linductor-alkaid/kairo/blob/master/docs/API.md) under "状态快照与背压诊断". The new `tests/test_lockfree_queue_status.cpp` covers the by-value copy and concurrent-sampling-doesn't-block-producers contract.

## Backoff, batch, and lifecycle

CAS failure uses bounded exponential PAUSE/yield-style backoff. It reduces contention pressure but increases one-push delay; it is not automatic waiting until success, and producers have no FIFO fairness. If every request needs acceptance, add an upper-layer bounded retry/deadline; infinite retry turns visible backpressure into starvation.

`push_batch()` validates candidate slot sequences before one CAS reservation, then writes/releases each slot. Reserving first and discovering a bad middle slot can leave a position gap. Normal batch reports partial `pushed`; `push_batch_exact()` requires all-or-nothing admission. Upper layers must fulfill rejected futures for unpushed items.

```text
push_task → nonempty check → active_pushes++ → ObjectPool::acquire
          → set wrapper->func → LockFreeQueue::push(pointer)

consumer pop → execute wrapper->func → clear function → ObjectPool::release
```

Pool exhaustion differs from full queue. `stop()` blocks producers, waits `active_pushes_` to zero, stops consumer, and drains wrappers. Do not enqueue an external pointer then free it, or destroy objects captured by a wrapper while it may execute.

## Interpret measurements by mechanism

| Observation | Likely mechanism | Check first |
| --- | --- | --- |
| Push p99 jumps with more producers | CAS contention/backoff | Failed pushes, producer count, backoff multiplier |
| High throughput and high failure | Capacity or consumption insufficient | `failed_pushes`, capacity, consumer drain rate |
| One producer fast; many producers slower | Shared enqueue-position/cache-line contention | Retest with fixed task body and CPU affinity |
| `pending_count()` is briefly zero with work | Approximate snapshot | Reconcile pop/completed; do not use size for completion |
| Stop intermittently waits | Producer not exited or wrapper blocks | Active pushes, task stop point, drain count |

Object-pool mutex, function-object allocation, task body cost, CPU frequency, and consumer execution may dominate end-to-end performance; do not optimize CAS in isolation. Worker local queues/stealing/internal pools are implementation details and cannot be integrated from `src/`.

## Validate performance

| Question | Method |
| --- | --- |
| Data race or lifetime error? | TSAN, concurrent stress, boundary tests |
| Correct degradation when full? | Assert failure returns, drop/rejection counts, recovery |
| Faster on a hardware target? | Fix build, CPU, workload, threads, data size, then benchmark |
| Better real end-to-end latency? | Measure submit, queue, execution, tail latency—not one microbenchmark |

Performance numbers exist only in their measured environment. Record commit/compiler/build/hardware/power policy/task/concurrency/statistics; never make an API promise from one result. Source changes need protocol correctness (`accepted = completed + rejected`), weak-memory/TSAN stress, then distribution metrics with fixed producers/capacity/task/CPU. Queue size/pending stats are observability values, not completion reconciliation.

See current files [`lockfree_task_executor.cpp`](https://github.com/Linductor-alkaid/kairo/blob/master/src/kairo/lockfree_task_executor.cpp), [`lockfree_task_executor.hpp`](https://github.com/Linductor-alkaid/kairo/blob/master/include/kairo/lockfree_task_executor.hpp), [`lockfree_queue.hpp`](https://github.com/Linductor-alkaid/kairo/blob/master/src/kairo/util/lockfree_queue.hpp), and [`object_pool.hpp`](https://github.com/Linductor-alkaid/kairo/blob/master/src/util/object_pool.hpp).

Next: [performance measurement and regression gates](/en/advanced/performance-measurement).
