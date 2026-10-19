# Scheduling Runtime（0.6.0 / 0.6.1 / 0.7.x）

## 1. 目标与边界

0.6.0 把”何时执行、在哪里执行、以何种准入执行”的调度决策从 `Executor`
facade 的内联代码中解耦为可注入的调度器组件，并建立统一的任务调度模型：
**deadline / QoS / affinity / resource**。

0.6.1 不扩张调度策略维度，而是对 0.6.0 建立的边界做**稳定化、可观测性
补全与工程验证**：结构化路由决策（§6.1）、调度指标（§6.2）、执行期
反馈通道（§6.3）、capability snapshot 弱一致语义（§6.4）与调度路径
基准（tests/benchmark_scheduling_paths.cpp）。所有新增均为 additive
extension，不改变 0.6.0 已发布的提交协议与调度模型语义。

0.7.x 系列将反馈闭环引入调度决策（0.7.0 AdaptiveScheduler），补全资源
预留（0.7.1 ResourceLease）与 deadline policy（0.7.1），并在 0.7.2 完成
**优先级语义从提交 API 转移到调度器**的架构清理。

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
- 0.7.0 起，流入的样本在 `FeedbackAggregator` 中聚合（§7.1），并由
  AdaptiveScheduler 消费（§8）；本节的 measurement contract 本身不变。

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

## 8. 0.7.0 AdaptiveScheduler（M3，已落地）

`include/kairo/adaptive_scheduler.hpp`。M2 建立了聚合与消费基础设施；
M3 是 §0 原则 1 的兑现：自适应以新的 `AdaptiveScheduler` 经
`set_scheduler()` 注入，DefaultScheduler 保持确定性、行为与 0.6.1 逐项
一致。结构上复用 M1 pipeline（`SchedulingPipeline<IdentityScoring>` 产出
0.6.1 基线决策）与 M2 聚合器（内嵌 `FeedbackAggregator` 作决策输入，
与 facade 诊断聚合器相互独立），再对基线候选做三类后处理。

- **CPU/GPU 历史选择**（CpuOrGpu）：端到端时延 EWMA（queue wait +
  execution duration）按 `(backend, executor_name, qos)` 两侧对比；
  双侧 `attempts >= min_samples`（默认 8）才采信历史，否则回退 0.6.1
  启发式结果（不标记）；切换需挑战侧优于在用侧 `(1 - hysteresis_margin)`
  （默认 0.25，防震荡）。命中历史（含边际内"维持"）→ reason =
  `AdaptiveHistory` + diagnostics 位 + detail 携带两侧 EWMA 纳秒值，
  决策完全可复原。学习粒度为 per-(backend, executor, qos)。显式用户
  约束优先于历史：`RequireRequestedBackend` 的请求不做历史翻转。配套
  测量通道补齐：`submit_auto(CpuGpuTask)` 在 wants_feedback() 时为
  CPU/GPU 两侧附加测量包装（此前该路径不产生 SchedulingFeedback，
  GPU 侧经泛型转发包装上报 `(Gpu, executor_name, qos)`）。
- **QoS 感知降载**：按 QoS 类把 queue-wait 直方图**按窗口差分**（两次
  评估间的样本增量——聚合器直方图自构造起累计，直接评估会让"证据
  不足"在预热后不可达、陈旧 p99 无遗忘），求窗口 p99 与
  `min_window_samples` 证据门槛；连续 `shed_breach_windows` 个评估窗口
  超过该类目标 → 开闸，对**严格低于**超阈类的提交合成结构化拒绝
  （`RoutingReason::LoadShedding`，detail 标明超阈类与目标值）；连续
  `shed_close_windows` 个恢复窗口（p99 < 目标 × recovery_ratio）才
  关闸——开/关对称滞回，防止阈值附近反复开关。证据不足或目标为 0 →
  "未知即宽容"（fail-open：清零连击并解除激活态）。与
  `max_in_flight_tasks` 硬上界共存：只做更早、更有区分度的拒绝。
