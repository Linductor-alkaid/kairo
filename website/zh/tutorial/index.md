---
title: 循序教程
description: 通过机器人数据流水线与服务端数据导入逐步学习 Kairo Facade。
---

# 循序教程

教程使用同一条机器人数据流水线：采集 `SensorFrame`、解析 `ParsedFrame`、生成 `Plan`，并把 `ControlCommand` 送入控制循环。每章只增加一个业务问题和一组必要 API。

从[第一个任务](/zh/quick-start/first-task)开始，并先掌握[如何提交自己的函数与数据](/zh/quick-start/task-inputs-and-ownership)，然后沿着下面的业务问题推进：

1. [让控制命令优先](/zh/tutorial/priority)：普通分析与紧急控制命令共用线程池。
2. [延迟重试与健康检查](/zh/tutorial/delayed-and-periodic)：稍后重试设备，并取消可观察的软周期任务。
3. [批量处理传感器帧](/zh/tutorial/batch)：根据是否需要 future 选择批量提交路径。
4. [加载、感知与规划依赖](/zh/tutorial/dependencies)：让规划在前置工作完成后执行。
5. [有界等待与状态快照](/zh/tutorial/waiting-and-status)：在切换阶段或关闭前可靠收尾。
6. [用 Topic 扇出事件流](/zh/tutorial/topic-subscriptions)：为独立 consumer 提供各自有界队列、背压策略和关闭所有权。
7. [完整机器人数据流水线](/zh/tutorial/complete-robot-pipeline)：把启动依赖、帧流、配置、控制命令、状态快照、诊断和退出组合起来。
8. [服务端数据导入案例](/zh/tutorial/service-data-import)：验证依赖、批量、逐项失败和有界关闭同样适用于服务端请求。

前五章分别引入一种普通任务问题；完整案例进一步连接 `examples/comm_robot_pipeline.cpp`，并明确哪些部分只是可移植教学模拟、哪些必须在生产中替换为实时 Facade、停止协议与过载策略。

每章都按同一组问题深化：示例规模和并发假设是什么、异步对象由谁拥有、失败如何注入、退出时还会有哪些在途工作，以及需求变化后应该继续使用当前接口还是更换抽象。阅读时可直接把这些小节当作项目评审清单。

服务端案例不是第二套 API 教程，而是对教学模型的交叉验证：机器人场景强调长期角色、通信和实时边界；数据导入场景强调请求生命周期、部分失败、事务、幂等和下游容量。

需要直接按问题选接口时，先阅读[执行模型与路由边界](/zh/guides/execution-models-and-routing)，再进入[场景指南](/zh/guides/choosing-submit-api)；只有约束明确时才逐项深入[实时与通信](/zh/realtime-and-communication/)、GPU 或高级专题。
