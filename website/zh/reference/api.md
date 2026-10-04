---
title: API 参考
description: 公开 API 的模块入口与稳定性边界。
---

# API 参考

本站以 `v0.5.3` 为稳定版本基线，包含统一自动路由、等待、通信、任务图和诊断能力；后续 `master` 的未发布能力不构成稳定承诺。完整签名、默认值、错误码和兼容语义只在仓库的 [`docs/API.md`](https://github.com/Linductor-alkaid/kairo/blob/master/docs/API.md) 维护，避免网站内容和仓库文档出现两套说法。

## 先按模块定位

| 模块 | Facade / 类型 | 学习入口 | 完整 API 文档 |
| --- | --- | --- | --- |
| 生命周期 | `instance`、独立实例、`initialize[_ex]`、`shutdown` | [初始化与关闭](/zh/quick-start/lifecycle) | `docs/API.md` 的生命周期与配置章节。 |
| 普通任务与路由 | `submit_auto`、`TaskOptions`、`RoutingDecision`、`submit` | [执行模型与路由边界](/zh/guides/execution-models-and-routing)、[任务输入与所有权](/zh/quick-start/task-inputs-and-ownership) | `Executor` 模板 API。 |
| 周期与批量 | `submit_periodic`、`TimerHandle::cancel`、周期状态、三种 batch | [延迟与周期](/zh/tutorial/delayed-and-periodic)、[批量](/zh/tutorial/batch) | Facade 定时与批量章节。 |
| 任务图 | `submit_with_handle`、`submit_after[_with_handle]`、`when_all` | [任务依赖](/zh/tutorial/dependencies) | 任务依赖章节。 |
| 失败与等待 | failure callback/status、recent failures、`wait_for_completion[_ex]`、完成状态 | [失败可观察性](/zh/reliability/failure-observability)、[有界等待](/zh/tutorial/waiting-and-status) | 失败、等待与类型章节。 |
| 监控 | `enable_monitoring`、采样率、任务统计 | [监控与采样](/zh/reliability/monitoring) | 监控 API 章节。 |
| 通信 | `kairo::comm`：channel、mailbox、snapshot、phase | [通信组件选型](/zh/guides/choosing-communication) | 通信 API 章节。 |
| 取消与定时 | `submit_cancellable*`、`request_task_cancel`、`TimerHandle`、`ScopedTimerHandle` | [取消与定时](/zh/realtime-and-communication/cancellation-and-timers) | 取消与定时 API 章节。 |
| 串行派发与总量 admission | `submit_on[_with_handle]`、`SerialExecutionContext`、`max_in_flight_tasks`、`CapacityExhaustedException` | [容量与告警](/zh/realtime-and-communication/capacity-and-alerting)、[事件循环互操作](/zh/guides/event-loop-interop) | admission 与串行派发章节。 |
| 实时 | 注册/启动 `_ex`、push、状态、任务列表 | [实时控制循环](/zh/realtime-and-communication/realtime-control) | 实时任务 API。 |
| GPU | 注册 `_ex`、`submit_gpu`、状态、`submit_auto`、scheduler | [GPU 专题](/zh/gpu/) | GPU API 与构建文档。 |
| 有界 dispatch 与 worker | `dispatch_auto`、`DispatchResult`、`start_worker`、`WorkerHandle` | [提交接口选型](/zh/guides/choosing-submit-api)、[Blocking I/O worker](/zh/realtime-and-communication/blocking-io-workers) | 路由与 Blocking I/O API。 |
| 高级 | `ExecutorManager`、执行器指针、`ICycleManager`、`LockFreeTaskExecutor` | [高级与原理](/zh/advanced/) | 高级接口与设计文档。 |

## Facade 覆盖索引

下表是发布前检查表：每一组公开 `Executor` Facade 至少有一个教学、选型或参考入口。它不替代重载签名。

| 接口组 | 默认入口 | 需要进一步确认 |
| --- | --- | --- |
| `instance`、独立构造、`initialize[_ex]`、`shutdown` | 快速开始 | 配置、资源隔离和关闭策略。 |
| `submit_auto`、`submit`、priority、delayed、periodic、batch | 场景教程与提交选型 | future、路由决策、周期状态、背压和 benchmark。 |
| handles、依赖、汇合 | 任务依赖教程 | 同实例限制、失败传播和任务图规模。 |
| failure、recent buffer、等待、完成快照 | 可靠性与等待教程 | `FailureKind`、`WaitResult` 与状态字段。 |
| 监控与统计 | 监控与采样 | 采样率和统计开销。 |
| realtime 注册、push、列表与状态 | 实时控制教程 | 权限降级、周期预算和拒绝计数。 |
| `submit_cancellable*`、`request_task_cancel`、`get_cancellation_status` | 取消与定时教程 | 协作语义、registry 容量与取消计数。 |
| `submit_delayed`、`submit_periodic`、`TimerHandle` / `ScopedTimerHandle` | 取消与定时教程 | cancel/reschedule、终态与定时计数。 |
| `submit_on`、`submit_on_with_handle`、`SerialExecutionContext` | 事件循环互操作指南 | FIFO ticket 顺序、非阻塞派发与 shutdown 拒绝。 |
| `max_in_flight_tasks`、`set/get_max_in_flight_tasks`、`get_in_flight_submissions` | 容量与告警 | 覆盖路径、capacity 拒绝语义与计数。 |
| `register_lockfree_executor` / `start_` / `stop_` / `get_lockfree_executor_names` | 提交接口选型、高级与原理 | 后端生命周期；`dispatch_auto` 的 `accepted` 只代表队列接收。 |
| 有界 dispatch、Blocking worker | 执行模型与路由边界 | admission 不等于完成、worker 生命周期。 |
| GPU 注册、提交、状态、自动调度 | GPU 教程 | 后端可用性、stream 与硬件验证。 |
| 直接 manager / executor 指针 | 高级接口 | 所有权、并发和生命周期责任。 |

## 状态与结果怎么读

不要把所有失败压成一个 `bool`。单次任务用 `future.get()`；可诊断控制操作用 `ExecutorResult`；持续趋势用 `ExecutorFailureStatus` 和最近事件；等待超时用 `WaitResult`；实时/GPU/通信各有自己的状态快照和统计。通信 `CommStats` 不会自动进入 `ExecutorFailureStatus`。

## 不属于普通手册的入口

`set_timer_thread_factory_for_test()` 是测试注入钩子，用于模拟定时器线程创建失败；它不是生产配置 API。`src/` 下 `ThreadPool`、调度器、队列和对象池是当前实现，不保证为稳定集成接口。需要理解它们时阅读[高级与原理](/zh/advanced/)，实际程序仍依赖 `include/kairo/` 下公开头文件。

## 下一步阅读

升级现有代码请阅读[版本与迁移](/zh/reference/version-and-migration)；不确定当前问题的默认入口时，先看[如何选择提交接口](/zh/guides/choosing-submit-api)。
