# 通信与并发辅助 Facade 更新计划

本文档承接阶段 7：[通信与并发辅助 Facade 设计](../design/comm_facade.md)。

目标是提供 executor 级别的高层通信/并发抽象，让控制线程、采集线程、规划线程、通信线程之间的数据传递和时序控制默认安全、可观察，并且适合实时线程集成。

---

## 当前基线

- [x] `util::LockFreeQueue<T>` 已提供 MPSC 无锁队列、容量、批量 push/pop、失败 push 和峰值统计。
- [x] `RealtimeThreadExecutor` 已有实时 push、队列满/未运行/对象池耗尽等背压计数。
- [x] `Executor` facade 已有 failure event、recent failures、wait result、周期任务状态等诊断入口。
- [x] `TaskDependencyManager` 已有依赖图、ready 检查、完成标记和 cycle 检测。
- [x] `tests/harness/test_comm_facade_usage.cpp` 已有 disabled 用例，占位覆盖 channel、mailbox、phase gate、double buffer、realtime channel。
- [x] 已新增公开的 `executor::comm` 命名空间和聚合头。
- [x] Typed Channel 场景下用户无需自行组合锁、原子变量、条件变量或底层队列来表达跨线程通信。

---

## 任务映射

| id | category | area | severity | effort | title |
|----|----------|------|----------|--------|-------|
| P1 | feature | inter-thread communication | high | M | Typed Channel：类型安全 MPSC/SPSC 数据通道，解决共享变量 + 锁的易错用法 |
| P2 | feature | timing/order control | high | M/L | PhaseGate / Sequencer：显式控制线程间步骤顺序，解决“先后时序不确定” |
| P3 | feature | real-time integration | high | M | Realtime Mailbox：实时线程周期内消费最新数据 / 队列数据，带背压和丢弃策略 |
| P4 | feature | data race avoidance | medium | M | Snapshot / DoubleBuffer：读写线程无锁交换状态快照，替代共享 mutable state |
| P5 | feature | task graph | medium | L | submit_after / when_all：把已有 TaskDependencyManager 暴露为任务时序 API |
| P6 | diagnostics | observability | medium | M | 通信时序监控：drop、latency、stale、missed phase、producer/consumer lag |
| P7 | feature | in-process pub/sub | medium | L | Topic / Subscription：多模块独立消费同一后续事件流，逐订阅者有界背压与诊断 |

---

## 设计原则

1. **默认拒绝静默丢失**：满队列默认 `RejectNewest`，返回 `false` 并累计统计。
2. **实时线程不阻塞**：实时消费 API 使用 `try_*` / `drain_for_cycle()`，不做无限等待。
3. **类型安全优先**：用户传递 `T`、`Snapshot<T>`、`TaskHandle`，不传裸 `void*` 或共享 mutable 引用。
4. **生命周期显式**：所有通道支持 `close()`，关闭后发送和等待行为必须可预测。
5. **观察入口一致**：每个组件有 `stats()`；可选 `CommEventCallback`，后续再接入 `Executor` 聚合状态。
6. **先保守可用，再优化内部实现**：第一版可以用锁保护非实时/非平凡类型路径，但 API 不泄露锁语义。

---

## 阶段 7.0：API 骨架与文档入口

### 任务

- [x] 新增 `include/executor/comm.hpp` 聚合头。
- [x] 新增 `include/executor/comm/types.hpp`：
  - `CommErrorCode`
  - `CommResult`
  - `DropPolicy`
  - `CommStats`
  - `CommEventKind`
  - `CommEvent`
  - `CommEventCallback`
- [x] 新增空实现/最小声明文件，保证后续组件有稳定命名空间。
- [x] 在 `docs/API.md` 增加阶段 7 API 索引和最小使用提示。

### 验收

- [x] `#include <executor/comm.hpp>` 可编译。
- [x] `executor::comm` 通用类型有单元测试覆盖默认值、bool 转换和错误码字符串化。

---

## 阶段 7.1：P1 Typed Channel

