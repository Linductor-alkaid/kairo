---
title: 选择消息传递方式
description: 为普通数据流选择 MpscChannel，为实时周期选择有预算的 RealtimeChannel。
---

# 选择消息传递方式

## 学习目标

把“每条帧都必须处理”和“实时周期只能处理有限命令”区分开，分别选择 `MpscChannel<T>` 与 `RealtimeChannel<T>`。

## 普通数据流：MpscChannel

采集线程到规划线程的帧流通常要求 FIFO 消费。`MpscChannel<T>` 是有界的多生产者/单消费者通道；普通消费者可用 `receive_for()` 设置等待边界：

```cpp
kairo::comm::ChannelOptions options;
options.capacity = 256;
options.drop_policy = kairo::comm::DropPolicy::RejectNewest;

kairo::comm::MpscChannel<SensorFrame> frames(options);
frames.try_send(SensorFrame{});

SensorFrame frame;
if (frames.receive_for(frame, std::chrono::milliseconds(10))) {
    plan(frame);
}
frames.close();
```

通道在构造期预分配有界 node pool。producer 先在私有节点中完整构造值，再原子发布；一个逻辑 consumer 分离完整批次并保持发布 FIFO。`try_send()` 与 `try_receive()` 是非等待入口，不获取 mutex，也不分配队列存储；竞争中的原子重试是 lock-free，而不是 wait-free。`send_for()` 与 `receive_for()` 在该核心上 spin/yield，并通过 `CommResult` 区分 `Timeout`、`Closed` 等结果，所以只适合普通控制线程。`close()` 停止新生产，已经入队的数据仍可继续 drain。

## 通道传递的是值，不是任务函数

通信组件没有 callable 与参数绑定模型。生产者直接提交一个 `T`：传左值会复制，传右值会把值移动进通道；发送成功后，消费者取得的是通道拥有的消息，而不是生产者栈上对象的引用。

```cpp
SensorFrame frame = capture_frame();
if (!frames.try_send(std::move(frame))) {
    handle_backpressure();
}
```

移动发送后不要继续依赖 `frame` 的原内容。若消息只保存裸指针、span 或 view，通道只拥有这个轻量对象，不拥有其指向的数据；底层 buffer 仍必须活到消费结束。大型数据推荐传递带明确归还规则的 buffer handle 或智能指针，并为池耗尽定义背压动作。

## 实时周期：RealtimeChannel

实时线程不应在周期内等待 condition variable，也不应无限清空积压。用 `drain_for_cycle()` 限制本周期处理量：

```cpp
kairo::comm::RealtimeChannelOptions options;
options.capacity = 128;
options.max_items_per_cycle = 8;

kairo::comm::RealtimeChannel<ControlCommand> commands(options);
commands.try_send(ControlCommand{});
commands.drain_for_cycle([](const ControlCommand& command) {
    apply_command(command);
});
```

`max_items == 0` 时使用配置中的 `max_items_per_cycle`；配置为 `0` 才表示不限。生产环境应保留明确上限，防止突发积压侵占整个控制周期。handler 抛异常时，本轮 drain 停止、统计记录 `handler_exception_count`、组件发出 `HandlerException`，异常仍会继续传播。

`RealtimeChannel` 使用相同的构造期预分配 MPSC 核心和单逻辑消费者契约。`try_send()`、`drain_for_cycle()`、close 与状态查询不获取 mutex，也不分配队列存储。若平台所需指针/整数原子不是 lock-free，构造会抛异常而不是静默退化；`is_synchronization_lock_free()` 只报告这层内部同步性质。

这仍不代表整个调用满足硬实时。`T` 的复制/移动/析构、`steady_clock`、handler、异常传播、诊断事件字符串/callback、缺页与 OS 调度都不在保证内。callback 只应在诊断/控制面配置；周期路径应使用有界、非抛出的 payload 操作和 handler，并在目标平台验证整条路径。lock-free 也不等于每次竞争操作都 wait-free。

`drain_for_cycle()` 的 handler 在调用期间接收 `const T&`；这个引用只在当前 handler 调用内有效。需要异步保留消息时应复制或转移到另一个所有权对象，不能保存该引用供周期结束后使用。handler 本身也在实时周期内执行，应保持有界并避免阻塞。

## 满队列时的业务选择

| 策略 | 含义 | 常见场景 |
| --- | --- | --- |
| `RejectNewest` | 拒绝新消息，生产者观察失败。 | 每条消息重要，调用方要主动背压。 |
| `DropOldest` | 丢掉最旧消息，接受新消息。 | 更关注较新数据，但仍以队列传递。 |
| `KeepLatest` | 保留最新消息。 | 接近状态语义；通常也可直接选 `LatestMailbox`。 |

不要忽略 `try_send()` 的返回值。队列容量、drop policy、峰值深度与 drop 数共同构成背压协议，而不是可随意调大的性能参数。

## 下一步阅读

只关心最新配置时请使用[最新值、状态快照与阶段同步](/zh/realtime-and-communication/state-and-phases)，不要用 FIFO 队列模拟覆盖语义。