- **QoS→priority 有界提升**：同一超阈信号、更长的持续窗口
  （`promotion_windows` ≥ shed_breach_windows，取消需
  `promotion_close_windows`）→ 该 QoS 类默认排队优先级 +1 级（封顶
  CRITICAL）。经 `IScheduler::effective_priority_for()` 生效（0.7.0
  新增虚方法，默认实现原样返回 default_priority）；**显式 priority
  永不被覆盖**；被调整的提交计数到
  `SchedulingMetrics::priority_promoted_count`。防饿死兜底仍由 M0 的
  可选 aging（roadmap §2.1）承担。
- **决策可复原**：reason（`AdaptiveHistory` / `LoadShedding`）+
  diagnostics 位 + detail 数值 + `SchedulingMetrics` 三个新计数
  （`adaptive_history_count` / `load_shedding_rejected_count` /
  `priority_promoted_count`）+ AdaptiveScheduler 诊断接口
  （`load_shed_active()` / `priority_promoted()` / `feedback_snapshot()`
  / `format_state_text()`）。
- **明确不做**（roadmap §2.4 非目标）：NUMA、跨 pool 迁移、基于
  利用率的线程数调整（已有 resizer，避免双控制回路打架）。
- **评估节奏与热路径形状**：route() 侧每 `merge_interval`（内嵌聚合器
  配置，默认 100ms）至多重评估一次派生状态（原子 fast 门限 + 互斥锁内
  重评估 + 双原子掩码发布，提交热路径只付两次无锁原子读）；实测
  route() 纯策略路径配对差 ~19-50ns、on_task_completed 转发 ~23ns
  （验收线均 150ns）。
- **验收**（已达成）：`benchmark_adaptive_scheduler` 内置验收线全过
  （route Auto ≤150ns、CpuOrGpu 冷路径 ≤250ns、on_task_completed
  ≤150ns、effective_priority_for ≤50ns）；降载场景 Interactive 探针
  端到端 p99 自适应 1.3-4.2ms vs DefaultScheduler 10.8-16.9ms
  （绑核配对，160-168 次结构化拒绝）；TSAN 全绿；默认路径配对
  A/B vs M2 head 在 ±5% 内。基准记录：`docs/performance/m3_adaptive_scheduler_results.md`。

## 9. 测试与验证

行为测试由 Independent-Verification-Agent 独立编写与执行，覆盖：
EDF 排序、deadline 拒绝/错过计数、QoS 映射、affinity 诊断、
resource 拒绝、自定义 IScheduler 注入、以及全量行为回归；0.6.1 追加
契约加固（set_scheduler 时序、所有权与销毁、shutdown 竞争、旧式
拒绝归一化、高并发 route()、deadline 边界与同时 deadline、
capability 缺失退化）、结构化决策/指标/反馈断言与调度基准。

## 10. 0.7.1 能力快照缓存（roadmap §3.0.1，开发快照）

### 10.1 能力快照 TTL 缓存

CR-106 之后，`submit_auto` 的 CpuOrGpu / affinity 路由与 `dispatch_auto`
的能力消费仍要每次锁 Manager 注册表采集能力快照（5 把注册表锁 +
vector 构建）。0.7.1 把该采集缓存化：

- **载体**：Executor 内不可变 `CapabilitySnapshot`（`manager_epoch` +
  `collected_ns` + `std::vector<ExecutorCapability>`），经 M2 同款可移植
  RCU 槽（`scheduling::detail::SnapshotSlotT`，本版自
  `SnapshotSlot` 泛化，既有别名不变）发布。命中路径 = 一次 acquire
  load + epoch/TTL 两个整数比较 + 一次时钟采样。
- **命中判据**：`cached->manager_epoch == manager.get_state_epoch()`
  且 `now - collected_ns < ttl`。TTL 默认 100ms（roadmap §3.0.1 的
  保守取值），`set_capability_cache_ttl()` 可调，0 表示完全关闭
  （每次实时采集，回到 CR-106 行为）；`invalidate_capability_cache()`
  立即丢弃。TTL 以采集**前**的时钟样本计，有效窗口略短于名义值。
