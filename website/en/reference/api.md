---
title: API Reference
description: Module entry points and stability boundaries for the public API.
---

# API Reference

This site anchors on `v0.6.1` as its stable baseline, covering unified auto-routing, waiting, communication, task graphs and diagnostics; unreleased capabilities on `master` do not constitute a stability commitment. Complete signatures, defaults, error codes and compatibility semantics are maintained only in the repository's [`docs/API.md`](https://github.com/Linductor-alkaid/kairo/blob/master/docs/API.md), so the site and the repository never carry two competing versions of the truth.

## Locate by module first

| Module | Facade / types | Learning entry | Full API docs |
| --- | --- | --- | --- |
| Lifecycle | `instance`, standalone instances, `initialize[_ex]`, `shutdown` | [Lifecycle](/en/quick-start/lifecycle) | Lifecycle and configuration chapters of `docs/API.md`. |
| Regular tasks and routing | `submit_auto`, `TaskOptions`, `RoutingDecision`, `submit` | [Execution Models and Routing Boundaries](/en/guides/execution-models-and-routing), [Task Inputs and Ownership](/en/quick-start/task-inputs-and-ownership) | `Executor` template API. |
| Periodic and batch | `submit_periodic`, `TimerHandle::cancel`, periodic status, three batch flavors | [Delayed and Periodic](/en/tutorial/delayed-and-periodic), [Batch](/en/tutorial/batch) | Facade timer and batch chapters. |
| Task graphs | `submit_with_handle`, `submit_after[_with_handle]`, `when_all` | [Task Dependencies](/en/tutorial/dependencies) | Task dependency chapter. |
| Failure and waiting | failure callback/status, recent failures, `wait_for_completion[_ex]`, completion status | [Failure Observability](/en/reliability/failure-observability), [Bounded Waiting](/en/tutorial/waiting-and-status) | Failure, waiting and type chapters. |
| Monitoring | `enable_monitoring`, sampling rate, task statistics | [Monitoring and Sampling](/en/reliability/monitoring) | Monitoring API chapter. |
| Communication | `kairo::comm`: channel, mailbox, snapshot, phase | [Choosing Communication Components](/en/guides/choosing-communication) | Communication API chapters. |
| Cancellation and timers | `submit_cancellable*`, `request_task_cancel`, `TimerHandle`, `ScopedTimerHandle` | [Cancellation and Timers](/en/realtime-and-communication/cancellation-and-timers) | Cancellation and timer API chapters. |
| Serial dispatch and total admission | `submit_on[_with_handle]`, `SerialExecutionContext`, `max_in_flight_tasks`, `CapacityExhaustedException` | [Capacity and Alerting](/en/realtime-and-communication/capacity-and-alerting), [Event Loop Interop](/en/guides/event-loop-interop) | Admission and serial dispatch chapters. |
| Realtime | `_ex` registration/start, push, status, task list | [Realtime Control Loops](/en/realtime-and-communication/realtime-control) | Realtime task API. |
| GPU | `_ex` registration, `submit_gpu`, status, `submit_auto`, scheduler | [GPU Topic](/en/gpu/) | GPU API and build docs. |
| Bounded dispatch and workers | `dispatch_auto`, `DispatchResult`, `start_worker`, `WorkerHandle` | [Choosing a Submit API](/en/guides/choosing-submit-api), [Blocking I/O Workers](/en/realtime-and-communication/blocking-io-workers) | Routing and Blocking I/O API. |
| Advanced | `ExecutorManager`, executor pointers, `ICycleManager`, `LockFreeTaskExecutor` | [Advanced and Internals](/en/advanced/) | Advanced interfaces and design docs. |

## Facade coverage index

The table below is the pre-release checklist: every public `Executor` facade family has at least one tutorial, guide or reference entry. It does not replace overload signatures.

| Interface family | Default entry | Points needing further confirmation |
| --- | --- | --- |
| `instance`, standalone construction, `initialize[_ex]`, `shutdown` | Quick start | Configuration, resource isolation and shutdown strategy. |
| `submit_auto`, `submit`, priority, delayed, periodic, batch | Scenario tutorials and submit selection | futures, routing decisions, periodic status, backpressure and benchmarks. |
| handles, dependencies, joins | Task dependency tutorial | same-instance limits, failure propagation and task-graph scale. |
| failure, recent buffer, waiting, completion snapshots | Reliability and waiting tutorials | `FailureKind`, `WaitResult` and status fields. |
| Monitoring and statistics | Monitoring and sampling | Sampling rate and statistics overhead. |
| `submit_cancellable*`, `request_task_cancel`, `get_cancellation_status` | Cancellation and timers tutorial | Cooperative semantics, registry capacity and cancellation counters. |
| `submit_delayed`, `submit_periodic`, `TimerHandle` / `ScopedTimerHandle` | Cancellation and timers tutorial | cancel/reschedule, terminal states and timer counters. |
| `submit_on`, `submit_on_with_handle`, `SerialExecutionContext` | Event loop interop guide | FIFO ticket ordering, non-blocking dispatch and shutdown rejection. |
| `max_in_flight_tasks`, `set/get_max_in_flight_tasks`, `get_in_flight_submissions` | Capacity and alerting | Covered paths, capacity rejection semantics and counters. |
| realtime registration, push, list and status | Realtime control tutorial | Permission degradation, cycle budgets and rejection counters. |
| Bounded dispatch, Blocking workers | Execution models and routing boundaries | admission is not completion; worker lifecycle. |
| `register_lockfree_executor` / `start_` / `stop_` / `get_lockfree_executor_names` | Submit selection, advanced and internals | Backend lifecycle; `dispatch_auto`'s `accepted` only means queue admission. |
| GPU registration, submission, status, auto scheduling | GPU tutorials | Backend availability, streams and hardware validation. |
| Direct manager / executor pointers | Advanced interfaces | Ownership, concurrency and lifetime responsibilities. |

## How to read status and results

Do not collapse every failure into one `bool`. Use `future.get()` for single tasks; `ExecutorResult` for diagnosable control operations; `ExecutorFailureStatus` plus recent events for trends; `WaitResult` for wait timeouts; realtime/GPU/communication each expose their own status snapshots and statistics. Communication `CommStats` does not flow into `ExecutorFailureStatus` automatically.

## Entries outside the regular manual

`set_timer_thread_factory_for_test()` is a test-injection hook for simulating timer-thread creation failure; it is not a production configuration API. `ThreadPool`, schedulers, queues and object pools under `src/` are the current implementation, not guaranteed stable integration interfaces. To understand them, read [Advanced and Internals](/en/advanced/); actual programs should depend only on the public headers under `include/kairo/`.

## Further reading

For upgrading existing code, read [Versions and Migration](/en/reference/version-and-migration); when unsure which entry point fits your current problem, start with [How to Choose a Submit API](/en/guides/choosing-submit-api).
