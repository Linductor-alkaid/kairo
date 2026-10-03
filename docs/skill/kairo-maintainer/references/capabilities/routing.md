# Routing And Named Backends

## Use It When

Search terms: `dispatch_auto`, `TaskOptions`, executor kind, named backend, low latency, realtime queue, auto routing, fallback, `RoutingDecision`.

Route only when the requested backend and acceptance semantics are explicit. Automatic routing does not prove callable realtime safety, data ownership, or backend availability at future execution time.

## Public Boundary

- `include/executor/task_options.hpp`: route request and fallback policy.
- `include/executor/task_router.hpp`: `TaskRouter` and `RoutingDecision`.
- `include/executor/executor.hpp`: dispatch/auto submission facade.

## Implementation Trail

Follow `src/executor/task_router.cpp` and Facade admission into manager registry snapshots. Registration lookup and physical queue admission are separate stages and can race with stop or capacity change.

## Observable Contract

- `DispatchResult.accepted` means the target accepted work; it is not task completion.
- A failed route never silently selects an unrelated backend.
- CPU fallback is policy-driven; callers inspect the recorded decision and still handle execution failures.

## Change Safeguards

Keep selection explanation, rejection reason, and failure event consistent. Validate all `test_executor_auto_routing_stage*` and `test_dispatch_task_fallback.cpp` cases affected by the policy.

## Related Material

`website/en/guides/execution-models-and-routing.md` and `website/en/guides/choosing-submit-api.md`.