- **主动失效**：manager `state_epoch` 在注册集合与可观测运行状态变化
  时推进（register/start/stop/shutdown）。0.7.1 补齐两处此前绕过
  注册表的转换：facade `start_realtime_task` / `start_blocking_io_worker`
  在执行器对象上启停，成功后显式调用新增的
  `ExecutorManager::notify_backend_state_changed()` 推进 epoch。
  因此 TTL 只兜底 `pending_work`、GPU 可用内存等**运行期数值**漂移；
  生命周期状态（registered/running）不跨转换陈旧。
- **弱一致语义不变**：缓存只影响快照新鲜度，不改变 0.6.1 契约——
  快照是 advisory（未知即宽容、feasibility hint 非 reservation），
  缓存开/关的路由决策逐项一致（`test_capability_cache.cpp` 锁定）。
  并发未命中各自采集、最后发布者胜出（每 TTL 至多一轮，不为它引入
  热路径互斥）；被覆盖的旧快照由 shared_ptr 引用计数在读者手中兜底。
- **可观测性**：`SchedulingMetrics` 新增 `capability_cache_hit_count` /
  `capability_cache_miss_count`（访问点统计，与决策记录解耦、不受
  CR-106 观测开关影响）；命中产出的决策带
  `RoutingDiagnostics::CapabilityCacheHit`（`1u<<7`）位与
  `RoutingDecision::capability_from_cache == true`（位与字段同置，
  掩码过滤与逐条判读两便）。公开的 `get_executor_capabilities()`
  保持实时采集语义，不经缓存（预检/显示用途依赖最新值）。

### 10.2 submit_fast() 的评审移除

路线图 §3.0.1 曾规划零路由快速提交 API（提交至优先级通道
`submit_priority(NORMAL, ...)`，不产出 `RoutingDecision`）。实现后
按评审要求与 `submit` 做配对实测：**普通提交通道反而快 31.4%**
（1881 vs 2473 ns/op，Windows MSVC Release，20000 任务配对同进程）
——优先级通道的 promise/atomic_bool/bind 间接层比 submit 的
CR-107 PromiseCell 合并控制块更重。且两者的用户可见语义本就重复
（submit 系列从不产出路由决策、也不表达 QoS/deadline）。按
2026-10-14 评审决定：**移除 `submit_fast`，保留 `submit`**。
推论：零路由语义无需新 API 承载；§3.0.1 的实际性能收益由 §10.1
的能力快照缓存承载。`benchmark_scheduling_paths` §6 改为 submit vs
submit_auto 裸 builder 守卫（submit ≤ 1.05 × bare，实测快约 30%）。
### 10.3 验收（配对 A/B，验收线内置 `benchmark_scheduling_paths` §5/§6）

- 缓存命中路径移除端到端 36.6% 的采集成本（fresh 1415 ns/op →
  warm 898 ns/op，Windows MSVC Release；验收线 warm ≤ 0.9 × fresh），
  对应 roadmap §7.1 的 route 级 ~120ns → ~60ns 目标方向；
- submit（普通通道）2042 ns/op vs `submit_auto` 裸 builder 2906 ns/op
  （-30%；验收线 submit ≤ 1.05 × bare）；submit_fast 评审实测
  2473 ns/op，慢于 submit 31.4%，据此移除（见 §10.2）；
- 契约测试 `test_capability_cache.cpp`（14 用例）：命中/未命中计数
  与来源标记、TTL 过期/关闭/重设、注册-启动-停止的 TTL 内主动失效、
  决策一致性矩阵、纯提交路径（submit）无决策语义与 admission、
  并发 + epoch 扰动下的 hit+miss 守恒。

## 11. 0.7.1 deadline policy（roadmap §3.2，开发快照）

在既有的 admit → EDF → miss-observe 链之上，增加 per-task 执行期
处置策略（`DeadlinePolicy`，`TaskOptions::deadline_policy`，默认
`RunAnyway`）。裁决点与 0.6.1 的 miss-observe 完全一致——**开始执行
时**（facade 包装层，即 worker 取到任务的时刻），边界亦同：**严格
已过才算错过，恰好相等接受**。提交时点的准入拒绝（严格已过 →
`DeadlineExpired`）不受策略影响。

