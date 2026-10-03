# Observability And Diagnostics

## Use It When

Search terms: failure callback, failure event, status, snapshot, monitor, timeout, queue depth, alert, completion status, metrics.

Every failed admission, task exception, drop, and wait timeout needs a queryable outcome. Choose the component that owns the condition; do not force unrelated failures into a single counter.

## Public Boundary

- `include/executor/executor.hpp`: failure callbacks, failure status, completion status, snapshots.
- `include/executor/monitor/`: monitor and formatter APIs.
- `include/executor/types.hpp` and `comm/types.hpp`: result/status/event records.

## Implementation Trail

Follow `src/executor/executor.cpp` failure recording plus `src/executor/monitor/`. Callbacks must execute after releasing internal diagnostic locks to avoid re-entry deadlocks.

## Observable Contract

- A wait timeout is evidence that work remains; it does not cancel or prove a task failure.
- Completion state, execution failure, queue rejection, and comm events are distinct diagnostic surfaces.
- Snapshot APIs are best-effort observations and must not become synchronization authority for changing behavior.

## Change Safeguards

Preserve event ordering, callback isolation, counter reconciliation, and non-blocking callback boundaries. Validate failure-observability, snapshot, monitoring, and timeout tests that own the changed event.

## Related Material

`website/en/reliability/monitoring.md`, `website/en/reliability/failure-observability.md`, and `website/en/realtime-and-communication/observability.md`.
