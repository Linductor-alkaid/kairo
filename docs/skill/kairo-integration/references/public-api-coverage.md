# Public API Coverage Matrix

This audit matrix is intentionally not linked from `SKILL.md`. Use it when upgrading Executor, adding a user-visible capability, or checking that the integration skill still maps every public header. It is not a normal task router.

| Public header or API family | Integration card | Scope |
| --- | --- | --- |
| `executor.hpp`, `config.hpp`, `types.hpp` | [Quick start](quick-start.md), [tasks and lifecycle](tasks-and-lifecycle.md), [scheduling](scheduling.md) | Facade, configuration, futures, lifecycle, task graph, scheduler APIs |
| `task_options.hpp`, `task_router.hpp`, `lockfree_task_executor.hpp` | [Routing and low-latency](routing-low-latency.md) | Named bounded dispatch, routing decisions, direct lock-free worker |
| `blocking_io.hpp` | [Blocking I/O](blocking-io.md) | Interruptible dedicated worker |
| `comm.hpp`, `comm/*.hpp` | [Communication](communication.md) | FIFO/latest/realtime channels, snapshots, phases, sequencing, allocation diagnostics |
| `interfaces.hpp`, `executor_manager.hpp` | [Advanced extensions](advanced-extensions.md) | Custom cycle and executor composition, lifecycle snapshots |
| `monitor/*.hpp` and Facade failure/snapshot APIs | [Observability](observability.md) | Failure events, status, metrics, snapshots |
| `gpu/device_query.hpp`, `gpu/gpu_scheduler.hpp` | [GPU](gpu.md) | Device discovery and CPU/GPU selection |
| `gpu/kernel_launch_optimizer.hpp`, `gpu/task_scheduler_optimizer.hpp`, `gpu/transfer_optimizer.hpp` | [GPU](gpu.md) | Expert standalone optimization helpers |

## Coverage Rule

Every row must point to a card with a minimal usage path, observable success/failure surface, and integration boundary. Add a row and a card update in the same change as any new public header or public Facade capability.
