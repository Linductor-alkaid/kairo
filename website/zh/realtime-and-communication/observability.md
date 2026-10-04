---
title: 通信可观察性
description: 用 CommStats 和 CommEventCallback 观察通信背压、覆盖、陈旧读取、延迟与时序错误。
---

# 通信可观察性

## 学习目标

把通信错误视为组件本地的协议状态，并通过 `stats()` 与 `set_event_callback()` 观察它们，而不是误以为都会出现在 `ExecutorFailureStatus`。

## 两个入口

```cpp
channel.set_event_callback([](const kairo::comm::CommEvent& event) {
    report_comm_event(event.component_name, event.message);
});

const auto stats = channel.stats();
if (stats.dropped_count != 0 || stats.timeout_count != 0) {
    raise_backpressure_alert(stats);
}
```

`CommStats` 是本地累计快照，可包含发送、接收、drop、覆盖、stale read、关闭后发送、超时、handler 异常、missed phase、当前/峰值深度、producer/consumer lag 和延迟。它包含固定对数桶延迟直方图与近似的 `p50_latency` / `p99_latency`；`CommEventCallback` 适合少量诊断事件。callback 自身抛出的异常会被隔离，不会改变通信操作的返回值或组件状态。安装/替换 callback 可能分配，调用 callback 会在内部同步之外执行任意应用代码；两者都是控制面操作，不属于实时保证。

组件 latency 是由组件定义的数据年龄、等待时长或发布到消费时长，不能当作端到端流水线延迟。业务消息应携带源时间戳，并在最终消费者计算完整时长；`comm_robot_pipeline` 演示了传感器到控制的测量。报告任何延迟结论时，都应同时给出组件、测量边界和样本数。

## 根据语义设置告警

| 组件 | 重点字段/事件 | 含义 |
| --- | --- | --- |
| `MpscChannel` | `dropped_count`、`current_depth`、`timeout_count` | 背压、积压或等待失败。 |
| `RealtimeChannel` | `dropped_count`、`handler_exception_count`、lag | 单周期预算不足、满队列或 handler 失败。 |
| `LatestMailbox` | `overwritten_count`、`stale_read_count` | 更新过快或消费者没有拿到更高版本。 |
| `DoubleBuffer` | sequence、`stale_read_count`、latency | 读者未见新快照或状态发布延迟。 |
| `PhaseGate` / `Sequencer` | `timeout_count`、`missed_phase_count` | 阶段未到达、已关闭或顺序被跳过。 |

阈值必须来自业务周期和数据重要性。例如 mailbox 的覆盖在“只要最新目标值”场景中可以正常，而在必须保存每条审计消息的场景中则说明选择了错误组件。

## 与 Kairo 失败状态的边界

`CommStats` 和 `CommEventCallback` 默认不汇总到 `ExecutorFailureStatus`，也不会调用 `Executor::set_failure_callback()`。若服务需要统一告警，在组件 callback 中把低频事件桥接到自己的监控系统。不要因为 `is_synchronization_lock_free()` 为 true 就在高频路径安装 callback：该查询只覆盖内部原子，不覆盖事件/字符串构造、callback 分配或用户代码。实时路径要求有界时，应由普通监控线程轮询计数。

## 下一步阅读

继续阅读[容量评估与告警处理](/zh/realtime-and-communication/capacity-and-alerting)，把累计统计转换成窗口速率、余量和处置动作；也可以回到[完整机器人数据流水线](/zh/tutorial/complete-robot-pipeline)按故障注入步骤验证背压与退出。