- **`RunAnyway`（默认）**：0.6.x 行为逐位一致——错过仍执行，记录
  `FailureKind::DeadlineMissed` failure 事件 + `deadline_missed_count`。
- **`DropIfLate`**：错过则不执行用户代码。future 以
  `TaskCancelled(TaskCancellationReason::DeadlineMissed)` 终态就绪；
  计 `deadline_missed_count` + 新增 `deadline_dropped_count`。遵循
  取消类约定：**不进 failure 统计**（任务根本没运行，终态由 future
  与调度指标承载）。结算分类器（`is_deadline_drop_exception`）保证
  丢弃不落入 task-exception 统计。
- **`CancelIfLate`**：错过则向任务的协作停止源发停止请求（**不是
  抢占**），任务仍启动；错过本身仍记 `deadline_missed_count`。用户
  代码经新增的 `kairo::this_task::get_stop_token()` 轮询请求并自行
  退出——该设施以 inline thread_local + RAII 守卫（嵌套安全）在
  CancelIfLate 任务执行期间安装令牌；非任务上下文或未声明
  CancelIfLate 时返回空令牌（`stop_possible() == false`）。

### 11.1 与依赖图的交互

tracked 提交新增 `submit_after / submit_after_with_handle` 的
TaskBuilder 重载（携带完整 TaskOptions：deadline/策略/QoS/优先级，
QoS→默认优先级映射与 `effective_priority_for` 咨询语义与
`submit_auto` 一致）。上游 DropIfLate 丢弃时，任务图节点落 Failed、
异常为 `TaskCancelled(DeadlineMissed)`，下游经既有的
`reclassify_dependency_exception` 规则统一改写为
`TaskCancelled(DependencyCancelled)`——零新增传播机制。上游
CancelIfLate 协作完成（不抛异常）→ 下游照常执行；协作退出若抛
`TaskCancelled` 而取消请求并非来自 registry，则按既有规则计任务
异常（deadline 协作请求独立于 registry 取消簿记）。

### 11.2 适用面与边界

- 策略消费面：`submit_auto(TaskBuilder)` 与 tracked TaskBuilder
  重载。`CpuGpuTask` 本阶段未接入（其 options 的 policy 字段被
  忽略；CPU/GPU 路径维持 0.7.0 的 feedback 级 miss 观测），
  `submit_priority_scheduled` 的 meta 路径无 policy 表达（不变）。
- 无 deadline 时策略字段无效（零观测、零处置）；策略只改变
  **执行期处置**，不改变 EDF 排序与准入。
- 取消是请求不是中断：`CancelIfLate` 不提供任何抢占或强制终止；
  用户代码不观测令牌时行为等同 RunAnyway（多一次 missed 计数）。

### 11.3 验收（契约矩阵，`tests/test_deadline_policy.cpp`，14 用例）

- 边界不漂移：三种策略的提交时点准入拒绝一致（DeadlineExpired、
  missed/dropped 均 0）；未来 deadline 与无 deadline 的 policy 零作用。
- 三策略 × 强制错过：RunAnyway 执行 + failure 事件；
  DropIfLate 不执行 + `TaskCancelled(DeadlineMissed)` + 不进
  failure 统计；CancelIfLate 执行 + 令牌可观测请求 + 协作完成。
- 依赖传播矩阵：丢弃上游 → 下游 `DependencyCancelled`；RunAnyway /
  协作完成上游 → 下游正常执行；未错过 tracked 丢弃策略零作用。
- 指标守恒：每次严格错过恰计一次 missed；dropped 只归 DropIfLate。

## 12. 0.7.1 ResourceLease / 资源预留（roadmap §3.1，开发快照）

闭合 §6.4 记录的 TOCTOU 窗口的 facade 级方案：admission 时从**显式
声明的静态配额**中真正扣留资源，任务终态归还。