### 任务

- [x] 实现 `MpscChannel<T>`：
  - `try_send(const T&)`
  - `try_send(T&&)`
  - `try_receive(T&)`
  - `send_for(...)`
  - `receive_for(...)`
  - `close()`
  - `stats()`
- [x] 实现 `SpscChannel<T>` 类型别名或轻量封装，后续可替换为 SPSC 优化实现。
- [x] 支持 `ChannelOptions`：
  - `capacity`
  - `drop_policy`
  - `enable_stats`
  - `name`
- [x] 满队列默认拒绝新消息；`DropOldest` 必须增加 `dropped_count`。
- [x] 关闭后发送返回失败，消费者可 drain 剩余数据。

### 测试

- [x] 启用/替换 `FacadeCommUsage.SensorProducerPlannerConsumer`。
- [x] 单生产者单消费者 FIFO 顺序测试。
- [x] 多生产者单消费者并发压力测试。
- [x] 队列满返回 false 且 `dropped_count` 或 `closed_send_count` 增加。
- [x] `close()` 唤醒阻塞 `receive_for()`。
- [x] 非平凡类型传递测试，如 `std::string` 或 move-only 类型视实现范围决定。

### 验收

- [x] 用户无需手写 mutex 即可完成 producer/consumer 数据传递。
- [x] 队列满、关闭后提交、超时等待均可通过返回值和统计观察。

---

## 阶段 7.2：P3 LatestMailbox / RealtimeChannel

### 任务

- [x] 实现 `LatestMailbox<T>`：
  - `publish(...)`
  - `try_load(T&)`
  - `try_load_newer_than(last_seen, out, new_sequence)`
  - `sequence()`
  - `stats()`
- [x] 实现 `RealtimeChannel<T>`：
  - `try_send(...)`
  - `drain_for_cycle(handler, max_items)`
  - `close()`
  - `stats()`
- [x] 支持 `RealtimeChannelOptions`：
  - `capacity`
  - `max_items_per_cycle`
  - `drop_policy`
  - `enable_stats`
  - `name`
- [x] 定义 handler 抛异常时的最小语义：停止本轮 drain、增加 `handler_exception_count`、记录 `HandlerException` 事件并继续外抛。
- [x] 与 `RealtimeThreadConfig::max_tasks_per_cycle` 的文档语义对齐：`0` 表示不限，非 0 表示单周期预算。

### 测试

- [x] 启用/替换 `FacadeCommUsage.ConfigThreadRealtimeControlThread`。
- [x] 启用/替换 `FacadeCommUsage.RealtimeCycleDrainsMessages`。
- [x] mailbox 多次 publish 后实时线程只消费最新值。
- [x] `try_load_newer_than()` 不重复消费旧 sequence。
- [x] `drain_for_cycle()` 不超过每周期预算。
- [x] 满队列策略计数正确。

### 验收

- [x] 实时周期内可用非阻塞 API 消费最新配置或有限条消息。
- [x] producer 过快时，drop/overwrite/lag 均可观察。

---

## 阶段 7.3：P2 PhaseGate / Sequencer

### 任务

- [x] 实现 `PhaseGate`：
  - `current_phase()`
  - `advance()`
  - `advance_to(phase)`
  - `has_reached(phase)`
  - `wait_for(phase, timeout)`
  - `close()`
  - `stats()`
- [x] 实现 `Sequencer`：
  - `next_ticket()`
  - `publish(ticket)`
  - `is_published(ticket)`
  - `wait_until_published(ticket, timeout)`
- [x] 定义 phase 倒退、重复 advance、close 后 wait 的返回语义。
- [x] missed phase 计数进入 `CommStats::missed_phase_count`。

### 测试

- [x] 启用/替换 `FacadeCommUsage.InitThreadWorkerThread`。
- [x] wait-before-advance 正常唤醒。
- [x] wait-after-advance 立即成功。
- [x] close 唤醒所有 waiter。
- [x] missed phase 场景返回 `MissedPhase` 并计数。
- [x] 并发 waiter 压力测试。

