# Scheduling Runtime（0.6.0）

## 1. 目标与边界

0.6.0 把"何时执行、在哪里执行、以何种准入执行"的调度决策从 `Executor`
facade 的内联代码中解耦为可注入的调度器组件，并建立统一的任务调度模型：
**deadline / QoS / affinity / resource**。

解耦边界（沿用 `unified_facade_and_auto_routing.md` 的原则）：

- 调度器只产出决策（准入 + 投递位置），不执行投递；
- 五种后端提交协议（future / DispatchResult / WorkerHandle /
  push_task / push_realtime_task）保持互不相同，由 Executor 适配层执行；
- 已验收的热路径不变式不回退：admission 计数先于 future、queued 诊断
  先于 enqueue、路由惰性能力采集（CR-106）、驻停代次唤醒（PA-2）。

## 2. 组件

### 2.1 IScheduler（include/kairo/scheduler.hpp）

```cpp
class IScheduler {
    virtual RoutingDecision route(const TaskRouter::Request&,
                                  const std::vector<ExecutorCapability>&) = 0;
    virtual void on_task_completed(const SchedulingFeedback&);
};

Executor::set_scheduler(std::unique_ptr<IScheduler>);  // nullptr 恢复默认
```

- `DefaultScheduler`：意图路由（TaskRouter）+ 调度模型约束检查（§3）。
- 注入时机：须在首次提交前；运行中替换需调用方自行同步。
- `route()` 在多提交线程并发调用，实现必须自身线程安全。
- 反馈通道现状（0.6.0）：failure event 回调 + routing observation；
  `on_task_completed` 为预留接口（DefaultScheduler 无操作）。

### 2.2 提交协议扩展

`IAsyncExecutor::try_submit_priority_task(priority, task, on_timeout, meta)`
新增重载，`TaskSchedulingMeta{deadline_ns, qos}` 随任务进入
`kairo::Task`（`deadline_ns`、`qos` 字段）。后端不感知 meta 时默认实现
退化为普通优先级提交。

## 3. 调度模型

### 3.1 deadline

- 声明：`TaskBuilder::deadline(time_point)` / `TaskOptions::deadline`。
- **EDF 排序**：同优先级内，有 deadline 的任务按 deadline 升序排在无
  deadline 任务之前；都无 deadline 时保持提交 FIFO
  （`Task::operator<`，PriorityScheduler 堆序随之生效）。
  这是 **priority class 内部的 EDF，不是全局 EDF**：层次为
  `priority → EDF → FIFO`，优先级仍然高于 deadline。NORMAL 级别但
  deadline 紧迫的任务仍可能被持续产生的 CRITICAL/HIGH 任务饿死——
  在既有 priority 契约存在的前提下，让 deadline 跨越优先级改变全局
  调度序反而会破坏原有语义。
- **准入**：提交时点已过期的 deadline 被明确拒绝
  （`RoutingReason::Rejected`，detail "deadline already missed"）。
- **错过观测**：任务开始执行时已错过 → `FailureKind::DeadlineMissed`
  事件 + `ExecutorFailureStatus::deadline_missed_count`。契约保持
  "取消/超时是请求不是中断"：错过的任务仍会执行。
- 不改变 `ThreadPoolConfig::task_timeout_ms` 软超时语义。

### 3.2 QoS

- `QosClass { BestEffort, Standard, Interactive, Critical }`。
- 未显式设置 priority 时映射默认排队优先级：
  BestEffort→LOW、Standard→NORMAL、Interactive→HIGH、
  Critical→CRITICAL（`default_priority_for_qos()`）；
  `TaskBuilder::priority()` 显式设置后 QoS 不再覆盖。
- `CpuGpuTask::qos()` 同步映射到 `gpu_config.priority`。
- **语义定位（0.6.0 评审确认）**：QoS 是**排队优先级 preset**，不是
  完整的 QoS policy——它不提供带宽、延迟界、抢占或实时性保证。
  命名有意避开 "HardRealtime"：真正的硬实时（周期确定性）必须使用
  RealtimeQueue 意图 + 专用实时线程执行模型，该名称预留给未来具备
  对应执行模型与系统约束的能力。
