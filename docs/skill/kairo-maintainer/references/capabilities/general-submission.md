# General Submission And Completion

## Use It When

Search terms: ordinary work, `future`, result, task exception, soft timeout, queue timeout, batch submit, fire-and-forget, completion wait.

Use `submit_auto()` for ordinary finite work and `submit()` when explicit default-pool control is required. A returned `future` represents execution result or exception, not just queue admission.

## Public Boundary

- `include/executor/executor.hpp`: `submit_auto`, `submit`, batch submission, `wait_for_completion_ex`.
- `include/executor/task_options.hpp`: task-level options.
- `include/executor/types.hpp`: completion, wait, and failure status.

## Implementation Trail

Trace Facade templates to `src/executor/thread_pool_executor.cpp`, `thread_pool/`, and `task/`. `ThreadPool` and dispatcher own acceptance, dispatch, execution, and completion accounting.

## Observable Contract

- `future.get()` returns the value or rethrows the task exception.
- A soft task timeout skips work that waited too long before execution; it never forcefully terminates executing C++.
- Batch submission reduces repeated submission overhead only when measured; do not promise a fixed speedup.
- Fire-and-forget still needs failure/status observation when the caller needs operational evidence.

## Change Safeguards

Accepted work must become completed or failed, and counters must reconcile. Verify Facade, timeout, batch, and thread-pool integration tests; include failure-observability coverage when error propagation changes.

## Related Material

`website/en/quick-start/first-task.md`, `website/en/quick-start/return-values-and-errors.md`, and `docs/API.md`.
