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
- **准入**：提交时点已过期的 deadline 被明确拒绝
  （`RoutingReason::Rejected`，detail "deadline already missed"）。
- **错过观测**：任务开始执行时已错过 → `FailureKind::DeadlineMissed`
  事件 + `ExecutorFailureStatus::deadline_missed_count`。契约保持
  "取消/超时是请求不是中断"：错过的任务仍会执行。
- 不改变 `ThreadPoolConfig::task_timeout_ms` 软超时语义。

### 3.2 QoS

- `QosClass { BestEffort, Standard, Interactive, HardRealtime }`。
- 未显式设置 priority 时映射默认排队优先级：
  BestEffort→LOW、Standard→NORMAL、Interactive→HIGH、
  HardRealtime→CRITICAL（`default_priority_for_qos()`）；
  `TaskBuilder::priority()` 显式设置后 QoS 不再覆盖。
- `CpuGpuTask::qos()` 同步映射到 `gpu_config.priority`。
- **饥饿契约（CR-024）**：PriorityScheduler 保持严格优先级、无自动
  aging——持续高优先级负载会饿死 BestEffort。这是有意设计（实时场景
  要求高优先级零干扰）；防饿死应在应用层拆分流量或使用独立执行器。
  HardRealtime 类别只影响排队序，池内任务不会被抢占；需要周期确定性
  时必须使用 RealtimeQueue 意图 + 专用实时线程。

### 3.3 affinity

- `AffinityHint { cpus, exclusive }` 是 per-task advisory 约束。
- DefaultScheduler 检查请求核集合与目标后端 `bound_cpus`（能力快照
  新维度）的相交性：不相交时任务仍被接受，但
  `RoutingDecision.detail` 附加 `AffinityMismatch` 警告。
- 线程绑核是后端启动期属性（ThreadPoolConfig::cpu_affinity 等）；
  per-task hint 不会重新绑定 OS 线程。`exclusive` 当前仅诊断可见。

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

## 5. 测试与验证

行为测试由 Independent-Verification-Agent 独立编写与执行，覆盖：
EDF 排序、deadline 拒绝/错过计数、QoS 映射、affinity 诊断、
resource 拒绝、自定义 IScheduler 注入生效、以及全量行为回归。
