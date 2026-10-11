# Kairo v0.7.x 开发大纲

> 状态：**0.7.0 已发布**（2026-10-10，M0 至 §2.5 全部落地，tag v0.7.0）。基线：v0.6.1。
> 进度：**0.7.0 M0 前置清债已完成**（§2.1 五项全部落地，基准对比见
> `docs/performance/m0_debt_paydown_results.md`，变更清单见 CHANGELOG
> Unreleased 节；分支 refactor/split-executor-facade、
> perf/task-monitor-sharding、perf/task-allocation-baseline、
> feat/optional-priority-aging、perf/dependency-topo-order）。
> **M1 pipeline 化已落地**（§2.2，分支 refactor/scheduler-route-pipeline）。
> **M2 反馈聚合层已落地**（§2.3，分支 feat/feedback-aggregator）。
> **M3 AdaptiveScheduler MVP 已落地**（§2.4，分支 feat/adaptive-scheduler）。
> **0.7.0 文档与发布已落地**（§2.5，分支 docs/v070-website-migration-sync）；
> 0.7.0 里程碑全部完成，下一版本为 0.7.1（§3）。
> 输入：`docs/design/scheduling_runtime.md` §5/§6、`docs/CODE_REVIEW_2026-09-30.md`
> 未结项、`CHANGELOG.md` 0.6.x 边界声明。

## 0. 系列主题与原则

**主题：Runtime-aware Scheduling——从"可观测的调度"走到"利用观测的调度"，
同时闭合 admission 与 execution 之间的资源 TOCTOU 窗口。**

0.6.0 建立了调度模型，0.6.1 建立了 measurement contract。0.7.x 要回答的是：
反馈怎样以**可解释、可关闭、不震荡**的方式影响调度决策。

贯穿全系列的原则（延续项目既有风格）：

1. **自适应是 opt-in**。`DefaultScheduler` 保持确定性，行为与 0.6.1 逐项一致；
   自适应策略以新的 `AdaptiveScheduler`（或策略组件）通过 `set_scheduler()` 注入。
   `Auto` 不会因为"历史上更快"就静默切换语义不同的后端（与"Auto 不静默选
   lockfree/realtime"的承诺一致）。
2. **每个自适应决策都可解释**。命中历史时使用已预留的
   `RoutingReason::AdaptiveHistory`，诊断位和 `SchedulingMetrics` 同步扩展；
   不得只靠 `detail` 字符串表达。
3. **热路径预算先定、后实现**。以 `tests/benchmark_scheduling_paths.cpp` 为基线，
   每项特性都先写下"关闭时零开销 / 开启时 ≤ X ns"的验收线。
4. **先清债，再加策略**。会污染反馈测量或放大自适应风险的遗留项，优先于新特性。
5. 沿用流程：设计文档 → 独立验证代理编写契约测试 → 基准 → `check_docs_drift.sh`
   → website/skill 同步（`docs/RELEASE_CHECKLIST.md`）。

## 1. 版本切分总览

| 版本 | 主题 | 性质 |
|---|---|---|
| 0.7.0 | 调度基础整备 + 反馈闭环 MVP（opt-in AdaptiveScheduler） | additive 为主；如需破坏性变更，只放在本版 |
| 0.7.1 | Resource Reservation（ResourceLease / ReservationToken）+ deadline policy | additive |
| 0.7.2 | 反馈覆盖扩展到 GPU / lockfree / realtime + 生命周期可靠性收尾 | additive + 修复 |
| 0.7.3 | `CpuGpuTask<T>` 返回值 / `ExecutionReport<T>` + 0.7 系列稳定化 | additive |

0.7.3 可以视进度并入 0.7.2，或推迟到 0.8。

---

## 2. v0.7.0 — 调度基础整备 + 反馈闭环 MVP

### 2.1 前置清债（M0，先于任何自适应代码合入）✅ 已完成（2026-10-10）