- **opt-in（默认关闭）**：`ExecutorConfig::enable_resource_reservation`
  + `resource_quota`（`ResourceQuota`：GPU 显存桶、GPU 并发槽桶、
  CPU 独占核总量）。未启用时注册表不创建，所有提交路径行为与开销
  与 0.7.0 完全一致。配额是显式声明值而非能力快照读数——预留是
  facade 级的确定性簿记，闭合"多个声明相同资源的提交在 admission
  与执行之间超卖"的窗口；硬件层弱一致语义（0.6.1 契约）不变。
- **需求面**：`ResourceRequirements` 新增 `gpu_concurrent_slots` /
  `cpu_exclusive_cores`（`memory_bytes` 复用为 GPU 显存需求）。
  消费面：`submit_auto(TaskBuilder)` 与 tracked TaskBuilder 重载。
  `CpuGpuTask` 的 GPU 投递路径本阶段不接入。
- **授予语义（全或无）**：联合需求任一维度不足则整体拒绝、零部分
  授予；GPU 维度未指定 device → `InvalidRequirement`，device 未在
  配额中声明 → `NotConfigured`。互斥锁保护——预留只发生在声明资源
  且启用预留的提交上，不在未声明快车道。
- **决策一致性**：submit_auto 在记录决策**前**尝试预留，失败则改写
  为 `CapacityPressure` + `ResourceInfeasible` 诊断位（计入既有
  `resource_rejected_count`），保持 0.6.1 的"决策与实际投递结果
  一致"不变式；tracked 路径以结构化拒绝结算（未创建图节点）。
- **终态释放**：`detail::TerminalReleaser`（CAS 恰好一次 + 析构
  兜底）经 `submit_priority_scheduled` / `submit_tracked_with_hook`
  新增的 `terminal_release` 参数接入既有 admission 释放链——成功、
  异常、协作取消、DropIfLate 丢弃、queued 软超时、提交拒绝、
  shutdown 排空丢弃排队任务全部路径均归还（admission 机制的
  CR-010/011 级异常路径覆盖原样复用）。
- **手动入口**：`Executor::try_reserve(ResourceRequirements) ->
  ReservationResult{ResourceLease, ReserveError}`（RAII、move-only、
  `release()` 幂等）供任务绑定之外的用途；`Executor::
  get_resource_reservation_status()` 输出 granted/rejected/active
  计数与在租量。
- **后续联动（未落地）**：调度器把 lease 可得性作为 candidate
  filter 输入（roadmap §3.1）；能力缓存快照包含资源可用量供 auto
  路由预过滤（roadmap §3.0 联动）。
- **验收**：`tests/test_resource_lease.cpp`（12 用例）——opt-in
  边界、手动授予/拒绝/RAII、GPU 各维度、任务绑定预留终态归还、
  并发超卖结构化拒绝、终态释放矩阵（异常/软超时/丢弃/admission
  拒绝）、tracked 预留与拒绝、多维全或无。

## 13. 0.7.1 任务特征自动推断与执行器自动选择（roadmap §3.0.2/§3.0.3，开发快照）

### 13.1 任务特征自动推断（§3.0.2，opt-in）

`ExecutorConfig::enable_auto_inference`（默认关闭；运行期
`set_auto_inference_enabled()` 可调）。启用时：

- **采样**：submit_auto 附加测量包装（与 `wants_feedback()` 同级开销；
  反馈门为 `wants_feedback() || enable_auto_inference`），完成反馈经
  `report_scheduling_feedback` 喂入 `detail::TaskInferenceTable`——
  按任务名的有界历史表（64 键，满后丢弃新名称并计数；record 在
  worker 线程、analyze 在提交线程，互斥锁保护）。表内维护执行时长
  EWMA 与完成间隔 EW 均值/方差（完成间隔近似提交间隔，文档化偏差）。
- **推断规则（确定性，全部只建议不切换）**：
  - blocking I/O：名称含 `_io_` / `_read_` / `_write_`（纯名称模式
    即命中——建议的误报代价只是噪音，历史时长仅作为
    `estimated_duration_ns` 报告值）；
  - 低延迟：名称含 `_realtime_` / `_control_`；
  - periodic：≥4 个完成间隔且 σ/μ < 10%。
