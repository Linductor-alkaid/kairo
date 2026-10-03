# Scheduling And Task Graphs

## Use It When

Search terms: priority, delayed task, periodic task, cancellation, batch, dependency, `TaskHandle`, `when_all`, task graph, timer.

Select the smallest scheduling semantic that fulfills the request. A soft periodic maintenance callback is not a realtime control loop, and a dependency handle is not a cross-process workflow engine.

## Public Boundary

- `include/executor/executor.hpp`: priority, delayed, periodic, batch, `submit_with_handle`, `submit_after`, and `when_all` APIs.
- `include/executor/types.hpp`: task IDs, task handles, periodic status.

## Implementation Trail

Read `src/executor/thread_pool/priority_scheduler.*`, `src/executor/task/task_dependency_manager.*`, and timer-related Facade code. Dispatch failure must return accepted work to a viable path rather than strand its future.

## Observable Contract

- Priority changes dequeue order; it does not preempt an already-running task.
- Delayed and periodic tasks are best-effort scheduling facilities with observable status/cancellation, not hard deadlines.
- Dependencies are scoped to their originating `Executor`; retention capacity may expire terminal handles while active chains stay protected.

## Change Safeguards

Preserve dependency state transitions, handle retention behavior, task cancellation semantics, and completion accounting. Run relevant `test_executor_task_graph`, `test_task_dependency_manager`, priority, batch, periodic failure, and timer tests.

## Related Material

`website/en/tutorial/priority.md`, `website/en/tutorial/delayed-and-periodic.md`, and `website/en/tutorial/dependencies.md`.
