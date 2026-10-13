---
title: 如何选择提交接口
description: 先区分完成、接收和生命周期结果，再按业务约束进入默认 Facade 或专家路径。
---

# 如何选择提交接口

不要先从执行器类名开始。先问调用方在这次操作后需要确认什么：一项工作已经完成、一个有界队列已经接收，还是一个长期 worker 已经启动并可管理。这个答案比“哪个后端更快”更早决定接口。

## 30 秒选择表

| 业务问题 | 默认接口 | 返回结果表示 | 何时进一步了解 |
| --- | --- | --- | --- |
| 一次性后台工作 | `submit_auto(lambda)` | `future` 的完成或异常 | 需要 priority、delay、batch 或 dependency |
| 确认无约束的极热短任务（0.7.1） | `submit_fast(lambda)` | `future` 的完成或异常 | 使用前读下方边界说明 |
| 独立 CPU/GPU 实现 | `submit_auto(cpu_gpu_task(...))` | 已选路径的 future 完成或异常 | 需要后端注册、GPU 诊断或调参 |
| 已验证的无锁单消费者路径 | `dispatch_auto(LowLatency)` | 有界队列接收 | 需要容量、对象池和关闭细节 |
| 已存在的周期实时队列 | `dispatch_auto(RealtimeQueue)` | 有界队列接收 | 需要周期预算、drop 与平台权限 |
| 长期、可唤醒的 I/O 循环 | `start_worker(BlockingWorkerSpec)` | worker 启动和生命周期 | 需要协议、重连、设备与部署细节 |
| 软周期维护 | `submit_periodic()` | task ID 与周期状态 | 需要严格周期或低 jitter |
| 依赖、延迟、优先级或批量 | 对应显式 Facade API | future、handle 或 task ID | 需要组合调度语义 |

`submit_auto(lambda)` 是普通开发者的默认入口。它当前安全地选择默认异步线程池，不会因为 priority、deadline 或压力自动转投 GPU、无锁或实时后端。0.7.0 起 CpuOrGpu / affinity 路由消费的能力快照经 TTL 缓存（默认 100ms；后端注册/启停立即失效），命中路径不再逐次锁注册表采集。

不确定名称、intent、fallback 与 capability snapshot 如何共同决定目标时，先读[自动路由如何匹配目标](/zh/guides/execution-models-and-routing)，再继续按场景选型。

## 先看调用方要确认什么

```mermaid
flowchart TD
    A{调用方需要确认什么?}
    A -- 工作完成或异常 --> B[future 路径]
    B --> C[默认: submit_auto(lambda)]
    C --> D{是否有明确附加约束?}
    D -- priority/delay/batch/dependency --> E[对应显式 Facade API]
    D -- 独立 CPU/GPU 实现 --> F[cpu_gpu_task + submit_auto]
    A -- 队列是否接收 --> G[dispatch_auto]
    G --> H[必须指定运行中的 LowLatency 或 RealtimeQueue 后端]
    A -- 长期 worker 是否启动/停止 --> I[start_worker]
    I --> J[WorkerHandle 生命周期]
```

三条路径不能互相替代：`future` 就绪不表示实时周期已运行；`DispatchResult::accepted` 不表示任务完成；`WorkerHandle::started()` 不表示 I/O 协议、设备或第一条数据已经就绪。

## 默认选择：`submit_auto(lambda)`

当工作是有限的一次性计算，且调用方需要返回值或异常时，从此入口开始：

```cpp
auto future = executor.submit_auto([frame] { return decode(frame); });
auto decoded = future.get();
```

按值捕获 `frame` 让任务拥有稳定输入。`future.get()` 是完成和异常传播边界；`get_last_routing_decision()` 则解释默认 Facade 为什么选择该路径。需要直接控制已有线程池语义或维护兼容代码时，`submit()` 仍是有效的显式入口。

## 何时用 `submit_fast`（以及何时不该用）

`submit_fast(lambda)` 是 0.7.1 引入的零路由快速路径：等价于以 NORMAL 优先级直接提交默认池，且**不产出路由决策**（不进入 `SchedulingMetrics` 与 recent decisions）。配对基准实测比 `submit_auto` 裸 builder 快约三成。

只应同时满足以下条件时使用：

- 任务确定是 CPU-only，永远不会需要 GPU / 无锁 / 实时后端；
- 不需要 deadline、QoS、affinity、resource 约束与自定义优先级；
- 不依赖路由决策做可观测性或审计。

任何一条不满足，就用 `submit_auto`（保留意图路由与结构化诊断）或对应显式 API。它不是默认推荐入口——路由可观测性是 0.6.x 以来调度体系的排障基础，为省几十纳秒放弃它之前，先确认该路径真的足够热。

## 何时使用显式 future API

`submit_priority()`、`submit_delayed()`、`submit_periodic()`、`submit_batch()` 和任务依赖 API 不应被自动路由猜测，因为它们表达的是业务调度语义：

- priority 只改变等待队列，不提供抢占、deadline 或完成顺序；
- delayed 表示最早运行时机，periodic 是普通线程池上的软周期维护；
- batch 只适用于相互独立、拥有相同调度语义的工作；
- `TaskHandle`、`submit_after()` 和 `when_all()` 表达成功依赖，不要用优先级或隐藏的 `future.get()` 模拟。

这些 API 仍返回 future、handle 或 task ID；按各自的语义观察失败，不要把它们当成“只是另一种 submit”。

## 有界接收：只在约束已知时用 `dispatch_auto`

`dispatch_auto()` 不是普通任务的加速按钮。只有业务已经确认单消费者无锁路径或实时周期队列的背压语义时，才显式声明 intent 和目标名称：

```cpp
TaskOptions options;
options.intent = ExecutionIntent::RealtimeQueue;
options.preferred_executor = "control";
const auto result = executor.dispatch_auto(options, [] { apply_control(); });
if (!result.accepted) {
    // 读取 result.decision、result.message、failure event 和状态计数
}
```

未启动、队列满、对象池耗尽和并发停止都可能拒绝投递；不会静默回退线程池。进入[执行模型与路由边界](/zh/guides/execution-models-and-routing)了解接收与完成的区别，再按需阅读实时或无锁专题。

## 长期工作：使用 `start_worker`

永久监听、阻塞 read/poll 或协议服务循环不应占用共享 worker。若循环能响应 stop token 且 `wakeup()` 能解除当前等待，使用 `start_worker(BlockingWorkerSpec{...})` 获取 `WorkerHandle`；由它控制启动结果、`request_stop()`、`stop()` 和状态。完整的 worker 契约见[Blocking I/O worker](/zh/realtime-and-communication/blocking-io-workers)。

## 自动路由的边界

自动路由不会验证 callable 的实时安全、线程安全、GPU 内存所有权或 I/O 可中断性。`get_executor_capabilities()` 也是建议性快照，实际提交仍会受并发 stop 和容量变化影响。需要 CPU/GPU 回退时，必须用独立双路径 callable 并明确 `FallbackPolicy`；允许回退由 `RoutingDecision` 解释，不是任务异常。

## 下一步阅读

先阅读[执行模型与路由边界](/zh/guides/execution-models-and-routing)，弄清完成、接收和 worker 生命周期的区别；随后按明确需求进入[实时控制](/zh/realtime-and-communication/realtime-control)、[GPU 与降级](/zh/gpu/)或[Blocking I/O worker](/zh/realtime-and-communication/blocking-io-workers)。任务输入所有权见[提交自己的函数与数据](/zh/quick-start/task-inputs-and-ownership)。
