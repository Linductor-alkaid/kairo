---
title: 实时与通信
description: 按数据语义选择专用实时线程和跨线程通信组件。
---

# 实时与通信

普通 `submit_auto(lambda)` 和 `submit_periodic()` 走默认异步路径；固定周期控制、周期预算、实时队列或长期可中断 I/O 才需要进入本专题。它们不是同一个抽象层。

如果还没有看过组件如何组合，先阅读[完整机器人数据流水线](/zh/tutorial/complete-robot-pipeline)：它先建立角色、数据所有权和退出协议，再把每条边映射到具体组件。

1. [阻塞 I/O worker](/zh/realtime-and-communication/blocking-io-workers)：通过 `start_worker()` 管理长期阻塞循环的所有权、唤醒与 join；协议仍由应用定义。
2. [启动专用实时控制循环](/zh/realtime-and-communication/realtime-control)：把综合示例中的普通周期模拟替换为可诊断实时 Facade。
3. [选择消息传递方式](/zh/realtime-and-communication/channels)：普通帧流与实时周期内有限 drain。
4. [最新值、状态快照与阶段同步](/zh/realtime-and-communication/state-and-phases)：配置、完整状态与启动顺序。
5. [通信可观察性](/zh/realtime-and-communication/observability)：统一理解 `CommStats` 与本地事件 callback。
6. [容量评估与告警处理](/zh/realtime-and-communication/capacity-and-alerting)：把累计统计转换成窗口速率、消费余量、告警级别和处置动作。
7. [取消与定时](/zh/realtime-and-communication/cancellation-and-timers)：基于 `StopToken` 的协作取消与可取消、可重排的定时句柄——以及取消从不承诺的事。

完整 API 见 [`docs/API.md`](https://github.com/Linductor-alkaid/kairo/blob/master/docs/API.md)；综合示例的可运行代码是 [`examples/comm_robot_pipeline.cpp`](https://github.com/Linductor-alkaid/kairo/blob/master/examples/comm_robot_pipeline.cpp)。
