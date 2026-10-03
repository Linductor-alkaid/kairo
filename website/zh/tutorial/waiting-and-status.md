---
title: 有界等待与状态快照
description: 以有界等待、WaitResult 和状态快照安全完成一次流水线收尾。
---

# 有界等待与状态快照

## 学习目标

在阶段切换或关闭前，以超时为边界等待异步任务，并根据 `WaitResult`、完成状态和执行器状态作出下一步处理。

## 场景问题

规划流水线准备切换地图或退出。无限期等待会让调用线程失去响应；只看一个布尔值又不足以诊断是队列仍有工作、任务正在运行，还是执行器已经停止。

## 推荐方案

新代码优先使用 `wait_for_completion_ex(timeout)`：

```cpp
auto result = executor.wait_for_completion_ex(std::chrono::milliseconds{200});
if (!result.completed) {
    // result.timed_out 为 true；result.status 是超时瞬间的状态快照。
    std::cerr << "pending=" << result.status.pending_tasks << '\n';
}
```

`WaitResult` 同时给出 `completed`、`timed_out`、传入的 timeout 和 `CompletionStatus`。超时时，Facade 会记录 `WaitTimeout`，可在可靠性专题中通过失败状态和回调统一处理。

## 选择等待接口

| 目的 | 接口 | 结果 |
| --- | --- | --- |
| 兼容旧调用方 | `wait_for_completion()` | 最多等待默认时长；超时不抛出。 |
| 只要完成/超时判断 | `try_wait_for_completion(timeout)` | `bool`。 |
| 以任意 chrono 时长表达边界 | `wait_for_completion_for(timeout)` | `bool`。 |
| 需要诊断超时 | `wait_for_completion_ex(timeout)` | `WaitResult` 与状态快照。 |

`is_idle()` 用于快速判断默认异步执行器当前是否空闲；`get_completion_status()` 提供初始化、排队、活跃和待完成数量的快照。需要确认执行器生命周期时，再查询 `get_async_executor_status()`。

## 状态快照的范围

`CompletionStatus` 只描述当前 Kairo 的默认异步执行器：active、queued 和 pending 不包含应用自建线程、通信 channel 中的数据、实时任务队列或外部 I/O。完整机器人流水线是否 idle 必须由应用汇总，而不能只看 `executor.is_idle()`。

状态查询是一个瞬时快照。它返回 idle 后，另一个生产者仍可能立即提交新任务；因此阶段切换和关闭必须先关闭提交入口，再等待，而不是反过来先轮询 idle。

## 为什么这样做

有界等待把“尚未完成”变成调用方可以处理的业务状态，而不是无期限卡住。状态快照还能区分执行器未初始化、队列积压与运行中任务，为重试、降级或故障报告保留事实。

## 失败如何观察

超时不是任务异常，也不代表任务已经取消；检查 `result.timed_out` 和 `result.status`，并查询失败状态中的 `wait_timeout_count`。单个任务的返回值和异常仍应由各自的 `future.get()` 处理。

当超时还可能涉及实时、Blocking I/O 或 GPU 后端时，在选择后续策略前采集完整 Kairo 现场：

```cpp
const auto result = executor.wait_for_completion_ex(std::chrono::milliseconds{200});
if (!result.completed) {
    const auto snapshot = executor.get_snapshot();
    // 持久化 lifecycle、后端状态、failures 和 snapshot.partial。
}
```

`get_snapshot()` 是 best-effort 诊断查询，不会取消或预留工作；它补充
`WaitResult::status`，后者的完成字段仍只覆盖默认异步任务。

## 正确收尾顺序

1. 停止产生新任务，例如取消不再需要的周期任务。
2. 以业务可接受的 timeout 调用 `wait_for_completion_ex()`。
3. 完成时调用 `shutdown(true)`；超时时记录 `WaitResult` 与完整 snapshot，并按业务策略重试、降级或调用 `shutdown(false)`。

不要在超时后假设任务已经停止：超时只说明它们尚未全部完成。

## 故障注入与退出决策

1. 提交一个已开始且超过 timeout 的任务；确认 `WaitResult` 超时，但任务仍可能继续运行并产生副作用。
2. 用单 worker 先运行阻塞任务，再排入多个短任务；确认快照同时显示 active 与 queued，便于区分“正在慢”与“仍在排队”。
3. 在等待期间保留一个生产者继续提交；观察排空条件不稳定，从而验证“先停生产者”的必要性。
4. 超时后分别演练继续等待、持久化未完成输入和 `shutdown(false)`，记录每种策略接受的数据后果。

应用应事先定义两类预算：单项请求等待预算，以及服务整体排空预算。前者超时不必立即关闭 Kairo；后者超时通常意味着进入降级或快速停止流程。

## 需求变化时如何演进

| 新需求 | 下一步选择 |
| --- | --- |
| 等一个具体结果 | 使用该任务 future 的有界等待，不用全局 completion |
| 等一组有依赖的工作 | 保留图的最终 future，并结合全局状态诊断 |
| 等通信数据被消费 | 查询/关闭相应 channel；Kairo pending 不包含它 |
| 等实时控制停止 | 调用实时停止并查询实时状态；普通 completion 不包含它 |
| 进程重启后继续未完成工作 | 将业务输入持久化；内存状态快照不能恢复任务 |

## 下一步阅读

至此，普通业务流水线只使用了 Facade。继续进入[完整机器人数据流水线](/zh/tutorial/complete-robot-pipeline)，看普通任务、长期线程和通信组件如何共同定义整体退出；统一失败回调与诊断见[可靠性专题](/zh/reliability/)。