| 项 | 问题 | 为何是 0.7.0 前置 | 状态 |
|---|---|---|---|
| CR-071 / CR-163 | `TaskMonitor` 单把全局 mutex 串行化提交/执行热路径；采样率为 0 时仍然取锁 | 反馈测量的 `queue_wait_ns` 和 `execution_duration_ns` 会混入监控锁的争用，自适应会学到噪声 | ✅ 分片 + 采样门控（perf/task-monitor-sharding） |
| CR-107 | 每个任务 6-10 次堆分配 | 反馈包装还要再加开销；先把基线压下来，验收线才有意义 | ✅ 第一阶段：10→9 allocs/task + 计数基准入库（perf/task-allocation-baseline） |
| CR-024 + NN-05 | 严格优先级无老化，LOW 无限饿死；本地队列优先级倒置 | 自适应会动态调整优先级/QoS，没有防饿死机制会放大饿死。先提供可选的 aging policy（默认关闭，保持现有语义） | ✅ 可选 aging 落地，NN-05 维持文档化（feat/optional-priority-aging） |
| CR-052 遗留 | 依赖建链 O(n²)、环检测在锁内遍历（n=4000 约 3.9s） | 0.7.1 的 deadline policy 会与依赖图交互；改为增量拓扑序 | ✅ 增量拓扑序，n=20000 正序链 21-36ms（perf/dependency-topo-order） |
| executor.cpp 拆分 | 单个文件 2242 行，承担门面、路由、决策记录、tracked 图 | 新策略代码不再往巨石里堆。按 routing / tracked graph / lifecycle / gpu facade 拆成独立编译单元，**纯结构改动，单独 PR，零行为变化** | ✅ 五编译单元 + detail 头（refactor/split-executor-facade，第一个合入） |

验收（已达成）：全量测试绿（171/171）；`benchmark_scheduling_paths` 和
`benchmark_thread_pool_hotpath` 无回归（配对 A/B）；CR-071 修复后，多线程
提交吞吐在采样率为 0 时与关闭监控持平（±5%，实测 ±1.2%）。

### 2.2 DefaultScheduler 内部 pipeline 化（M1）✅ 已落地（2026-10-10，refactor/scheduler-route-pipeline）

按 §5 的既定方向，把 `route()` 内部重构为

```
constraint filter → candidate generation → scoring/ranking → selection
```

- `IScheduler` 对外接口不变；`DefaultScheduler` 的 scoring 阶段为空实现，结果逐项等同 0.6.1。
- pipeline 的各阶段作为可组合组件对外开放（`include/kairo/scheduling_pipeline.hpp`：
  Deadline/GpuResource 约束过滤器、`ConstraintFilterChain<>`、
  `IntentCandidateGenerator`、`IdentityScoring`、`select_first`、
  `apply_affinity_advisory`），`AdaptiveScheduler` 复用 filter / candidate，只替换 scoring。
- 热路径形状约束（实测沉淀）：公开阶段函数是普通外联定义（可链接、可组合），
  热路径经由的内部实现函数以 always_inline 折叠进
  `SchedulingPipeline<IdentityScoring>` 的显式实例化——未折叠的阶段边界
  实测各值 ~10ns，候选中间结构二次物化实测 +10ns 级。
- 验收：0.6.1 的契约测试（`test_scheduling_contract_v061.cpp`）一字不改通过；
  route 纯策略路径配对 A/B 实测快于 0.6.1 基线约 4%（36.6ns → 35.0ns，
  4 轮方向一致）。

### 2.3 反馈聚合层（M2）✅ 已落地（2026-10-11，feat/feedback-aggregator）

`on_task_completed()` 在 worker 线程同步调用，因此聚合必须满足：不阻塞、不分配、不抛异常。

- 新增 `FeedbackAggregator`（`include/kairo/feedback_aggregator.hpp`）：per-worker 分片
  （固定 32 片，thread_local 槽位稳定映射）的 EWMA / 分桶直方图（queue wait、执行时长、
  失败率、deadline 错过），按 `(backend, executor_name, qos)` 分键；读方只读周期性
  合并出的快照（原子 shared_ptr 指针交换，RCU 风格，读路径无锁）。设计语义见
  `docs/design/scheduling_runtime.md` §7，基准见 `docs/performance/m2_feedback_aggregator_results.md`。
- 键空间有界（每分片默认 16 槽、上限 64，支持 `register_key()` 预注册），超限就退化为
  "未知即宽容"（丢弃 + `dropped_samples` 计数），与 §6.4 的弱一致语义一致。
- 对外提供 `Executor::get_feedback_snapshot()`（诊断用），并接入 `get_snapshot_text()`
  （追加 `scheduling_feedback.*` 段）。
- 验收（已达成）：开启聚合时每任务聚合开销实测 23.8ns/task（预算 ≤ 100ns，验收线
  内置于 `benchmark_feedback_aggregator`）；facade 端到端聚合差值在噪声内不可分辨；
  TSAN 全绿；默认调度器路径配对 A/B ±1.1% 内（零变化）。

### 2.4 AdaptiveScheduler MVP（M3）✅ 已落地（2026-10-11，feat/adaptive-scheduler）

范围刻意收窄到三类有明确收益、可解释的决策：

