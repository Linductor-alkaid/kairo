# Realtime Control

## Use It When

Search terms: realtime thread, cycle, affinity, priority, bounded MPSC, dropped task, `register_realtime_task`, `push_realtime_task`, `ICycleManager`.

Use this path for periodic control with explicit cycle and intake budgets. It is not an absolute deadline guarantee and must not absorb unbounded blocking, allocation, or draining in its callback.

## Public Boundary

- `include/executor/executor.hpp`: realtime registration, start/stop, and push APIs.
- `include/executor/config.hpp`, `interfaces.hpp`, and `types.hpp`: `RealtimeThreadConfig`, cycle manager, results and status.

## Implementation Trail

Trace `src/executor/realtime_thread_executor.*`, `src/executor/util/thread_utils.*`, and bounded queue/pool paths. The application owns a custom cycle manager; Executor borrows it.

## Observable Contract

- Queue acceptance means a later cycle may consume the task, not completion.
- Capacity and per-cycle budget protect the period; overflow and timing misses are status/failure evidence, never silent success.
- Platform permissions can prevent requested scheduling/affinity settings from taking effect; inspect the reported result/status.

## Change Safeguards

Keep producer shutdown, final drain, wrapper release, and overflow accounting ordered. Validate realtime lifecycle, hardening, overflow, per-cycle-budget, timer-race, and custom-cycle-manager tests.

## Related Material

`website/en/realtime-and-communication/realtime-control.md` and `website/en/advanced/custom-cycle-manager.md`.
