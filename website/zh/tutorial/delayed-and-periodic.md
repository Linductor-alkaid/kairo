---
title: 延迟重试与健康检查
description: 用 Facade 的延迟和软周期任务处理设备重试与后台健康检查。
---

# 延迟重试与健康检查

## 学习目标

用 `submit_delayed()` 安排一次设备重试，用 `submit_periodic()` 运行健康检查，并通过状态查询与 `cancel_task()` 结束周期工作。

## 场景问题

设备暂不可用时，立即重试只会制造额外负载；另一方面，后台健康检查需要隔一段时间重复运行，但必须能被停止并观察到是否真正运行过。

## 推荐方案

这两种工作都使用 `Executor` Facade，而不是直接操作底层线程池：

<<< @/../examples/tutorial/03_delayed_periodic.cpp{1-36}

完整源码：[`examples/tutorial/03_delayed_periodic.cpp`](https://github.com/Linductor-alkaid/kairo/blob/master/examples/tutorial/03_delayed_periodic.cpp)。

```bash
./build/examples/tutorial/tutorial_03_delayed_periodic
```

## 预期输出

```text
retry complete
health checks=1
periodic status=running, cancelled=yes
```

健康检查次数受调度影响，实际值可能不同；关键事实是取消前状态存在且 `execution_count > 0`。

## 状态与取消

- `submit_periodic()` 返回任务 ID；保存它，才能调用 `cancel_task()`。
- `get_periodic_task_status(id)` 返回单个仍在注册的任务状态；取消后会得到空结果。
- `get_all_periodic_task_status()` 适合监控页或关闭前检查所有仍注册的周期任务。
- 延迟任务返回 `future`，因此其返回值、执行异常和拒绝提交仍由 `get()` 观察。

## 周期任务如何接收业务输入

`submit_delayed(delay, fn, args...)` 与显式 `submit(fn, args...)` 一样，可以把函数和参数分别传入。`submit_periodic(period, task)` 不同：它只接收可重复调用的 `std::function<void()>`，没有单独的参数列表，也没有逐次返回值。需要输入时，应先用 lambda 把输入绑定到这个无参数 callback：

```cpp
auto device = std::make_shared<DeviceClient>(endpoint);
const auto task_id = executor.submit_periodic(1000, [device] {
    device->check_health();
});
```

callback 必须可复制，移动独占的 `unique_ptr` lambda 通常不能直接转换为 `std::function<void()>`；需要长期独占资源时，让一个稳定 owner 持有它，或捕获 `shared_ptr`。按引用捕获只适用于能够保证“取消后等待所有在途 callback，再销毁对象”的场景。

每次周期触发都会调用同一个闭包，因此闭包中的可变状态可能被多次甚至重叠访问。若需要累计状态，使用原子、mutex 或应用级 single-flight；不要把普通成员变量的可变引用捕获进去后假设每次调用天然串行。

## 运行假设与所有权

示例的延迟只有 `1 ms`，周期为 `5 ms`，并以最多 `2 s` 的有界轮询等待第一次健康检查完成，而不是睡眠固定时长后直接断言；这些值只是让 smoke test 快速完成，不是设备重试参数。调度时间使用相对时长，表示“最早到期时间”，到期后仍要等待共享线程池可用。

周期 callback 捕获 `health_checks` 的引用，因此该原子对象必须活到取消任务、停止 timer 并完成在途 callback 之后。长期服务中，task ID 的 owner 通常也是 callback 所依赖状态的生命周期 owner。

当前 timer 在每次到期时把 callback 提交给普通线程池，不等待上一次执行完成。如果 callback 执行时间大于 period，同一个周期任务可能有多个在途执行；有状态 callback 必须自行保证并发安全，或增加应用级 single-flight 防护。软周期接口不会自动串行化每次调用。

## 为什么这样做

延迟与周期调度属于 Facade 的能力：调用方不需要取得线程池对象或管理计时器线程。周期任务没有逐次 `future` 可保存，至少应保留任务 ID、查看执行/失败状态，并在不再需要时取消它。

## 失败如何观察

周期回调的异常不会由调用 `submit_periodic()` 的位置返回；运行中的任务应检查周期状态，并在长期服务中配置失败状态或回调。`cancel_task()` 返回 `false` 表示 ID 已不存在、已取消或不是当前实例的周期任务，不能把它当作已经安全停止的证明。

取消成功只会移除后续周期调度；已经提交到线程池或正在运行的 callback 仍可能完成。需要销毁 callback 捕获的对象时，取消后还要有界等待普通任务排空。

## 故障注入与退出

1. 让 retry 抛异常，确认延迟 future 在 `get()` 处重新抛出。
2. 让健康检查每次执行 `20 ms`、period 保持 `5 ms`，观察重叠执行和队列增长；不要以增加队列容量作为修复。
3. 让 callback 连续失败，检查 `failed_count`、`consecutive_failure_count` 和 `last_error_message`，并定义达到阈值后的停止或降级。
4. 在 delayed task 到期前开始 shutdown，确认 future 得到“timer stopped before delayed task execution”的可观察拒绝，而不是永久等待。

推荐退出顺序是：停止创建新的延迟/周期工作，取消所有保存的 task ID，等待已经提交的 callback，最后关闭 Kairo。若 callback 内有网络或设备 I/O，它们必须有自己的超时。

## 需求变化时如何演进

| 新需求 | 下一步选择 |
| --- | --- |
| 重试间隔需要指数退避 | 每次失败后按策略提交下一次 delayed task，并限制总次数 |
| 同一健康检查不能重入 | 增加 single-flight 状态，或由一个长期 owner 串行调度 |
| 多实例争抢同一外部资源 | 在业务层加租约/幂等协议，周期 API 不提供分布式互斥 |
| 需要固定相位与 jitter 预算 | 使用专用实时任务，不使用 `submit_periodic()` |
| 停机后需要恢复未到期重试 | 将计划持久化；内存 timer 不承担恢复保证 |

## 何时不选

`submit_periodic()` 是软周期后台调度：它不承诺控制循环的准点性，也不表达单周期预算。需要处理 CAN 或电机控制等严格周期工作时，应使用专用实时执行器。

## 下一步阅读

[批量处理一组传感器帧](/zh/tutorial/batch)处理同一时刻提交很多相似工作的情况。
