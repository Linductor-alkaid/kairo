# Index By Implementation

| Symbol, path, or test family | Capability card | Search terms |
| --- | --- | --- |
| `Executor`, `ExecutorManager`, `initialize`, `shutdown` | [Facade and lifecycle](capabilities/facade-lifecycle.md) | `executor.hpp`, `executor.cpp`, `executor_manager.cpp` |
| `submit_auto`, `submit`, `wait_for_completion` | [General submission](capabilities/general-submission.md) | `test_executor_facade`, `test_wait_*` |
| `submit_priority`, `submit_delayed`, `submit_periodic`, `TaskHandle` | [Scheduling and task graphs](capabilities/scheduling-task-graph.md) | `priority_scheduler`, `task_dependency_manager` |
| `IScheduler`, `DefaultScheduler`, `set_scheduler`, `QosClass`, EDF, `DeadlineMissed` | [Scheduling Runtime](capabilities/scheduling-runtime.md) | `scheduling.cpp`, `scheduler.cpp`, `test_scheduling_runtime` |
| `dispatch_auto`, `TaskOptions`, `TaskRouter`, `RoutingDecision` | [Routing](capabilities/routing.md) | `task_router.cpp`, `test_executor_auto_routing` |
| `RealtimeThreadExecutor`, `register_realtime_task`, `push_realtime_task` | [Realtime control](capabilities/realtime-control.md) | `test_realtime_*`, `test_realtime_push_task_contract` |
| `BlockingWorkerSpec`, `WorkerHandle`, `IBlockingIoWorker` | [Blocking I/O workers](capabilities/blocking-io.md) | `blocking_io_executor`, `test_blocking_io_*` |
| `MpscChannel`, `SpscChannel`, `Topic`, `TopicSubscription`, `LatestMailbox`, `DoubleBuffer`, `PhaseGate` | [Communication primitives](capabilities/communication.md) | `include/kairo/comm`, `test_comm_*` |
| `ExecutorFailureEvent`, `CompletionStatus`, `ExecutorSnapshot`, monitor | [Observability](capabilities/observability.md) | `test_executor_snapshot`, `test_executor_failure_observability` |
| `GpuExecutorConfig`, `submit_auto(cpu_gpu_task)`, CUDA, OpenCL | [GPU execution](capabilities/gpu.md) | `src/kairo/gpu`, `test_cuda_*`, `test_opencl_*` |
| `ThreadPool`, `TaskDispatcher`, `LockFreeQueue`, resize, steal | [Concurrency internals](capabilities/concurrency-performance.md) | `thread_pool/`, `test_*resize*`, `test_lockfree_*` |
