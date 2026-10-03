# Routing And Low-Latency Work

## Use It For

An explicitly named lock-free backend, bounded fire-and-forget admission, or a route decision that must be recorded. Do not use it when the caller needs a `future`; use ordinary `submit_auto()` instead.

## Minimal Usage

```cpp
auto events = std::make_unique<executor::LockFreeTaskExecutor>(256);
if (!executor.register_lockfree_executor("events", std::move(events))) return 1;
if (!executor.start_lockfree_executor("events")) return 1;

executor::TaskOptions options;
options.intent = executor::ExecutionIntent::LowLatency;
options.preferred_executor = "events";
const auto admission = executor.dispatch_auto(options, [] { publish_event(); });
```

Check `admission.accepted`, `admission.decision`, and `admission.message`. Stop the named backend during application shutdown with `stop_lockfree_executor("events")`.

## Integration Pitfalls

- Acceptance means the bounded queue received the callable. It does not provide a result, completion future, or exactly-once delivery guarantee.
- `LowLatency` requires both the explicit intent and a running named backend. Rejection never silently switches to the default pool.
- For a direct `LockFreeTaskExecutor`, call `start()` before `push_task()` and use `exception_count()`, `get_queue_stats()`, or an exception handler for execution evidence.
- Queue capacity is rounded for the underlying queue. Treat full, contention, stop, and callback failure as separate observable outcomes.

## Related Guide

`website/en/guides/execution-models-and-routing.md` and `website/en/advanced/lockfree-and-performance.md`.