### 验收

- [x] 用户无需手写 condition variable 即可表达启动、采集、规划、通信等阶段顺序。
- [x] 时序错过或等待超时可观察。

---

## 阶段 7.4：P4 Snapshot / DoubleBuffer

### 任务

- [x] 实现 `Snapshot<T>`：
  - `value`
  - `sequence`
  - `timestamp`
- [x] 实现 `DoubleBuffer<T>`：
  - `publish(T)`
  - `update(fn)`
  - `load()`
  - `load_newer_than(last_seen, out)`
  - `sequence()`
  - `stats()`
- [x] 明确第一版写入模型：单写多读；多写场景建议通过 channel 汇聚。
- [x] 文档说明大型对象的复制成本和后续 `SnapshotPtr<T>` 扩展方向。

### 测试

- [x] 启用/替换 `FacadeCommUsage.StateWriterMonitorReader`。
- [x] 读者只能看到完整发布后的状态。
- [x] 多读者并发读取无 data race。
- [x] `load_newer_than()` 避免重复消费旧状态。
- [x] writer 高频更新时 reader 不读到半更新状态。

### 验收

- [x] 共享 mutable state 的典型读写场景可迁移到不可变快照。
- [x] 读者有 sequence 判断新旧数据。

---

## 阶段 7.5：P5 submit_after / when_all

### 任务

- [x] 设计并实现 `TaskHandle`。
- [x] 在 `Executor` facade 增加：
  - `submit_after(dependency, task)`
  - `submit_after(dependencies, task)`
  - `when_all(dependencies)`
- [x] 增加 `submit_with_handle()` / `submit_after_with_handle()`，兼顾 future 返回值和后续依赖链。
- [x] 内部复用或扩展 `TaskDependencyManager`。
- [x] 定义依赖失败传播策略：
  - 默认不执行 dependent task。
  - dependent future 返回可诊断异常。
  - 依赖图 cycle 或无效 handle 返回 ready future 异常或失败逻辑 handle，并记录 `SubmitRejected`。
- [x] 增加具有明确 `TaskHandle` 保留/过期语义的依赖状态裁剪，默认保留最近 1024 个终态 handle，支持容量 0 和运行时调整；活动依赖不会提前回收。

### 测试

- [x] `submit_after(A, B)` 保证 B 在 A 完成后执行。
- [x] `when_all(A, B)` 等待全部完成后触发 C。
- [x] 嵌套 `when_all()` 可传播完成状态。
- [x] A 失败时 B 不执行且 future 可观察。
- [x] cycle 或无效 handle 返回可诊断失败。
- [x] shutdown 期间 pending dependency 不静默丢失。

### 验收

- [x] 用户可通过 facade 表达任务时序，而不是手写 promise/future 链或轮询 `TaskDependencyManager`。
- [x] 长生命周期任务图状态有界；已完成 handle 在保留窗口内可继续建 dependent task，过期后以可诊断错误拒绝。

---

## 阶段 7.6：P6 通信时序监控

### 任务

- [x] 每个组件补齐 `stats()` 字段：
  - drop
  - latency
  - stale
  - missed phase
  - producer lag
  - consumer lag
  - peak depth
- [x] 增加 `set_event_callback()`，用于低频诊断事件。
- [x] 评估 `Executor` 级聚合：
  - `set_comm_event_callback(...)`
  - `get_comm_status()`
- [x] 第一版暂不接入 Executor 聚合；通信事件保持组件本地诊断，调用方可自行桥接。
- [x] 文档明确：通信事件默认不计入任务失败计数。
- [x] 增加 README / README_zh / docs/API.md 最小示例。

### 测试

- [x] drop/overwrite/stale/missed phase/timeout 各有明确计数测试。
- [x] callback 抛异常不会破坏通信组件状态。
- [x] 高频路径未注册 callback 时不产生额外日志。

### 验收

- [x] 用户能回答“丢了多少、延迟多大、是否读旧、是否错过 phase、生产/消费谁落后”。