1. **CPU / GPU 选择**（`CpuOrGpu` 意图与 `CpuGpuTask`）：用历史的端到端时延取代静态 heuristic。
   带滞回（hysteresis）和最小样本数，防止震荡；样本不足时回落到 heuristic。
2. **QoS 感知的 admission / 降载**：某 QoS 等级的 queue wait p99 持续超过目标时，对低 QoS
   提交返回结构化拒绝（新增 reason，例如 `LoadShedding`），而不是让所有任务一起变慢。
   与 `max_in_flight_tasks` 的硬上界共存，只做更早、更有区分度的拒绝。
3. **QoS → priority 动态映射**（依赖 2.1 的 aging）：长期 queue wait 超标的 QoS 等级可以有界提升。

明确不做：NUMA、跨 pool 迁移、基于 utilization 的线程数调整（已有 resizer，避免两个控制回路相互打架）。

落地形态：`AdaptiveScheduler`（`include/kairo/adaptive_scheduler.hpp`）复用 M1 pipeline
与 M2 聚合器，三类决策全部命中预留的解释通道（`RoutingReason::AdaptiveHistory` /
新增 `LoadShedding`、诊断位、`SchedulingMetrics` 三个新计数）；窗口证据按窗口差分
（累计直方图无遗忘，会阻塞恢复与 fail-open）；开/关对称滞回。设计语义见
`docs/design/scheduling_runtime.md` §8。

验收：
- ✅ 合成负载下 CPU/GPU 选择收敛且交替负载不翻转（阈值内置于
  `tests/test_adaptive_scheduler.cpp`：双侧 min_samples 后 ≤2×min_samples 条收敛；
  滞回边际内维持在用侧；44 用例含交替负载零翻转）；
- ✅ 降载场景中高 QoS 的 p99 queue wait 明显优于 DefaultScheduler
  （绑核配对：Interactive 探针端到端 p99 自适应 1.3-4.2ms vs
  10.8-16.9ms，160-168 次结构化拒绝；记录到
  `docs/performance/m3_adaptive_scheduler_results.md`）；
- ✅ 每个自适应决策都能从 `RoutingDecision` 与 `SchedulingMetrics` 复原原因；
- ✅ 热路径预算先定后实现：route 纯策略配对差 18.8ns（线 150ns）、
  CpuOrGpu 冷路径 30.5ns（线 250ns）、on_task_completed 22.6ns（线 150ns）、
  effective_priority_for 0.74ns（线 50ns）；默认路径配对 A/B vs M2 head ±5% 内；
  TSAN 白名单全绿。

### 2.5 文档与发布 ✅ 已落地（2026-10-10，docs/v070-website-migration-sync）

- ✅ `docs/design/scheduling_runtime.md` 新增 §7 "0.7.0 Runtime-aware Scheduling"：随 M2/M3 落地（§7 反馈聚合层、§8 AdaptiveScheduler 设计语义，§9 测试与验证）。
- ✅ website 新增 "何时使用 AdaptiveScheduler / 何时不该用" 决策页（中英 `website/{zh,en}/guides/adaptive-scheduling.md`），重点写震荡、冷启动、不可复现性；版本与迁移页新增 0.7.0 开发快照小节；API 覆盖索引接入调度策略行；GPU 自动选择 / 执行模型 / 调度运行时教程页补开发快照交叉注记。
- ✅ MIGRATION：0.7.0 节（确认无必需迁移；记录 `effective_priority_for` 默认实现、`RoutingReason` 穷举 switch 注意事项与 opt-in 行为差异）。`docs/API.md` §3.8 追加 0.7.0 开发快照文档；`docs/skill` 两张调度卡同步；`scripts/check_docs_drift.sh` 通过。

---

## 3. v0.7.1 — Resource Reservation + deadline policy

### 3.1 ResourceLease / ReservationToken

闭合 §6.4 记录的 TOCTOU 窗口：admission 时就真正保留资源，而不是只做可行性判断。

- 资源类型（首批）：GPU 显存额度、GPU 并发槽、CPU 独占核（§5 "独占核 reservation"）。
- 语义：`try_reserve(ResourceRequirements) -> expected<ResourceLease, ReserveError>`；lease 采用
  RAII，任务终态（成功、异常、取消、shutdown 排空）都必须释放。这是本版最主要的测试面，
  需要覆盖 0.5.3 修过的所有异常路径（CR-010/011 类泄漏不能复现）。
- 显式 opt-in：没有声明 `ResourceRequirements` 的提交路径行为不变，也不付任何代价。
- 与 0.7.0 联动：调度器可以把 lease 的可得性作为 candidate filter 输入。

### 3.2 deadline policy

在已有的 admit → EDF → miss-observe 之上，增加 per-task 策略：

