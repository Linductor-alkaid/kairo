---
title: Advanced Interfaces
description: Leave the Kairo Facade cautiously only for genuine resource isolation, executor extension, or GPU resource control.
---

# Advanced Interfaces

## Goal

Decide when to stay with the `Executor` Facade and when an independent instance, `ExecutorManager`, or a direct executor pointer is required—then accept the added lifecycle and concurrency responsibility.

| Need | Default | Enter advanced path when |
| --- | --- | --- |
| Ordinary background, periodic, dependency, monitoring work | `Executor` Facade | Never necessary |
| Isolate threads and shutdown timing from another subsystem | Independent `Executor` | Need independent resources and deterministic destruction |
| Register custom executors or manage several executors | `ExecutorManager` | You own transfer, naming, and shutdown |
| Direct realtime queue/cycle detail | `get_realtime_executor()` | Facade push is insufficient and you can handle state/rejection/stop race |
| Device memory, stream, P2P, unified memory | `get_gpu_executor()` | Backend and resource lifetimes verified |

## Instance isolation

`Executor::instance()` shares one process manager. `kairo::Kairo executor;` creates an independent `ExecutorManager`, so pools, realtime/GPU registries, and shutdown timing do not mix with the singleton. RAII destruction cleans its manager, but explicitly call `shutdown()` after producers stop to choose whether accepted work drains.

Do not transfer `TaskHandle`, realtime/GPU executor pointers, or dependency relationships across instances. They belong to the manager that created them and lose valid lifecycle/concurrency semantics elsewhere.

## Manager and direct-pointer responsibility

`ExecutorManager` creates, registers, and retrieves `IAsyncExecutor`, `IRealtimeExecutor`, and `IGpuExecutor`. Registration accepts `std::unique_ptr`, transferring ownership; creation is not registration. Direct pointers from `get_*_executor()` remain manager-owned: never delete them, retain them past manager shutdown, or use them concurrently with shutdown.

When an operation must retain an executor through concurrent `shutdown()`, use the manager's `get_*_executor_snapshot()` APIs. They return a `std::shared_ptr`, keeping the backend object alive after registry removal until that snapshot is released. A snapshot is not a keep-running lease: shutdown can still stop the backend, so callers must handle submission results, futures, and status.

The Facade aggregates common rejection, failure event, and status behavior. Direct interfaces can bypass those observation paths, so their caller owns return values, futures, status, and resource closure.

## Stability boundary

Facade/configuration/interface/manager declarations under `include/kairo/` are public API. `src/` `ThreadPool`, schedulers, queues, and object pools are implementation detail: useful to understand current behavior or troubleshoot, never integration dependencies or compatibility promises.

Next: [custom cycle source](/en/advanced/custom-cycle-manager) or [execution paths](/en/advanced/execution-paths).