---

## 阶段 7.7：示例与迁移材料

### 任务

- [x] 新增综合用户场景示例 `examples/comm_robot_pipeline.cpp`：
  - 采集线程到规划线程：`MpscChannel<T>`
  - 配置线程到实时控制线程：`LatestMailbox<T>`
  - 实时周期命令 drain：`RealtimeChannel<T>`
  - 初始化顺序控制：`PhaseGate`
  - 监控线程读取状态快照：`DoubleBuffer<T>`
  - 启动前 CPU 依赖任务：`TaskHandle` / `when_all()` / `submit_after()`
  - 本地通信诊断：`stats()` / `set_event_callback()`
- [x] 更新 README / README_zh / docs/API.md，指向综合场景示例。
- [x] 更新 `docs/MIGRATION.md`：从共享变量、手写锁、底层队列迁移到 comm facade。

### 验收

- [x] 示例优先展示 `executor::comm`，不要求用户先理解底层 `LockFreeQueue`。
- [x] 文档说明何时选择 Channel、Mailbox、DoubleBuffer、PhaseGate、TaskHandle。

---

## 阶段 7.9：P7 进程内 Topic / Subscription（已实现，等待 TSAN CI 验证）

### 进入条件与范围

现有 `MpscChannel<T>` 一条消息只能由一个 consumer 取走，而 `DoubleBuffer<T>` / `LatestMailbox<T>`
只保存最新值；它们无法使两个独立模块都按 FIFO 消费每一条后续事件。综合机器人流水线中若新增
第二个 planner、记录器或告警模块来处理同一帧，就进入本阶段。

范围仅限同一进程的 typed fan-out。它不实现网络传输、broker/topic discovery、持久化、重放、
确认、重连、QoS 协商或 reactive graph；这些需求由 ROS 2、NATS、MQTT 等系统负责。

### 任务

- [x] 新增 `include/executor/comm/topic.hpp`，并由 `include/executor/comm.hpp` 聚合导出：
  - `TopicSubscriptionOptions`：逐订阅者 `capacity`、`drop_policy`、`enable_stats`、`name`。
  - `TopicPublishResult`：匹配、成功、拒绝订阅者数量；至少一处拒绝时 bool 结果为 false。
  - move-only、RAII 的 `TopicSubscription<T>`：`try_receive()`、`receive_for()`、`close()`、`stats()`、`set_event_callback()`。
  - `Topic<T>`：`subscribe()`、`publish()`、`subscriber_count()`、`close()`。
- [x] 每个 subscription 使用独立有界 FIFO 与独立 `DropPolicy`；慢订阅者不得阻塞、回滚或改变其他订阅者的成功投递。
- [x] 明确消息从订阅成功之后才开始可见，不提供历史重放；Topic close 和 subscription close 必须唤醒等待者，同时允许 drain 已成功投递的消息；析构负责 RAII 注销/关闭，但不得与句柄自身仍在执行的成员调用并发。
- [x] 定义 publish/unsubscribe 的线性化点，并以稳定订阅快照和受管所有权防止 in-flight publish 访问已析构 subscription。
- [x] 第一版要求 `T` 可复制；文档说明大型不可变消息使用 `Topic<std::shared_ptr<const T>>`，不隐式共享 mutable object。
- [x] 复用 `BoundedQueue<T>` 的 drop、统计与 callback 语义；callback 必须在内部锁外触发并隔离异常。
- [x] 不将该实现标记为 lock-free 或硬实时；若以后需要控制周期中的确定性广播，另立固定订阅数、预分配的专用原语设计。
- [x] 扩展 `comm_robot_pipeline`：一份帧数据由规划和记录两个订阅者各自消费，并展示一个慢订阅者仅影响自身的背压统计。
- [x] 更新通信选择指南、API 文档和 maintainer communication capability card，明确 Topic 与 `MpscChannel`、`DoubleBuffer`、`LatestMailbox`、`RealtimeChannel` 的选择边界。

### 测试