- **饥饿契约（CR-024）**：PriorityScheduler 保持严格优先级、无自动
  aging——持续高优先级负载会饿死 BestEffort。这是有意设计（实时场景
  要求高优先级零干扰）；防饿死应在应用层拆分流量或使用独立执行器。
  Critical 类别只影响排队序，池内任务不会被抢占。

### 3.3 affinity

- `AffinityHint { cpus }` 是 per-task advisory 约束。
- DefaultScheduler 检查请求核集合与目标后端 `bound_cpus`（能力快照
  新维度）的相交性：不相交时任务仍被接受，但
  `RoutingDecision.detail` 附加 `AffinityMismatch` 警告。
- 线程绑核是后端启动期属性（ThreadPoolConfig::cpu_affinity 等）；
  per-task hint 不会重新绑定 OS 线程。
- **设计决定（0.6.0 评审）**：不公开 `exclusive` 字段。声明一个暂时
  没有行为差异的强语义字段，长期比暂时缺少字段更容易产生兼容性
  负担；独占核请求留待真正实现 CPU reservation / exclusive ownership
  语义时随对应 enforcement 一起公开。

### 3.4 resource

- `ResourceRequirements { memory_bytes, gpu_device }` 声明式需求。
- GPU 相关提交（CpuOrGpu 意图 / CpuGpuTask）的检查：
  - 声明 `gpu_device` 与目标执行器实际设备不符 → 拒绝
    （`BackendUnavailable`）；
  - 声明内存超过设备可用量 → 拒绝（`CapacityPressure`；总量未知时
    跳过检查）。
- 能力快照 `ExecutorCapability` 新增 `gpu_device`、
  `gpu_memory_total_bytes`、`gpu_memory_free_bytes`、`bound_cpus`。

## 4. 数据流

```
submit_auto(TaskBuilder)
  → QoS→priority 映射（未显式设置时）
  → task_scheduler_->route(options, capabilities)   ← 可注入
      ├─ deadline 过期拒绝
      ├─ GPU device/memory 可行性
      ├─ TaskRouter 意图路由
      └─ AffinityMismatch 诊断
  → admission（总量有界，不变式不变）
  → try_submit_priority_task(priority, fn, on_timeout, meta)
      → ThreadPool: Task{deadline_ns, qos} → PriorityScheduler 堆序（EDF）
  → worker 执行前 deadline 检查 → DeadlineMissed 诊断（不中断）
```

## 5. 演进方向（0.6.0 评审记录，非本版承诺）

以下扩展点在 0.6.0 中只保留接口/文档空间，不提前实现：

- **deadline policy**：当前固定为 admit→EDF→miss-observe。未来可增加
  `RunAnyway / DropIfLate / CancelIfLate` 之类的 per-task deadline
  policy；错过观测（DeadlineMissed）已建立正确的语义基础。
- **resource 演进**：从 feasibility filter 走向
  `constraint filter → candidate generation → cost/ranking → selection
  → reservation` 模型，在 admission 与 dispatch 之间引入
  ResourceLease/ReservationToken 真正保留资源，闭合现有 TOCTOU 窗口。
- **DefaultScheduler 内部 pipeline 化**：当前调度输入少，顺序判断
  足够；一旦引入 NUMA topology、queue depth、utilization、VRAM
  pressure、locality、transfer cost 等，应把 route() 内部重构为
  `约束过滤 → candidate generation → scoring/ranking → selection`
  的 pipeline。`IScheduler` 对外接口保持不变，防止策略巨石。
- **feedback 闭环**：`on_task_completed(SchedulingFeedback)` 视为
  Scheduling Runtime 的正式组成部分（非纯诊断回调）。字段可逐步
  扩展 queue latency、execution time、selected backend、deadline
  missed、failure kind、实际资源用量，形成
  `decision → execution → measurement → feedback → next decision`
  闭环，支撑 adaptive/load-aware scheduler。
- **独占核 reservation**：`AffinityHint` 的独占语义随 CPU reservation
  机制一起引入（见 3.3 设计决定）。

## 6. 测试与验证

行为测试由 Independent-Verification-Agent 独立编写与执行，覆盖：
EDF 排序、deadline 拒绝/错过计数、QoS 映射、affinity 诊断、
resource 拒绝、自定义 IScheduler 注入生效、以及全量行为回归。
