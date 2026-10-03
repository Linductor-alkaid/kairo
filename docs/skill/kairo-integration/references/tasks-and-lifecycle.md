# Tasks And Lifecycle

## Use It For

Finite background work, values, exceptions, a bounded wait, service shutdown, and ordinary default-pool configuration.

## Minimal Usage

```cpp
executor::Executor executor;
executor::ExecutorConfig config;
config.max_threads = 4;
if (!executor.initialize_ex(config)) return 1;

auto result = executor.submit_auto([] { return compute(); });
auto value = result.get();
executor.shutdown(true);
```

`future.get()` is the result and task-exception boundary. `submit()` is the explicit default-thread-pool entry; `submit_auto()` is the recommended ordinary entry.

## Integration Pitfalls

- A successful submission is not successful execution. Retain and inspect the future when the outcome matters.
- `task_timeout_ms` is a pre-execution soft timeout: it skips queued work that waited too long and never kills running C++ code.
- `wait_for_completion_ex()` waits only for default future-style asynchronous work. It does not wait for realtime queues, GPU executors, or blocking I/O workers.
- Stop producers before `shutdown(true)`. If shutdown is called by a pool worker, it requests stop without joining itself; an external owner must complete teardown.

## Related Guide

`website/en/quick-start/first-task.md`, `website/en/quick-start/return-values-and-errors.md`, and `website/en/quick-start/lifecycle.md`.