- [x] 两个订阅者各自按 FIFO 收到同一批订阅后的消息；新订阅者不接收订阅前历史。
- [x] 每个订阅者可独立消费；一个慢订阅者满队列时，快订阅者仍收到完整消息流。
- [x] `RejectNewest`、`DropOldest`、`KeepLatest` 分别只影响对应 subscription，`TopicPublishResult` 与该 subscription 的 `CommStats` 一致。
- [x] publisher 与 subscribe/unsubscribe 并发压力测试，使用 TSAN 验证没有 use-after-free、data race 或死锁。
- [x] subscription close 和 Topic close 会唤醒 `receive_for()`；析构会注销活动订阅；已入队消息可先 drain，再得到 Closed。
- [x] move-only subscription 句柄的移动、重复 close、callback 异常隔离和无订阅者 publish 的语义测试。
- [x] `shared_ptr<const T>` 测试验证 fan-out 使用显式不可变共享所有权，不保留 publisher 局部对象引用。

本地普通并发压力探针已通过；TSAN 探针可成功编译，但当前容器在进入程序前因
`ThreadSanitizer: unexpected memory mapping` 退出。`test_comm_topic` 已加入正式 TSAN CI，待该 job
成功后勾选上项。

### 验收

- [x] 用户无需手工维护多个 `MpscChannel`，即可让多个独立模块接收同一后续事件流。
- [x] 单个慢订阅者的背压、drop、lag 和关闭均可单独诊断，且不会破坏其他订阅者的交付。
- [x] API 边界明确为进程内 best-effort fan-out，不被误解为网络 Pub/Sub、可靠广播或实时原语。

---

## 推荐实施顺序

1. 阶段 7.0：通用类型、聚合头、文档入口。
2. 阶段 7.1：Typed Channel，先解决最常见 producer/consumer。
3. 阶段 7.2：LatestMailbox / RealtimeChannel，补齐实时配置和周期 drain。
4. 阶段 7.3：PhaseGate / Sequencer，分别解决单调阶段推进与可跳 ticket 的 publication watermark。
5. 阶段 7.4：DoubleBuffer，替代共享 mutable state。
6. 阶段 7.6：通信时序监控贯穿补齐，也可随每个组件同步落地。
7. 阶段 7.5：submit_after / when_all，作为较大任务图扩展单独推进。
8. 阶段 7.7：示例、README/API/MIGRATION 同步。
9. 阶段 7.8：先定义 LET 与实时内存边界，再实现可验证的时间语义和延迟证据链。
10. 阶段 7.9：当出现至少两个独立 consumer 必须接收同一事件流的真实场景后，再实现进程内 Topic；保持网络 Pub/Sub 在库边界外。

这个顺序先覆盖最高频、最高风险的跨线程数据传递和实时消费，再扩展任务图 API。

---

## 测试矩阵

- [x] `test_comm_channel.cpp`
  - [x] FIFO
  - [x] MPSC 并发
  - [x] full/drop/close/timeout
- [x] `test_comm_mailbox.cpp`
  - [x] latest wins
  - [x] sequence freshness
  - [x] overwrite/stale stats
- [x] `test_comm_realtime_channel.cpp`
  - [x] cycle drain budget
  - [x] no blocking wait on realtime drain path
  - [x] backpressure counters
- [x] `test_comm_phase_gate.cpp`
  - [x] wait/advance
  - [x] close wakeup
  - [x] missed phase
- [x] `test_comm_double_buffer.cpp`
  - [x] complete snapshot
  - [x] concurrent readers
  - [x] no repeated stale consume
- [x] `test_executor_task_graph.cpp`
  - [x] submit_after
  - [x] when_all
  - [x] failure propagation
  - [x] cycle/invalid handle diagnostics
- [x] `test_comm_observability.cpp`
  - [x] drop/overwrite/stale/missed phase/timeout
  - [x] callback exception isolation
  - [x] latency/lag stats
- [x] `tests/harness/test_comm_facade_usage.cpp`
  - [x] replace disabled placeholders with compiling usage tests.
