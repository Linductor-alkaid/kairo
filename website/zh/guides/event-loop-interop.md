---
title: 与外部事件循环互操作
description: 把 io_context 或 strand 式循环托管为 worker，认清哪些派发是 executor 看不见的，并用 PhaseGate 完成批次收尾。
---

# 与外部事件循环互操作

应用可能已经拥有自己的事件循环——典型是 asio 的 `io_context` 与 `strand`。
executor 核心库不依赖 asio 或任何第三方事件循环；本指南描述两者如何正确协作、
哪些派发是 executor 看不见的、以及让盲区保持安全的纪律。

完整指南见
[`docs/external_event_loop_interop.md`](https://github.com/Linductor-alkaid/kairo/blob/master/docs/external_event_loop_interop.md)。
可编译的伴随示例（用互斥量 + 条件变量复现 strand 语义，不引入任何 SDK 依赖）是
[`examples/event_loop_interop.cpp`](https://github.com/Linductor-alkaid/kairo/blob/master/examples/event_loop_interop.cpp)：

<<< @/../examples/event_loop_interop.cpp{14-40}

## 模式 1：把循环托管为 Blocking I/O worker

围绕 `io_context::run()` 实现 `IBlockingIoWorker::run(StopToken)`，实现
`wakeup()` 让停止路径能唤醒循环。之后 `Executor::start_worker()` 拥有线程生命
周期、命名与停止顺序，循环也会出现在 `BlockingIoExecutorStatus` 与
`get_snapshot()` 中。

<<< @/../examples/event_loop_interop.cpp{118-128}

## 模式 2：strand 延续合法但不可见

线程池任务可以把延续 post 回 strand，但必须认清这意味着什么：

- `asio::post(strand, ...)` 不经过任何 executor 提交路径——没有 admission 判断，
  也没有排队计数。
- 被 post 的回调不出现在 `TaskStatistics`、在途诊断或 `ExecutorSnapshot` 中。
- 回调里抛出的异常不进入 executor 失败事件；asio 会吞掉或因此终止。

盲区内的纪律：跨界状态用 `shared_ptr` 显式移交，移交后原线程不再访问；延续内部
自行捕获异常；需要 admission、背压或失败计量的工作必须留在 executor 提交 API 上，
只把轻量延续 post 到 strand。

## 模式 3：用 PhaseGate 收尾批次

不要用轮询加 sleep 检测批次完成。串行侧每完成一步推进 `comm::PhaseGate` 相位；
任意线程都可以带超时地等待：

<<< @/../examples/event_loop_interop.cpp{164-178}

## 边界上的取消与定时

- 需要协作取消的任务必须走 executor 提交 API（`submit_cancellable` +
  `StopToken`）。executor 的取消不会伸进 asio 的内部等待。
- 不依赖 strand 所有权的自建 `sleep_until` 循环可以迁移到
  `submit_delayed_with_handle()` / `submit_periodic_with_handle()`（见
  [取消与定时](/zh/realtime-and-communication/cancellation-and-timers)与
  `docs/MIGRATION.md`）。
- 回调与销毁必须发生在同一 strand 上的 timer，在外部上下文定时器绑定通过评审
  （T2）之前继续由应用侧管理。`SerialExecutionContext` 只提供 FIFO 任务纳管，
  不改变 facade 定时器的执行或销毁上下文。此类 timer 暂不要迁移到 facade 句柄。

## 延伸阅读

- [取消与定时](/zh/realtime-and-communication/cancellation-and-timers)：取消语义本身。
- [阻塞 I/O worker](/zh/realtime-and-communication/blocking-io-workers)：完整 worker 契约。
- `docs/external_event_loop_interop.md`：含 asio 映射表的完整指南。
