# Facade And Lifecycle

## Use It When

Search terms: initialization, singleton, isolated instance, shutdown, stop/submit race, wait, ownership, RAII, lifecycle snapshot.

Choose `Executor::instance()` only for process-wide sharing; use an `Executor` object for isolated ownership and deterministic teardown.

## Public Boundary

- `include/executor/executor.hpp`: `Executor`, `initialize_ex()`, `shutdown()`, completion wait/status APIs.
- `include/executor/config.hpp` and `include/executor/types.hpp`: configuration and observable results.
- `include/executor/executor_manager.hpp`: registry and owner boundary.

## Implementation Trail

Follow `src/executor/executor.cpp` into `executor_manager.cpp`. The manager owns default async and registered backend lifetimes; callers own application resources and must serialize any borrowed advanced pointer with shutdown.

## Observable Contract

- Lazy initialization is available for ordinary default use; explicit initialization is required before the first submission when configuration matters.
- A facade wait covers default future-style async work, not realtime queues, GPU work, or blocking I/O workers.
- Shutdown initiated from a worker requests stop without self-joining; an external owner completes the wait/join.
- Use `_ex` results, statuses, and failure events to distinguish rejection, timeout, and completion rather than treating a bool as full diagnosis.

## Change Safeguards

Preserve an unambiguous owner, the ability to wake blocked waits, and the no-accepted-task-after-final-drain invariant. Verify `tests/test_executor_facade.cpp`, `tests/test_concurrent_stop_submit.cpp`, `tests/test_self_stop_handoff.cpp`, and relevant snapshot/wait tests.

## Related Material

`website/en/quick-start/lifecycle.md`, `website/zh/quick-start/lifecycle.md`, and `website/en/advanced/source-architecture.md`.