- **注解（建议优先于切换）**：submit_auto（裸 builder 两路）在路由
  后注解 `RoutingDiagnostics::AutoInferredIntent`（`1u<<5`）位 +
  新增 `RoutingDecision::inferred_reason` 字段 + detail 建议
  （blocking → start_worker；低延迟 → dispatch_auto(LowLatency)；
  periodic → submit_periodic），计 `auto_inferred_count`。
  **路由不被改变**——submit_auto 的 `std::future` 契约只有默认池能
  履行（lockfree/realtime 是有界 dispatch 语义，无 future 通道），
  这正是 §3.0.3 "建议优先于自动切换"原则的强制形态（对路线图草图
  中 `intent = LowLatency` 直接改写的一处有意偏差）。
- **显式优先**：用户显式 intent（≠ Auto）时推断被跳过，若有话可说
  计 `auto_inference_rejected_count`（"推断结果被用户显式设置覆盖"）。
  提交时点被拒绝的决策不注解。

### 13.2 有界 dispatch 的执行器自动选择（§3.0.3.1，随调度器 opt-in）

`IScheduler` 新增虚方法
`select_bounded_executor(intent, candidates) -> optional<string>`：
仅在用户声明 LowLatency/RealtimeQueue 意图但**未**显式指定
preferred_executor 时被 `route_dispatch` 咨询；默认实现返回空
（DefaultScheduler 与 0.6.x 自定义调度器行为不变，策略拒绝保持）。
`AdaptiveScheduler` 覆写：LowLatency 意图下在 lockfree 候选中按
**瞬时队列负载**（pending_work 相对 capacity_hint 占用率；未知容量
按绝对 pending_work）选最空闲者，并列取名字序（确定性）；
registered/running 之外的候选与已满候选被过滤，无可用候选返回空。
选择依据是能力快照（经 §10 的 TTL 缓存，§3.0 联动的"缓存的能力
快照包含资源可用量"形态之一），**不依赖执行期历史**——lockfree
后端的反馈通道在 v0.7.2（§4.1）才接入，因此这里不做历史负载；
命中时决策带 `RoutingDiagnostics::AutoExecutorSelection`（`1u<<6`）
位 + detail 负载说明，计 `auto_executor_selected_count`。
RealtimeQueue 不做负载选择（周期语义）。

### 13.3 与路线图的偏差与未落地项

- `RoutingDecision::original_intent` 未添加：推断从不改写 intent
  （建议优先于切换），原始意图恒等于 requested_intent，字段无信息量。
- §3.0.3.2（动态意图调整建议）并入 §3.0.2 的建议注解形态。
- §3.0.3.3（冷启动保守启发式）由 0.7.0 M3 的既有行为覆盖
  （min_samples 不足回退 0.6.1 启发式）。
- 验收线偏差：§3.0.3 的"多 lockfree 场景 p99 queue_wait 降低 >20%"
  依赖 lockfree 后端的 queue_wait 测量（v0.7.2 §4.1 反馈接入），
  本阶段以"最小负载当选的确定性断言 + 并列/过滤/回归守卫"替代
  （`tests/test_auto_inference.cpp`，11 用例）；"热路径 <80ns"由
  §3.0.1 的缓存命中路径覆盖（898ns 端到端含提交协议，路由增量同
  §3.0.1 实测）。

### 13.4 验收（`tests/test_auto_inference.cpp`）

opt-in 边界（默认零建议零计数）；blocking/低延迟模式注解与路由
不变；显式 intent 覆盖计数；运行期开关回退；TaskInferenceTable
EWMA/periodic/键空间上界单元；自动选择单元矩阵（最小负载、并列
名字序、未运行/已满过滤、RealtimeQueue/空候选）；dispatch_auto
集成（AdaptiveScheduler 注入后自动选择 + 指标；DefaultScheduler
回归守卫）。

## 14. 0.7.2 优先级语义转移到调度器（roadmap §4.0，架构清理）

### 14.1 语义不一致问题

0.7.1 及之前，优先级调度存在两条路径：

1. **submit_priority API 路径**：直接提交到优先级队列，绕过调度器抽象
   ```cpp
   executor.submit_priority(TaskPriority::HIGH, []{ work(); });
   ```
