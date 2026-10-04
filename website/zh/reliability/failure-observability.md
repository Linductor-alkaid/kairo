---
title: 失败可观察性
description: 将 future、failure callback、状态计数和最近事件组合为长期运行服务的失败观察路径。
---

# 失败可观察性

## 学习目标

区分结果、失败趋势和最近诊断事件；即使调用方暂时不消费 `future`，任务异常仍不会静默消失。

## 路由与失败是两条观察路径

自动路由增加“为什么选择该后端”的解释，但不改变失败模型。`RoutingDecision` 不表示任务已完成或被接收；failure event 不负责解释允许的 CPU fallback。先按问题选择观察对象：

| 你要回答的问题 | 默认入口 | 适用范围 |
| --- | --- | --- |
| 这次调用成功了吗，结果是什么？ | `future.get()` | 单个有返回值的任务；异常在此重新抛出。 |
| 为什么选择或拒绝这条路由？ | `get_last_routing_decision()` / routing callback | `submit_auto()`、`dispatch_auto()` 的意图、回退与预检解释。 |
| 有界队列接收了吗？ | `DispatchResult::accepted` | 无锁或实时的单次 admission；不表示完成。 |
| 长期 worker 启动或停止了吗？ | `WorkerHandle` 与 worker status | 启动结果、生命周期与退出原因；不表示协议就绪。 |
| 服务刚发生了什么失败？ | `set_failure_callback()` | 立即桥接日志、告警或自己的遥测系统。 |
| 某类失败累计了多少？ | `get_failure_status()` | 健康检查、仪表盘和阈值告警。 |
| 最近几次失败的上下文是什么？ | `get_recent_failures()` | 故障诊断、支持包和有限历史查看。 |

## 推荐方案

在初始化后设置 callback，提交任务后仍在需要结果的边界调用 `get()`：

<<< @/../examples/tutorial/06_observability.cpp{1-29}

完整源码：[`examples/tutorial/06_observability.cpp`](https://github.com/Linductor-alkaid/kairo/blob/master/examples/tutorial/06_observability.cpp)。

```bash
./build/examples/tutorial/tutorial_06_observability
```

## 预期输出

```text
failures=1, callback=1, recent=1
```

`future.get()` 仍是单次任务的结果和异常边界；routing decision、callback、计数和最近事件各自提供解释或服务级观察，不能互相替代。

`set_routing_callback()` 与 failure callback 一样会隔离回调异常。routing buffer 的容量独立于 failure buffer；允许 CPU fallback 应保留 `fell_back = true` 和 `FallbackPolicy` 解释，但不增加用户任务失败计数。

## 最近事件的保留策略

- `get_recent_failures(0)` 返回当前缓冲的全部事件；传入正数只返回最新的指定数量。
- `set_recent_failure_capacity(n)` 设置 ring buffer 容量。容量为 `0` 时不保留事件，但累计计数和 callback 仍生效。
- `clear_recent_failures()` 只清空诊断缓冲，**不会**重置 `get_failure_status()` 的累计计数。

长期运行服务应根据内存预算和排障窗口设置容量；不要将无限增长的历史保存在进程内。

## 回调边界

failure callback 运行在 Kairo 的失败记录路径上。保持它短小、无阻塞并自行处理外部 I/O；callback 自身抛出的异常会被隔离，不会终止 worker 或后台线程。需要复杂处理时，只投递一条事件到你自己的日志/告警队列。

## 不同失败不是同一件事

`TaskException`、`SubmitRejected`、`WaitTimeout`、实时 drop、GPU failure 和安全调优回退都可进入 `ExecutorFailureStatus`，但含义不同。任务异常需要处理业务结果；等待超时表示尚未完成；调优回退可能仍然安全运行。路由预检快照也不是投递 reservation：实际的 stop、队列满和对象池耗尽仍应产生 `DispatchResult` / future 拒绝及相应 failure event。通信组件事件默认停留在 `kairo::comm` 本地 callback 与统计中，不会自动触发这个 callback。

## 下一步阅读

[监控与采样](/zh/reliability/monitoring)关注吞吐、成功/失败与执行时间等趋势；[有界等待与状态快照](/zh/tutorial/waiting-and-status)说明如何处理等待超时。
