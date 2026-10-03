---
title: 最新值、状态快照与阶段同步
description: 为最新配置、完整状态与启动顺序分别选择 LatestMailbox、DoubleBuffer、PhaseGate 和 Sequencer。
---

# 最新值、状态快照与阶段同步

## 学习目标

按数据语义选择 `LatestMailbox<T>`、`DoubleBuffer<T>`、`PhaseGate` 或 `Sequencer`，而不是以共享对象和标志位拼凑同步。

## 最小流水线

教程示例把帧、配置、状态和启动阶段组合在一起：

<<< @/../examples/tutorial/08_communication.cpp{1-29}

完整源码：[`examples/tutorial/08_communication.cpp`](https://github.com/Linductor-alkaid/kairo/blob/master/examples/tutorial/08_communication.cpp)。

```bash
./build/examples/tutorial/tutorial_08_communication
```

## 预期输出

```text
frame=7, gain=3, state=21, phase=ready
```

## 只保留最新配置

`LatestMailbox<T>` 只保留最近一次 `publish()` 的值。实时消费者以 sequence 读取新版本，避免重复使用旧配置：

```cpp
uint64_t seen = 0;
ControlConfig config;
if (mailbox.try_load_newer_than(seen, config, seen)) {
    apply_config(config);
}
```

覆盖旧配置会增加 `overwritten_count`；没有更高 sequence 时是 stale read，而不是新消息丢失。

`publish(value)` 会复制左值，`publish(std::move(value))` 会移动新版本进入 mailbox；读取接口把当前值复制到调用方提供的输出对象。mailbox 不会保存对发布者局部变量的引用，但若 `T` 自身包含指针或 view，其底层数据生命周期仍由应用负责。适合共享大型只读配置时，可让 `T` 是 `shared_ptr<const Config>`，并在发布前完成完整校验。

mailbox 使用四个固定的 reader-pin 快照槽。writer 只取得未被 pin 的槽，因此 reader 复制非平凡 `T` 时不会与改写产生 data race。`try_load()` 最多检查四个槽。`try_publish(value, &sequence)` 是非等待、系统级 lock-free，但另一 publisher 推进时 publication CAS 可重试，因此不承诺单次调用有界或 wait-free。所有槽暂时繁忙或有限 sequence 域耗尽时它返回 `false`。兼容 `publish()` 用 `yield()` 重试暂时竞争；永久 sequence 耗尽时抛 `std::overflow_error`，不会无限 spin。

## 发布完整状态

`DoubleBuffer<T>` 的公开契约是单写多读。普通模式同样使用四个固定 reader-pin 槽：`publish()` 或 `update()` 完成候选值后再发布 sequence，reader pin 住稳定槽并按值复制 `Snapshot<T>`。它不会暴露半更新对象，也不获取 mutex。多写者先用 `MpscChannel` 汇聚到一个状态 owner；大型对象还要评估复制成本。

需要固定四槽读取尝试时使用 `try_load()`。`try_publish()` 是非等待 lock-free 发布接口，但不是单次有界/wait-free 接口。`publish()`、`load()` 和 `update()` 为保留兼容行为，在暂时竞争时会 spin/yield；它们是控制面调用，不是实时操作。快照 sequence 终止于 `2^56 - 1`：耗尽后 `try_publish()` 返回 `false`，publish/update 兼容路径抛 `std::overflow_error`。

`update()` 会在 writer 路径中把当前完整快照复制为局部候选值，同步修改后再发布；它不是提交给 Kairo 的异步任务。它捕获的引用只需覆盖这次同步调用，但仍要遵守 DoubleBuffer 的单 writer 约束。reader 得到的 snapshot 是自己的值副本，可以在下一次发布后继续使用。

## 阶段与发布水位

`PhaseGate` 适合初始化、标定、运行等单调推进的阶段：`advance_to()` 不能倒退或重复；`wait_for()` 区分成功、`Timeout` 和 `Closed`，`wait_for_exact()` 还能观察被跳过的 phase（`MissedPhase`）。

需要单调 publication watermark 时使用 `Sequencer`。`next_ticket()` 分配递增 ticket，但 `publish(ticket)` 可以直接跳到更大的 ticket。`is_published(ticket)` 表示水位达到或越过目标，并不证明该 ticket 曾被单独发布。`wait_until_published(ticket, timeout)` 是精确等待，水位越过目标后返回 `MissedPhase`。它不是数据队列，不能替代 `MpscChannel`。

两者都使用非阻塞原子状态核心；所需同步原子在平台上不是 lock-free 时会拒绝构造。带 timeout 的 wait 用 `steady_clock` 和 `yield()` 轮询核心，只适合启动/控制面协调，不应放进硬实时周期。

closed flag 占用状态最高位，因此 phase 和 ticket 必须小于 `2^63`。phase wait 对 `phase >= 2^63`、sequencer wait 对 ticket `0` 或 `ticket >= 2^63` 会在读取时钟或轮询前立即返回 `InvalidArgument`。关闭或 ticket 空间耗尽后 `next_ticket()` 返回 `0`。相位绑定 LET 把 `2^63 - 1` 保留为空槽哨兵，所以只有 `phase < 2^63 - 1` 才能发布。

## 相位绑定的 LET 值

当 reader 必须推理逻辑时刻时，可将 `DoubleBuffer<T>` 或 `LatestMailbox<T>` 显式绑定到
`PhaseGate`。这是既有组件的可选模式，不是独立的 `LetChannel<T>` API：

```cpp
kairo::comm::PhaseGate gate;
kairo::comm::DoubleBuffer<ControlState> state(ControlState{});
state.bind_to_phase_gate(gate);

state.publish_for_current_phase(ControlState{/* phase 0 output */});
gate.advance();

kairo::comm::Snapshot<ControlState> visible;
if (state.load_for_current_phase(visible)) {
    consume(visible.value); // phase 1 读取完整的 phase 0 输出。
}
```

绑定契约第一版是 SWSR，使用固定双槽。每相位最多一次发布；当前相位不能读取自身正在生成的值；
缺少上一相位的值或推进/读写竞争会通过 `CommResult::NotReady` 报告。`LatestMailbox<T>` 使用
同名相位 API，但在绑定模式中成为“每相位一个值”的快照；未绑定 `publish()` / `try_load()`
仍是 latest-wins。

`RealtimeChannel<T>` 不自动继承 LET：FIFO 的周期预算与相位绑定的单值是不同契约。绑定模式
成功的周期操作使用固定槽，不获取 mutex、等待 condition variable 或分配内部存储；失败诊断
不属于这条成功路径。

`is_synchronization_lock_free()` 的含义刻意窄于“硬实时”：它只覆盖组件内部同步原子，不覆盖
`T` 的复制/移动、时钟、字符串/结果、callback、`T` 自身分配、缺页或 OS 调度。固定次数读取、
非等待的系统级 lock-free 发布、内部存储无分配、data-race-free 和经测量满足硬实时预算是不同
声明。事件 callback 的配置与执行都应留在诊断/控制面。

## 下一步阅读

[通信可观察性](/zh/realtime-and-communication/observability)说明如何用本地统计与事件处理 drop、覆盖、陈旧读取和 missed phase。
