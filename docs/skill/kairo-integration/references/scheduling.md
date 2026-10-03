# Scheduling

## Use It For

Priority, delayed retry, periodic maintenance, batches, and dependencies between finite tasks.

## Minimal Usage

```cpp
auto urgent = executor.submit_priority(3, [] { apply_command(); });
auto retry = executor.submit_delayed(250, [] { retry_request(); });
const auto health = executor.submit_periodic(1000, [] { check_health(); });

urgent.get();
retry.get();
executor.cancel_task(health);
```

Use `submit_with_handle()`, `submit_after()`, and `when_all()` only when a task must wait for dependencies from the same `Executor` instance. Use `submit_batch()` when results for each item matter, otherwise benchmark `submit_batch_no_future()` before assuming a benefit.

## Integration Pitfalls

- Priority chooses waiting work first; it cannot preempt a task already running. Sustained critical work can starve lower priorities.
- Delayed and periodic APIs are soft scheduling, not realtime deadlines. Periodic callbacks may overlap when execution exceeds the period.
- Keep each periodic task ID and cancel it during service shutdown. Cancellation prevents future ticks but does not erase queued/running callbacks.
- Dependency handles are local to one Executor and terminal handles can expire according to the configured retention capacity.

## Related Guide

`website/en/tutorial/priority.md`, `website/en/tutorial/delayed-and-periodic.md`, `website/en/tutorial/batch.md`, and `website/en/tutorial/dependencies.md`.