- [x] `test_comm_topic.cpp`
  - [x] per-subscription FIFO fan-out and no pre-subscription replay
  - [x] independent backpressure/drop policies and publish result
  - [x] concurrent publish/subscribe/unsubscribe lifetime safety（TSAN CI 已接入，运行结果待确认）
  - [x] close/waiter wakeup and callback exception isolation

---

## Open Questions

- [x] 第一版 `MpscChannel<T>` 支持非平凡类型和 move-only 类型，测试覆盖 `std::string` 与 `std::unique_ptr<int>`。
- [x] `DoubleBuffer<T>::load()` 保持按值快照语义；大型对象复制成本已在 API 文档说明，`SnapshotPtr<T>` 留作后续扩展。
- [x] `send_for()` / `receive_for()` 第一版通过 `close()` 唤醒，不引入 stop token。
- [x] `submit_after()` 使用 `std::future` 返回结果；需要继续依赖链时使用 `TaskSubmission<T>`、`submit_with_handle()`、`submit_after_with_handle()`。
- [x] 通信诊断保持独立 `CommEvent` / `CommStats`，默认不进入 `FailureKind`。
- [x] 第一版不提供 `Topic<T>` aggregate stats；逐 subscription 的队列深度、drop 与 latency 是诊断事实源，publish result 只报告本次 fan-out 数量。
- [x] 跨进程 delivery、消息重放或可靠确认保持外部消息系统适配边界，除非项目范围明确扩展为传输层。

---

## 阶段 7.8：逻辑时间与实时证据链（进行中）

未绑定的阶段 7 通信组件仍是独立原语：`PhaseGate` / `Sequencer` 不绑定数据版本，`DoubleBuffer` 不绑定逻辑相位。当前 `LatestMailbox` / `DoubleBuffer` 使用 reader-pin 固定槽，`RealtimeChannel` 使用预分配 MPSC 核心；这些同步核心 lock-free，但不把 payload、callback、时钟或 OS 调度提升为硬实时保证。相位绑定提供 LET 可见性，动态 `Topic` 则继续作为 mutex + 动态分配的非实时例外。

### P0：既有 `PhaseGate` × 快照/最新值原语的 LET 契约

不新增独立的 `LetChannel<T>`。LET 是既有 `PhaseGate` 与 `DoubleBuffer<T>` /
`LatestMailbox<T>` 的可选绑定模式：前者是唯一逻辑时钟，后者保存相位值；普通快照和
latest-wins API 必须保持向后兼容。

- [x] 设计并实现 `PhaseGate` 驱动的相位感知 `DoubleBuffer<T>` 模式，第一版明确为单写单读；多写仲裁留在非实时控制面。
- [x] 将相位 N 的提交、相位推进和 N+1 边界读取绑定为一个契约；迟到/竞争写入、跳相位和未就绪读取返回可诊断 `CommResult`。
- [x] 保持未绑定 `DoubleBuffer<T>::publish()` / `load()` 的最新完整快照语义不变；绑定模式只通过显式 API 启用。
- [x] 绑定模式使用固定双槽存储和原子相位发布；成功周期路径不得等待 mutex、condition variable 或堆分配。
- [x] 约束绑定模式的 `T` 满足无异常复制语义，并提供固定容量（仅允许两个槽位）验证。
- [x] 将同一 LET 绑定契约扩展到 `LatestMailbox<T>`；未绑定 `publish()` / `try_load()` 仍保持 latest-wins。
- [x] 明确 `RealtimeChannel<T>` 不自动绑定 LET，避免将 FIFO 周期预算误解为单值相位快照。

### P0：PhaseGate LET 核心边界测试（审查补强）

现有 `DoubleBuffer` / `LatestMailbox` 测试覆盖了外部可见性，但尚未直接锁定
`PhaseGate` 的 LET 内部机制。以下测试必须直接针对 gate 的 lease 和过渡状态，避免并发回归
只表现为偶发的 mailbox/buffer 失败：

