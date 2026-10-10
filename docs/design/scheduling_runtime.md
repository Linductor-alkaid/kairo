# Scheduling Runtime（0.6.0 / 0.6.1）

## 1. 目标与边界

0.6.0 把“何时执行、在哪里执行、以何种准入执行”的调度决策从 `Executor`
facade 的内联代码中解耦为可注入的调度器组件，并建立统一的任务调度模型：
**deadline / QoS / affinity / resource**。

0.6.1 不扩张调度策略维度，而是对 0.6.0 建立的边界做**稳定化、可观测性
补全与工程验证**：结构化路由决策（§6.1）、调度指标（§6.2）、执行期
反馈通道（§6.3）、capability snapshot 弱一致语义（§6.4）与调度路径
基准（tests/benchmark_scheduling_paths.cpp）。所有新增均为 additive
extension，不改变 0.6.0 已发布的提交协议与调度模型语义。

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
    virtual bool wants_feedback() const noexcept;  // 0.6.1；默认 false
};

Executor::set_scheduler(std::unique_ptr<IScheduler>);  // nullptr 恢复默认
```

- `DefaultScheduler`：意图路由（TaskRouter）+ 调度模型约束检查（§3）。
  `wants_feedback()` 返回 false（0.6.1 契约：默认调度器不消费反馈，
  也不依据反馈调整策略）。
- 注入时机：须在首次提交前；运行中替换需调用方自行同步。
  `set_scheduler()` 在注入时缓存 `wants_feedback()` 到原子开关。
- `route()` 在多提交线程并发调用，实现必须自身线程安全。
- `on_task_completed()` 在 worker 线程同步调用（§6.3）；实现必须
  快速返回且不抛异常（抛出被隔离，反馈语义即“已尝试交付”）。

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
- **准入（0.6.1 锁定）**：提交时点已**严格过期**（now > deadline）的
  deadline 被拒绝——`status = Rejected` + `reason = DeadlineExpired`；
  恰好等于 deadline 的提交被接受。
- **错过观测**：任务开始执行时已错过 → `FailureKind::DeadlineMissed`
  事件 + `ExecutorFailureStatus::deadline_missed_count` +
  `SchedulingMetrics::deadline_missed_count`。契约保持
  “取消/超时是请求不是中断”：错过的任务仍会执行。
- 不改变 `ThreadPoolConfig::task_timeout_ms` 软超时语义。

### 3.2 QoS

- `QosClass { BestEffort, Standard, Interactive, Critical }`。
- 未显式设置 priority 时映射默认排队优先级：
  BestEffort→LOW、Standard→NORMAL、Interactive→HIGH、
  Critical→CRITICAL（`default_priority_for_qos()`）；
  `TaskBuilder::priority()` 显式设置后 QoS 不再覆盖。
- `CpuGpuTask::qos()` 同步映射到 `gpu_config.priority`。
- **语义定位**：QoS 是**排队优先级 preset**，不是完整的 QoS
  policy——它不提供带宽、延迟界、抢占或实时性保证。真正的硬实时
  （周期确定性）必须使用 RealtimeQueue 意图 + 专用实时线程执行模型。
- **饥饿契约（CR-024）**：PriorityScheduler 保持严格优先级、无自动
  aging——持续高优先级负载会饿死 BestEffort。这是有意设计（实时场景
  要求高优先级零干扰）；防饿死应在应用层拆分流量或使用独立执行器。
  Critical 类别只影响排队序，池内任务不会被抢占。

### 3.3 affinity

- `AffinityHint { cpus }` 是 per-task advisory 约束。
- DefaultScheduler 检查请求核集合与目标后端 `bound_cpus`（能力快照
  新维度）的相交性：不相交时任务**仍被接受**，但决策降级为结构化
  诊断（0.6.1）：`status = AcceptedDegraded`、
  `reason = AffinityMismatch`（若决策尚未因回退降级）、
  `diagnostics |= RoutingDiagnostics::AffinityMismatch`，`detail`
  保留人读描述。已拒绝的决策不再附加 affinity 警告。
- 线程绑核是后端启动期属性（ThreadPoolConfig::cpu_affinity 等）；
  per-task hint 不会重新绑定 OS 线程，也不提供独占核保留。

### 3.4 resource

- `ResourceRequirements { memory_bytes, gpu_device }` 声明式需求。
- GPU 相关提交（CpuOrGpu 意图 / CpuGpuTask）的检查：
  - 声明 `gpu_device` 与目标执行器实际设备不符 → 拒绝
    （`BackendUnavailable` + `ResourceInfeasible` 诊断位）；
  - 声明内存超过设备可用量 → 拒绝（`CapacityPressure` +
    `ResourceInfeasible` 诊断位）。
- 能力快照 `ExecutorCapability` 新增 `gpu_device`、
  `gpu_memory_total_bytes`、`gpu_memory_free_bytes`、`bound_cpus`。
- 弱一致语义见 §6.4。

## 4. 数据流

```
submit_auto(TaskBuilder)
  → QoS→priority 映射（未显式设置时）
  → task_scheduler_->route(options, capabilities)   ← 可注入
      ├─ deadline 过期拒绝（DeadlineExpired）
      ├─ GPU device/memory 可行性（ResourceInfeasible 诊断位）
      ├─ TaskRouter 意图路由（status: Accepted/Degraded/Rejected）
      └─ AffinityMismatch 结构化降级诊断
  → record_routing_decision（无条件累加 SchedulingMetrics）
  → admission（总量有界，不变式不变）
  → try_submit_priority_task(priority, fn, on_timeout, meta)
      → ThreadPool: Task{deadline_ns, qos} → PriorityScheduler 堆序（EDF）
      → [wants_feedback()] 测量包装：queue wait / duration / 终态
  → worker 执行前 deadline 检查 → DeadlineMissed 诊断（不中断）