2. **submit_auto + TaskBuilder 路径**：经调度器 `effective_priority_for()` 咨询
   ```cpp
   executor.submit_auto(TaskBuilder([]{ work(); }).priority(TaskPriority::HIGH));
   ```

问题：

- **隐式依赖**：submit_priority 与 submit_auto 共享 PriorityScheduler，
  因此隐式携带了优先级调度属性。这与 submit 系列的定位（§10.2：
  "从不产出路由决策"的快速提交路径）相矛盾。
- **抽象不完整**：优先级本质上是调度策略维度（与 deadline/QoS/affinity/
  resource 正交），但作为独立提交 API 暴露，绕过了 IScheduler 抽象。
- **演进不一致**：0.7.0 M3 建立了 `effective_priority_for()` 机制，
  0.7.1 的 deadline/QoS 通过 TaskBuilder 表达，而优先级仍保留独立 API。

### 14.2 重构方案（0.7.2 唯一破坏性变更）

优先级完全通过 TaskBuilder 表达，移除 `submit_priority` API：

```cpp
// 0.7.2+ 统一路径
executor.submit_auto(TaskBuilder([]{ work(); }).priority(TaskPriority::HIGH));
```

**语义等价性**：
- TaskBuilder 已有 `priority()` 方法，TaskOptions 已有 `priority_set` 标记
- `effective_priority_for()` 咨询机制保持不变（显式 priority 永不被调度器覆盖）
- 优先级 + EDF + FIFO 的调度逻辑完全相同（PriorityScheduler 内部实现不变）
- submit_auto 的优先级路径复用 `submit_priority_scheduled` 内部实现

**破坏性变更时间线**：
- 0.7.2-rc1：标记 `[[deprecated]]`，文档与示例迁移
- 0.7.2-rc2：验证零警告
- 0.7.2 正式版：移除 API（保留 `submit_priority_scheduled` 内部实现）

**迁移模式**（自动化脚本 `scripts/migrate_submit_priority.sh`）：
```cpp
// 简单替换
submit_priority(p, fn) → submit_auto(TaskBuilder(fn).priority(p))

// 带捕获 lambda
submit_priority(p, [&]{ ... }) → submit_auto(TaskBuilder([&]{ ... }).priority(p))

// tracked 依赖图（0.7.1 已支持）
submit_after(dep, TaskBuilder(fn).priority(p))
```

### 14.3 实现要点

- **内部实现不变**：PriorityScheduler 四队列结构、EDF 排序、aging
  机制全部保持不变，只是访问路径统一为 TaskBuilder → TaskOptions →
  `submit_priority_scheduled`。
- **性能无回归**：submit_auto 裸 builder 已在 §10.3 验收线内
  （配对 A/B ±2%）；优先级路径复用既有实现，无额外开销。
- **调度语义不变**：迁移前后的任务执行顺序（priority → EDF → FIFO）
  逐位一致；aging / effective_priority_for 咨询逻辑不变。

### 14.4 边界与风险

- **这是 0.7.x 唯一破坏性变更**：roadmap §1 原计划"0.7.2 additive +
  修复"被此项打破，但延后至 0.8.0 会让 0.7.x 内部调度语义不一致，
  造成更大的迁移负担。
- **影响面可控**：facade 层测试调用 8 处（规划阶段高估为 256 处，
  2026-10-19 实施复查勘误）+ 4 处示例，迁移模式清晰，
  自动化脚本覆盖主要场景。
- **文档同步**：`docs/API.md` §3.2 移除 submit_priority，
  website 教程页（priority.md）全面改写为 TaskBuilder 示例。

### 14.5 验收

- facade 层测试调用全部迁移（实际 8 处），零 deprecated 警告
- 配对 A/B 基准：迁移前后端到端吞吐 ±2% 内
- 契约测试：优先级 + EDF 排序、aging、effective_priority_for 咨询逻辑不变
- `scripts/check_docs_drift.sh` 通过
- MIGRATION.md 0.7.2 节补充语义等价性证明与迁移指南
