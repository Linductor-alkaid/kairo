# Concurrency Internals And Performance

## Use It When

Search terms: thread pool, scheduler, dispatch, work stealing, resize, lock-free queue, object pool, CAS, throughput, jitter, TSAN, benchmark.

This card is for internal behavior changes. Public compatibility applies to `include/executor/`; source layouts and algorithms may change only when externally observable guarantees and tests remain valid.

## Public Boundary

- Public results/statuses in `include/executor/executor.hpp`, `types.hpp`, and configuration headers.
- Internal owners: `src/executor/thread_pool/`, `src/executor/task/`, and `src/executor/util/lockfree_queue.hpp`.

## Implementation Trail

Start at `ThreadPool`, `PriorityScheduler`, and `TaskDispatcher`; then follow worker-local queues, resize snapshots, and the lock-free sequence protocol. Identify writer, reader, destruction point, and wakeup mechanism before editing.

## Observable Contract

- An accepted task must complete or fail visibly; it cannot disappear during dispatch, resize, or stop.
- A failed task is completed work for reconciliation purposes.
- Atomic use does not erase ownership, memory ordering, or cross-domain synchronization requirements.
- Performance claims require a reproducible workload, build mode, machine metadata, and correctness check.

## Change Safeguards

Write a minimal regression test before changing concurrency control. Run targeted resize/dispatcher/queue tests, stress or sanitizer coverage where available, then benchmarks only after correctness is established. Preserve the final-drain and lifetime guarantees across stop/submit races.

## Related Material

`website/en/advanced/source-architecture.md`, `website/en/advanced/lockfree-and-performance.md`, and `website/en/advanced/performance-measurement.md`.