- [x] 测试 writer lease 生命周期：获取 lease 后推进返回 `NotReady`，lease 析构后推进成功，活跃 writer 计数回落。
- [x] 测试 reader lease 生命周期：读取 lease 阻止相位复用，lease 释放后下一次推进成功，活跃 reader 计数不泄漏。
- [x] 测试 `advance()` / `advance_to()` 在绑定模式下都转发到 LET 过渡路径，并保持单调相位和重复推进诊断。
- [x] 测试 `let_transition_` CAS 竞争：同时推进时最多一个过渡成功，其余返回 `NotReady`，不得死锁或跳相位。
- [x] 增加多 writer / 多 reader 压力测试，验证推进只发生在所有 lease 释放后，且不出现半写快照、越相位读取或计数下溢。
- [x] 覆盖 gate close 与 lease/transition 竞争，确认发布、读取和推进都返回可区分的 `Closed` / `NotReady`。

### P0：实时内存契约

- [x] 文档明确实时回调禁止隐式堆分配、阻塞等待和诊断回调。
- [x] 为 Debug/Linux 增加可选 C++ `new` 分配检测 guard；检测结果包含组件和调用阶段。
- [x] 为 LET 关键路径提供固定容量双槽存储，并加入相位和分配 guard 回归测试。

### P1：分配 guard 强度与集成边界（审查补强）

`RealtimeAllocationGuard` 当前是显式、记录型的 Debug/Linux 工具，不应被误解为自动实时
安全证明。后续强化保持生产默认零开销，并避免无条件侵入宿主程序的 allocator：

- [ ] 增加可选违例策略：已实现 `RecordOnly` 与测试用 `Abort`；`CommEvent` 低频告警仍待设计为非实时路径桥接，默认不在实时路径分配字符串或调用用户 callback。
  （2026-09-22 定性：真实遗留设计项——RealtimeAllocationGuard 已交付 RecordOnly/Abort，
  剩余 CommEvent 桥接待设计）
- [x] 实现 guard 自动挂载到 `RealtimeThread` 周期入口的显式 API：`RealtimeThreadConfig::enable_allocation_guard` 默认关闭，开启后携带执行器名和 `cycle_callback` 阶段，保持旧回调默认行为不变。
- [x] 在文档和构建选项中明确全局 `operator new` 重载仅限 Linux 诊断构建，说明与宿主 allocator、内存池和共享库的冲突边界。
- [ ] 评估更低侵入的替代方案（链接器/`LD_PRELOAD` malloc hook 或平台专用 hook），在可移植性、部署复杂度和诊断完整性之间作出记录决策。
  （2026-09-22 定性：真实遗留评估项，无决策记录）
- [x] 集成回归测试：自动挂载路径定位执行器组件/阶段；关闭 guard 时不产生计数或事件，默认构建不启用全局分配跟踪。

### P1：LET 对外语义锚点

- [x] 在中英文 README、`docs/API.md`、设计文档和 website 双语手册统一使用 LET 语义说明：
  “相位 N 的完整输出，只有在 `PhaseGate` 推进到 N+1 后，才对读侧可见。”
- [x] 明确绑定模式是 `DoubleBuffer<T>` / `LatestMailbox<T>` 的可选 SWSR 双槽模式，不是独立 `LetChannel<T>`。
- [x] 明确未绑定 mailbox 仍为 latest-wins，绑定 mailbox 为每相位最多一次的单值快照；`RealtimeChannel<T>` 不继承 LET。

### P1：延迟直方图与端到端度量

- [x] 为通信组件增加固定开销的延迟直方图，提供近似 P50/P99。
- [x] 文档区分组件本地等待/发布到消费 latency 与业务消息端到端时间戳。
- [x] 更新 `comm_robot_pipeline` 示例，输出传感器→控制的端到端延迟与逻辑相位编号。
- [x] 将 jitter benchmark 与管线报告关联，记录 CPU、调度策略、编译器和采样边界；JSON 输出包含环境与测量定义。