- `RunAnyway`（默认，等于 0.6.x 行为）/ `DropIfLate`（出队时已过期则不执行，以
  `DeadlineMissed` 终态结算）/ `CancelIfLate`（向任务的 StopToken 发协作式取消请求，**不是抢占**）。
- 与依赖图交互：上游 DropIfLate 时，下游按既有的 `DependencyCancelled` 分类传播。
- 验收：deadline 边界（相等即接受）不漂移；三种策略 × 依赖传播的契约矩阵测试。

---

## 4. v0.7.2 — 反馈覆盖扩展 + 生命周期可靠性收尾

### 4.1 反馈接入其他后端

0.6.1 的反馈只覆盖默认异步池。本版把 measurement contract 扩展到：

- **GPU**：以 stream 完成回调计时（注意 CR-153：回调内不得调用 CUDA API，计时数据要转交出去处理）；
- **lockfree / realtime**：只提供聚合级反馈（周期内执行数、超预算次数），**不提供逐任务回调**，
  不破坏 RT 路径的无分配约束（随 NN-03 一起，把 realtime allocation guard 纳入 CI 构建矩阵）。

### 4.2 未结的可靠性项（来自 2026-09-30 评审）

| 项 | 问题 | 方向 |
|---|---|---|
| CR-054 | RT 内置循环 stop 延迟可达一个整周期；自停止后仍会调用用户回调 | 用可中断等待替代 `sleep_until` |
| CR-115 | shutdown 的 join 没有超时，超时参数给人"有界"的错觉 | 要么真正有界（带诊断的放弃 join），要么改名并写进文档 |
| CR-055 | BlockingIO 持 `lifecycle_mutex_` 调用户 `wakeup()`；并发 stop 无终结栅栏 | 锁外唤醒 + stop 栅栏 |
| CR-057 | SerialExecutionContext reservation 永不 publish 时整个上下文卡死 | reservation 超时 / 析构自动 abandon |
| CR-105 | `WorkerHandle` 持裸 `ExecutorManager*` | 弱引用 + 失效检测 |
| CR-043 / CR-129 | `Topic::publish` 异常导致部分扇出，且调用方拿不到部分完成信息 | 返回扇出结果，或给出明确的异常契约 |
| NN-09 | KeepLatest 饱和时退化为"只保留最新 1 条" | 修正语义或改名 |
| CR-113 / CR-114 | resize 中途失败的状态不一致；worker 内缩容会 join 自己 | 事务化 resize，拒绝自 join |
| CR-154 | CUDA 与 OpenCL 的 stop 语义不一致（排空 vs 取消） | 统一为显式的 `StopMode` |
| CR-023 / CR-050 一类 | 剩余的异常屏障缺口 | 逐项补齐，并配复现测试 |

其余 P2 项（CR-116/123/125/126/134/136/152 等）按"契约已文档化 / 补注释 / 修复"三类逐项
定性，结果回写到评审文档，避免长期悬挂。

---

## 5. v0.7.3 — 返回值语义补齐与系列稳定化

- `CpuGpuTask<T>` 带返回值 + 显式 `ResultAdapter<T>`（`unified_facade_and_auto_routing.md`
  第一阶段延后项），`ExecutionReport<T>` 报告实际选择的路径。以新 overload 形式加入，
  不改变 `void` 版本。
- 0.7 系列整体回归：长时间 soak（自适应开启，混合负载 ≥ 24h）、ASAN/TSAN/UBSAN 全矩阵、
  Android NDK smoke、Windows MinGW/MSVC。
- 冻结 0.7 调度接口，为 0.8 预留的方向写进设计文档（NUMA topology、跨 pool locality、transfer cost）。

---

## 6. 风险与待决事项

| 风险 / 决策点 | 建议 |
|---|---|
| 自适应引入不可复现的行为，用户难以排障 | 默认关闭；提供 "冻结快照" 模式，把聚合快照固定下来以便复现；决策全量结构化记录 |
| 自适应与 ThreadPoolResizer 形成两个控制回路，互相打架 | 0.7.x 的自适应不调整线程数，只做路由、admission 和优先级 |
| 反馈在 worker 线程同步执行，用户自定义调度器可能阻塞 worker | 保留 0.6.1 契约；文档和 debug 构建中加入耗时断言 |
| 0.7.0 是否允许破坏性变更 | 建议**不破坏**：0.6.0 刚完成清理窗口，所有新能力都可以 additive 落地 |
| executor.cpp 拆分与新特性并行开发会冲突 | 拆分作为 0.7.0 的第一个 PR 单独合入，之后再开特性分支 |
