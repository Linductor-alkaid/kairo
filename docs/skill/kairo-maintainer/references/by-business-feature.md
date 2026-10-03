# Index By Business Feature

| Business intent or wording | Read first | Then verify |
| --- | --- | --- |
| Run normal finite work and obtain a result | [General submission](capabilities/general-submission.md) | `executor.hpp`, Facade tests |
| Start, configure, or stop an application runtime | [Facade and lifecycle](capabilities/facade-lifecycle.md) | `executor.cpp`, lifecycle tests |
| Run urgent work before routine work | [Scheduling and task graphs](capabilities/scheduling-task-graph.md) | priority scheduler tests |
| Retry later, run periodic maintenance, submit a batch, or compose dependencies | [Scheduling and task graphs](capabilities/scheduling-task-graph.md) | timer/task graph tests |
| Route work to a named low-latency, realtime, or GPU backend | [Routing](capabilities/routing.md) | routing tests and backend status |
| Build a periodic control loop with bounded intake | [Realtime control](capabilities/realtime-control.md) | realtime overflow and cycle tests |
| Own a long-running transport/read loop | [Blocking I/O workers](capabilities/blocking-io.md) | blocking I/O tests |
| Pass commands, snapshots, phases, or latest configuration between threads | [Communication primitives](capabilities/communication.md) | `test_comm_*` |
| Fan one in-process event stream out to independent consumers | [Communication primitives](capabilities/communication.md) | `topic.hpp`, `test_comm_topic` |
| Report failures, queue state, waits, or service health | [Observability](capabilities/observability.md) | snapshot/failure tests |
| Run a CPU/GPU dual path or manage GPU executors | [GPU execution](capabilities/gpu.md) | CUDA/OpenCL tests |
| Alter queues, worker dispatch, resizing, or lock-free behavior | [Concurrency internals](capabilities/concurrency-performance.md) | stress, TSAN, and benchmark evidence |
