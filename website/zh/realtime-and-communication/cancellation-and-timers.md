---
title: 取消与定时
description: 基于 StopToken 的任务协作取消与可取消、可重排的定时句柄——以及取消从不承诺的事。
---

# 取消与定时

Facade 为长期工作新增两项能力：**任务协作取消**（`submit_cancellable` +
`request_task_cancel`）与**定时句柄**（`TimerHandle` / `ScopedTimerHandle`）。
两者都是"请求"，不是"中断"。

可运行的完整演示见
[`examples/tutorial/13_cancellation_and_timers.cpp`](https://github.com/Linductor-alkaid/kairo/blob/master/examples/tutorial/13_cancellation_and_timers.cpp)：

<<< @/../examples/tutorial/13_cancellation_and_timers.cpp{1-17}

## 三种不同的承诺：排队超时、deadline、取消请求

这三个机制经常被混淆，但它们的承诺完全不同：

| 机制 | 谁触发 | 它做什么 | 它从不做什么 |
| --- | --- | --- | --- |
| 排队软超时（`task_timeout_ms`） | worker **开始执行任务前**的时间流逝 | 跳过任务；future 收到 `TimedOutException`；计入超时诊断 | 打断已经开始运行的任务 |
| `TaskOptions::deadline` | 仅为路由提示 | 只影响路由与诊断 | 自动触发取消或中断 |
| 显式取消（`request_task_cancel`） | 你的代码主动请求 | 排队中：任务不再执行，future 收到 `TaskCancelled(Explicit)`。运行中：置位任务的 `StopToken` | 抢占运行中的任务，或解除没有 wakeup 机制的阻塞调用 |

一句话总结：超时是线程池策略，deadline 是标签，取消是必须由任务配合响应的显式请求。

## 协作取消语义

- `submit_cancellable(f)` 把 `kairo::StopToken` 注入为 callable 的**首参数**；
  任务在工作步之间轮询 `token.stop_requested()`。
- 排队取消赢得唯一的仲裁点：任务不执行，future 以 `TaskCancelled(Explicit)`
  就绪，依赖它的任务收到 `TaskCancelled(DependencyCancelled)`，且**不记录
  failure 事件**——取消是生命周期事件，由 `get_cancellation_status()` 独立计数。
- 运行中取消只置位 token。之后正常返回的任务保留业务结果；观察到请求后抛出
  `TaskCancelled` 的任务按取消归类。**没有取消请求**时主动抛 `TaskCancelled`
  仍按任务失败统计，异常类型不能绕过 failure 体系。
- 重复/过期句柄幂等：运行中重复请求返回 `AlreadyRequested`，终态返回
  `AlreadyCompleted`，未知句柄返回 `NotFound`，都不写 failure。

## 定时句柄

`submit_delayed()` 与 `submit_periodic()`（以及注入
`StopToken` 的 `*_cancellable_*` 变体）返回可复制的 `TimerHandle`：

- 到期前 `cancel()`：返回 `CancelledBeforeDispatch`，任务不执行，future 收到
  `TaskCancelled(Explicit)`。
- 派发后 `cancel()`：返回 `CancellationRequestedAfterDispatch`——取消继续向
  排队/运行中的任务传播，而不是假装从未派发。
- `reschedule_after(ms)` 重排下一次到期（周期 timer 只改下一次触发时间、不改
  周期）；`delay_ms <= 0` 返回 `InvalidDuration`。
- 析构**不取消**。需要"析构即取消"时用 move-only 的 `ScopedTimerHandle` 包装。
- shutdown 时未到期的 delayed timer 以 `TaskCancelled(Shutdown)` 就绪；计数见
  `get_timer_status_summary()`。

## 串行上下文派发

需要将 FIFO 串行工作纳入 Kairo admission 时，可使用
`SerialExecutionContext` 与 `submit_on(context, fn)`。派发与结算分离：池 worker
只做有界非阻塞的 ticket 发布，业务 future 由串行线程直接结算，因此小型多
worker 池在突发提交下仍按 ticket FIFO 有界时间内前进，不会互相饥饿；排队取消、
超时与拒绝都会释放 ticket，不阻塞后续顺序。上下文关闭后拒绝新提交并排空已接收
任务；该 API 不绑定 asio strand，必须与外部 strand 同上下文销毁的对象仍由应用侧
管理。

## 不承诺的事

- **不抢占**：阻塞在无 wakeup 机制的系统调用或库调用里的任务不会被中断，取消
  不能强迫它停止。
- **不绑定 strand**：facade 定时器把到期工作派发到普通线程池。回调与销毁必须在
  同一外部事件循环 strand 上发生的 timer（例如 asio `steady_timer`）继续由应用
  侧管理——见[与外部事件循环互操作](/zh/guides/event-loop-interop)。
- **不承诺定时精度**：到期后派发到线程池，延迟取决于负载；用
  `benchmark_timer_precision` 实测，不要假设上界。

## 延伸阅读

- API 参考：`docs/API.md` §3.8–3.9（定时与取消）。
- 仲裁内部机制与设计：`docs/design/task_cancellation_and_timers.md`。
- 迁移指引（包括哪些自建定时可以迁移到 `TimerHandle`）：`docs/MIGRATION.md`。