```

## 5. 演进方向（非本版承诺）

以下扩展点只保留接口/文档空间，不提前实现：

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
- **feedback 闭环（v0.7.0 Runtime-aware Scheduling）**：0.6.1 已建立
  measurement contract（§6.3）；利用 queue depth、backend load、
  historical latency、resource pressure 的动态评分属于后续版本。
- **独占核 reservation**：CPU reservation / exclusive ownership 语义
  随 resource reservation 机制一起引入（见 3.4）。

## 6. 0.6.1：结构化诊断、指标与反馈

### 6.1 结构化路由决策

`RoutingDecision` 新增三个字段（additive，0.6.0 字段全部保留）：

| 字段 | 语义 |
|------|------|
| `RoutingStatus status` | 结果的**权威判据**：`Accepted` / `AcceptedDegraded` / `Rejected`。消费方据此判断任务是否被接受，不再枚举 reason 组合。 |
| `RoutingReason reason` | 导致该结果的主要原因。新增 `DeadlineExpired`（过期 deadline 拒绝）与 `AffinityMismatch`（降级诊断）。 |
| `uint32_t diagnostics` | `RoutingDiagnostics` 位掩码：`AffinityMismatch`、`ResourceInfeasible`。保留并发存在的次要诊断（reason 只能表达单一主因）。 |

约定：

- 拒绝类 reason（`Rejected` / `BackendUnavailable` / `BackendNotRunning`
  / `CapacityPressure` / `DeadlineExpired`）只出现在
  `status == Rejected` 的决策上；接受类 reason（`DefaultPolicy` /
  `ExplicitIntent` / `PreferredExecutor` / `GpuHeuristic` /
  `AdaptiveHistory`）只出现在 Accepted；降级原因（`AffinityMismatch` /
  `FallbackPolicy`）表达 AcceptedDegraded 的首要降级因素。
- 0.6.0 风格的自定义调度器（只设拒绝类 reason、未设 status）由
  `Executor::route_task()` 归一化为 `status = Rejected`，行为不漂移。
- facade 内所有“记录的决策必须反映最终投递结果”：CpuGpuTask 的
  heuristic-CPU + 非 AllowCpu fallback 组合在记录前修正为
  `status = Rejected`（0.6.0 会记录 Accepted 决策后拒绝，计数与
  结果不一致）。
- `routing_status_to_string()` / `routing_reason_to_string()` 提供稳定
  名称。

### 6.2 调度指标（SchedulingMetrics）

`Executor::get_scheduling_metrics()` 返回无锁单调计数快照
（`include/kairo/types.hpp`）。计数在 `record_routing_decision()` 内
无条件累加（先于 CR-106 观测开关判断），提交路径只付若干次 relaxed
原子加；计数器结构体独占缓存行（alignas(64)）避免伪共享。

计数对象是**路由决策**而非任务（一次提交可能产生多条决策，如 GPU
提交异常后的回退会追加一条 AcceptedDegraded 决策）。字段与
status/reason/diagnostics 一一对应，另有 `deadline_missed_count`
（执行期错过，与 failure 体系同源）与 `feedback_reported_count`。

### 6.3 执行期反馈（SchedulingFeedback）

`SchedulingFeedback`（0.6.0：task_id/qos/success）扩展执行期测量：
`backend` / `executor_name` / `queue_wait_ns` /
`execution_duration_ns` / `had_deadline` / `deadline_missed` /
`failure_kind`（success 时为 `FailureKind::None`）。

- **开关**：`IScheduler::wants_feedback()`（默认 false）。false 时
  submit_auto 不附加测量包装——反馈通道零开销；true 时每个实际执行的
  任务付 2 次 steady 时钟采样 + 一次 `on_task_completed()` 同步调用。
  Executor 在 `set_scheduler()` 时缓存该值。
- **覆盖范围**：实际开始执行的默认异步池任务。提交前被拒 / admission
  拒绝 / 排队期软超时（`on_timeout` 先于执行）的提交不产生反馈——
  这些终态由 RoutingDecision、SchedulingMetrics 与 failure 体系观测。
  GPU / lockfree / realtime 路径的反馈接入属后续版本。
- **线程模型**：worker 线程同步调用，实现不得抛异常（隔离处理）、
  不得阻塞（会占用 worker）。
- **0.6.1 边界**：只建立 measurement contract。DefaultScheduler 不
  消费反馈；任何基于 queue pressure、historical latency 或 backend
  utilization 的自适应调度行为都属于 v0.7.0+。
- 0.7.0 起，流入的样本在 `FeedbackAggregator` 中聚合（§7.1）；本节
  的 measurement contract 本身不变。

### 6.4 capability snapshot 弱一致语义

`ExecutorCapability` 是**建议性快照**，不引入锁或全局资源管理器：

- **未知即宽容**：`bound_cpus` 为空 / `gpu_device < 0` /
  `gpu_memory_total_bytes == 0` 表示快照未知，对应检查跳过（permissive
  fallback），行为稳定可预测；
- **已知但不满足 → 明确拒绝**：结构化 reason + `ResourceInfeasible`
  诊断位；
- **admission 之后资源状态变化**（TOCTOU 窗口）：由后端执行失败
  （`FailureKind::GpuFailure` 等）负责报告——admission feasibility
  不等于 execution guarantee；
- `ResourceRequirements` 因此是 feasibility hint，**不是资源预留**；
  真正的 reservation layer 属后续版本（§5）。

### 6.5 调度路径基准

`tests/benchmark_scheduling_paths.cpp` 建立以下基线（目的在于发现
回归与明显的分配/锁/字符串构造问题，不在本版做激进优化）：

1. 普通优先级提交 vs 携带 SchedulingSpec（deadline/QoS/affinity）提交；
2. EDF enqueue/dequeue（PriorityScheduler 堆操作）；
3. DefaultScheduler::route 的纯策略路径 vs 能力快照路径；
4. 能力快照采集（get_executor_capabilities）；
5. 开启 feedback 时的测量包装开销。

成功调度的普通路径不为诊断构造字符串：结构化 reason/status 的
引入正是为了把 detail 字符串从程序判断接口的位置上移除。

## 7. 0.7.0 反馈聚合层 FeedbackAggregator（M2，已落地）

`include/kairo/feedback_aggregator.hpp`。0.6.1 建立了 measurement
contract（§6.3）；M2 为其补上聚合与消费的基础设施。聚合本身仍不改变
任何调度行为——DefaultScheduler 契约与 0.6.1 逐项一致，样本流入仅在
注入 `wants_feedback() == true` 的调度器时发生。

- **写路径（worker 热路径）**：`FeedbackAggregator::record()` 由
  `report_scheduling_feedback()` 喂入，与 0.6.1 测量包装同源。满足
  §2.1 的三条硬约束——不阻塞（无锁：样本写入调用线程的分片）、不分配
  （键槽定长、SSO 字符串比较）、不抛异常（`noexcept`；异常值夹取）。
  分片固定 32 片，进程级 `thread_local` 槽位稳定映射（worker 数超过
  分片数时共享分片，仍无锁）。
- **键与键空间**：按 `(backend, executor_name, qos)` 分键；
  executor_name 槽内定长 48 字节（超长截断，截断后相同的键合并）。
  键空间有界：每分片默认 16 槽（上限 64）；未见过的键先到先得建槽，
  表满后新键样本丢弃并计入 `dropped_samples`——§6.4 "未知即宽容"的
  聚合版。`register_key()` 支持预注册（幂等、全或无）。
- **聚合量**：计数自构造起累计（attempts / failures / deadline_misses /
  分桶直方图，桶界可配且须严格升序）；EWMA 是唯一带遗忘的量（整数
  定点，千分比平滑系数，默认 125 = 1/8；样本夹取到 [0, 1e15] ns 保证
  定点乘法不溢出）。
- **读路径（RCU）**：`route()` 侧与诊断只读周期性合并出的不可变快照
  （`FeedbackSnapshot`：EWMA、直方图、失败率、dropped 计数；entries
  按 key 排序输出确定性），经原子 `shared_ptr` 指针交换发布，读路径
  无锁。entries 只包含至少有一条样本的键（预注册未观测的键不出现，
  消费方可假设 `attempts > 0`）。合并由 `refresh()` / `refresh_if_stale()`
  驱动（本类不拥有线程：诊断读方与后续 M3 的 AdaptiveScheduler 决定
  刷新节奏）。快照为弱一致：分片间不同步；EWMA 跨分片按样本数加权
  平均。
- **facade 接入**：`Executor::get_feedback_snapshot()`（诊断用，
  超过 merge_interval 先重合并再读快照）；`get_snapshot_text()` 末尾
  追加 `scheduling_feedback.*` 行式段（无样本时仅输出汇总计数）。
- **验收**：开启聚合时每任务聚合开销 ≤ 100ns——
  `benchmark_feedback_aggregator` 内置验收线（record 快路径中位数）；
  facade 端到端（提交 → worker 反馈 → 聚合转发）差值同预算内；TSAN
  全绿；默认调度器路径零变化。

## 8. 测试与验证

行为测试由 Independent-Verification-Agent 独立编写与执行，覆盖：
EDF 排序、deadline 拒绝/错过计数、QoS 映射、affinity 诊断、
resource 拒绝、自定义 IScheduler 注入、以及全量行为回归；0.6.1 追加
契约加固（set_scheduler 时序、所有权与销毁、shutdown 竞争、旧式
拒绝归一化、高并发 route()、deadline 边界与同时 deadline、
capability 缺失退化）、结构化决策/指标/反馈断言与调度基准。
