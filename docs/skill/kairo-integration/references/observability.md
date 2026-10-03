# Failures, Status, And Snapshots

## Use It For

Service-level failure reporting, bounded wait diagnosis, queue/worker health, task statistics, and support snapshots.

## Minimal Usage

```cpp
executor.set_failure_callback([](const executor::ExecutorFailureEvent& event) {
    report_failure(event.kind, event.message);
});

const auto wait = executor.wait_for_completion_ex(std::chrono::seconds(1));
const auto status = executor.get_completion_status();
const auto snapshot = executor.get_snapshot_text();
```

Use backend-specific status for realtime, blocking I/O, and GPU paths. Use communication component `CommStats` and event callbacks separately.

## Integration Pitfalls

- A wait timeout means work remains; it does not cancel work or establish the cause. Preserve `WaitResult`, failure status, and recent events for diagnosis.
- `get_snapshot()` and `get_snapshot_text()` are low-frequency best-effort diagnostic views. Do not call them from realtime callbacks or use them as synchronization.
- Facade failure events do not automatically include communication events. Bridge both into the application's monitoring system when unified alerts are required.
- Callback code must be quick and exception-safe. It executes on the path reporting the event, not on a dedicated logging thread.

## Related Guide

`website/en/reliability/monitoring.md`, `website/en/reliability/failure-observability.md`, and `website/en/realtime-and-communication/observability.md`.
