---
title: 让控制命令优先
description: 使用 submit_priority 让少量控制工作在普通分析之前进入队列。
---

# 让控制命令优先

## 学习目标

在同一条机器人流水线中，让紧急 `ControlCommand` 比普通帧分析更早排队，同时保留 `future` 的结果和异常观察路径。

## 场景问题

传感器帧解析可以稍后开始，但一条停止或限速命令不应被普通分析任务长期挡在队列后面。

## 推荐方案

使用 `submit_priority(priority, task)`。优先级从 `0`（低）到 `3`（关键）；普通任务使用默认优先级。下面的示例以单工作线程演示两类工作均可正确完成。

<<< @/../examples/tutorial/02_priority.cpp{4,7-19,21-24}

完整源码：[`examples/tutorial/02_priority.cpp`](https://github.com/Linductor-alkaid/kairo/blob/master/examples/tutorial/02_priority.cpp)。

```bash
./build/examples/tutorial/tutorial_02_priority
```

## 预期输出

```text
priority tasks=analysis,control
```

## 为什么这样做

- `submit_priority()` 仍返回 `future`；调用 `get()` 可获得结果，也可重新抛出任务异常。
- 优先级只影响等待队列的取出顺序；已经开始运行的低优先级任务不会被抢占。
- 多工作线程、已有执行中的任务和同优先级 FIFO 都会影响实际观察到的完成顺序，因此不要把这个示例的输出顺序当作调度时序证明。

## 优先级任务如何接收输入

完整形式是 `submit_priority(priority, fn, args...)`：只有最前面的 priority 是额外调度信息，后面的 callable 与参数保存规则和 `submit(fn, args...)` 相同。

```cpp
ControlCommand command = read_command();
auto applied = executor.submit_priority(
    3, apply_control_command, command);
```

`command` 会按值保存到任务中。若改用 lambda，也优先按值捕获命令或捕获稳定 owner；提高优先级不会缩短引用必须存活的时间，也不会让裸 `this`、指针或共享可变状态变安全。

## 运行假设与所有权

示例固定一个 worker，只提交一个分析任务和一个控制任务，目的是验证两种 future 都能完成，而不是证明控制任务一定先完成。任务返回短字符串，没有捕获外部对象；真实 `ControlCommand` 应按值或用明确所有权传入，不能引用提交函数栈上的临时对象。

优先级整数最终映射到四档：`priority <= 0` 为 LOW，`1` 为 NORMAL，`2` 为 HIGH，`priority >= 3` 为 CRITICAL。应用应定义一张全局等级表，不要让不同模块用任意大数互相“加价”。如果最高级任务长期占据大多数提交，普通任务可能持续饥饿。

## 失败如何观察

- 空任务或关闭后提交会形成可观察的拒绝/异常结果；不要忽略返回的 `future`。
- 控制任务本身失败时，在其 `future.get()` 处处理；长期运行的统一失败统计将在[可靠性专题](/zh/reliability/)展开。

## 故障注入与退出

按以下方式验证设计，而不是只看一次正常输出：

1. 先提交一个会阻塞 worker 的 LOW 任务，再提交 CRITICAL 任务；确认后者不能抢占已经运行的工作。
2. 让控制任务抛异常；确认它只在自己的 future 和失败状态中出现，不会被分析任务的成功掩盖。
3. 用小队列持续提交最高级任务；观察拒绝、queued 数和普通任务等待时间。
4. 关闭时先停止控制与分析生产者，再消费仍持有的 futures，最后有界排空 Kairo。

若业务要求“控制命令在 5 ms 内生效”，需要测量排队时延并设计超时/降级；把等级改成 CRITICAL 不是验收标准。

## 需求变化时如何演进

| 新需求 | 下一步选择 |
| --- | --- |
| 只要紧急工作更早排队 | 保持 `submit_priority()`，监控各等级等待时间 |
| 必须在地图加载后执行 | 使用任务依赖，优先级不承担正确性 |
| 命令旧了就不能执行 | 在任务内校验 deadline/sequence，并设计拒绝结果 |
| 有固定周期和 jitter 预算 | 迁移到专用实时任务 |
| 控制输入持续流入 | 用有界通信组件表达背压，不用无限提交任务 |

## 何时不选

需要确定的周期、抖动预算、CPU 亲和性或运行中抢占时，不要提高普通线程池优先级来模拟实时性；请进入[专用实时线程教程](/zh/realtime-and-communication/realtime-control)。

## 下一步阅读

[延迟重试与健康检查](/zh/tutorial/delayed-and-periodic)处理“现在不做”和“定期做”两种新问题。
