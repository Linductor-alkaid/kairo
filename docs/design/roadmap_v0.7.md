# Kairo v0.7.x 开发大纲

> 状态：**0.7.1 已发布**（2026-10-14，§3 全部落地，tag v0.7.1）。基线：v0.7.0。
> 进度：**0.7.0 M0 前置清债已完成**（§2.1 五项全部落地，基准对比见
> `docs/performance/m0_debt_paydown_results.md`，变更清单见 CHANGELOG
> [0.7.0] 节；分支 refactor/split-executor-facade、
> perf/task-monitor-sharding、perf/task-allocation-baseline、
> feat/optional-priority-aging、perf/dependency-topo-order）。
> **M1 pipeline 化已落地**（§2.2，分支 refactor/scheduler-route-pipeline）。
> **M2 反馈聚合层已落地**（§2.3，分支 feat/feedback-aggregator）。
> **M3 AdaptiveScheduler MVP 已落地**（§2.4，分支 feat/adaptive-scheduler）。
> **0.7.0 文档与发布已落地**（§2.5，分支 docs/v070-website-migration-sync）；
> 0.7.0 里程碑全部完成，下一版本为 0.7.1（§3）。
> **0.7.1 §3.0.1 已落地**（能力快照 TTL 缓存；曾规划的 submit_fast 经
> 2026-10-14 评审实测与 submit 语义重复且慢 31.4%，移除保留 submit，
> 见 `scheduling_runtime.md` §10.2；分支 feat/v071-capability-cache）。
> **§3.2 已落地**（deadline policy +
> 依赖传播，`scheduling_runtime.md` §11）。**§3.1 已落地**（资源预留
> ResourceLease，`scheduling_runtime.md` §12）。**§3.0.2/§3.0.3 已
> 落地**（自动推断建议 + 执行器自动选择，`scheduling_runtime.md`
> §13）——**§3 全部落地**；0.7.1 剩发布收尾（定版 + §5.2 稳定化）。
> 输入：`docs/design/scheduling_runtime.md` §5/§6、`docs/CODE_REVIEW_2026-09-30.md`
> 未结项、`CHANGELOG.md` 0.6.x 边界声明。
> **0.7.2 §4.0 rc1 已落地**（submit_priority 标记 deprecated + facade 层
> 测试/示例迁移 + 迁移脚本与文档，2026-10-19，
> 分支 feat/v072-priority-transfer）。
> **§4.1 堆分配优化阶段一已落地并通过独立验证**（Task 节点池 +
> facade 单控制块整合 + AllocationMetrics 观测，9→7 allocs /
> 768→512B，A/B 全线达标，TSAN/ASAN 干净，2026-10-19，实施记录见
> `allocation_optimization_proposal.md` §9）。
> **§4.2 反馈覆盖扩展已落地并通过独立验证**（GPU stream 完成回调计时
> + lockfree/realtime 聚合级反馈 + NN-03 guard 入 CI 矩阵，2026-10-19，
> 实施记录见 `scheduling_runtime.md` §15）；
> 0.7.2 余项：§4.0 正式移除（发布收尾）、§4.3 可靠性收尾。

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
| 0.7.2 | **优先级语义转移到调度器（破坏性）** + 堆分配优化 + 反馈覆盖扩展 + 生命周期可靠性收尾 | **breaking（仅 submit_priority 移除）** + additive + 修复 |
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

## 3. v0.7.1 — submit_auto 智能化增强 + Resource Reservation + deadline policy

### 3.0 submit_auto 路由智能化（新增，来自 2026-10-13 设计评审）✅ 全部落地（§3.0.1/§3.0.1 缓存 + §3.0.2/§3.0.3，feat/v071-capability-cache）

**问题**：`submit_auto` 虽名为"auto"，但路由能力未充分体现：
- 默认 `DefaultScheduler` 是确定性的意图路由，未利用执行反馈
- 性能开销相对 `submit` 慢 65-70%（CR-106 已部分优化 CPU-only 路径）
- 用户仍需显式指定 `LowLatency`/`RealtimeQueue` 的 `preferred_executor`

**方向**：分三阶段增强 auto 路由的智能性和性能。

#### 3.0.1 能力快照缓存与快速路径（短期，预计 1-2 周）✅ 已落地（2026-10-14）

> 落地形态说明：缓存载体为 Executor 内整表不可变快照（`manager_epoch` +
> `collected_ns` + capabilities vector）经 M2 同款 RCU 槽发布，未按下方
> 草图采用 per-backend 类型原子分片——route 消费的是整张能力表，分片
> 命中路径需每次重组 vector（热路径堆分配），与快速路径目标冲突。
> 主动失效由 manager `state_epoch` 承担（注册/启停/shutdown 全覆盖，
> facade 直接启停 realtime / blocking-IO 的两处转换补
> `notify_backend_state_changed()`）。设计语义与验收数据见
> `docs/design/scheduling_runtime.md` §10。
>
> **2026-10-14 评审后记**：submit_fast 移除——实测与 submit 语义重复
> （submit 系列本就不产出路由决策）且普通通道快 31.4%（1881 vs 2473
> ns/op，优先级通道的 promise/atomic_bool/bind 间接层更重）。基准 §6
> 改为 submit vs submit_auto 守卫。

- **能力快照 TTL 缓存**：当前每次 `submit_auto` 都锁定并采集执行器能力（CR-106 优化了
  CPU-only 路径走纯策略判定）。引入 TTL-based 缓存（默认 100ms），热路径读取缓存快照：
  ```cpp
  std::shared_ptr<const std::vector<ExecutorCapability>> cached_capabilities_;
  std::atomic<int64_t> capabilities_cache_timestamp_{0};
  constexpr int64_t CACHE_TTL_NS = 100'000'000; // 100ms
  ```
- **按后端类型分片缓存**：不同后端类型（CPU/GPU/LockFree/Realtime）分开缓存，避免
  全量锁竞争：
  ```cpp
  struct CapabilityCache {
      std::atomic<ExecutorCapability*> default_cpu{nullptr};
      std::atomic<ExecutorCapability*> gpu{nullptr};
      std::atomic<ExecutorCapability*> lockfree{nullptr};
      std::atomic<ExecutorCapability*> realtime{nullptr};
  };
  ```
- **零路由开销快速路径**：为最常见的 CPU-only 无约束场景提供 `submit_fast()`：
  ```cpp
  template<typename F, typename... Args>
  auto submit_fast(F&& f, Args&&... args) {
      // 跳过路由决策，直接提交到默认池（适用于确定是 CPU-only 且无特殊约束）
      return submit_priority_scheduled(
          static_cast<int>(TaskPriority::NORMAL),
          TaskSchedulingMeta{},
          std::forward<F>(f),
          std::forward<Args>(args)...
      );
  }
  ```

**验收线**：
- 能力缓存命中时，CPU/GPU 选择路径从 ~120ns 降至 ~60ns（50% 改进）
- `submit_fast()` 路径 ~35ns，相比当前 `submit_auto` 纯策略路径 ~50ns 有 30% 改进
- 配对 A/B 基准，TSAN 全绿

#### 3.0.2 任务特征自动推断（中期，预计 1-2 月）✅ 已落地（2026-10-14，建议优先于切换）

> 落地形态说明：推断为**建议性**注解（`AutoInferredIntent` 位 +
> `inferred_reason` + detail 建议），不改写 intent/路由——submit_auto
> 的 `std::future` 契约只有默认池能履行，lockfree/realtime 是有界
> dispatch 语义（草图中的 `intent = LowLatency` 直接改写不可实现，
> 该形态留给 dispatch_auto 的显式意图表达）。历史为按任务名的有界
> 64 键表（`<kairo/task_inference.hpp>`）；opt-in
> `enable_auto_inference` 默认关闭。设计语义与偏差清单见
> `docs/design/scheduling_runtime.md` §13。

扩展 `TaskOptions` 的自动分析能力，减少用户显式指定意图的需求：

```cpp
struct TaskCharacteristics {
    bool is_blocking_io = false;      // 检测阻塞 I/O 调用特征
    bool needs_low_latency = false;   // 检测延迟敏感标记
    bool is_periodic = false;         // 检测周期性调用模式
    size_t estimated_duration_us = 0; // 基于历史的执行时长预估
    
    // 从 lambda 类型或历史统计推断
    template<typename F>
    static TaskCharacteristics analyze(const F& f, const std::string& task_name);
};

// 增强的 submit_auto 内部逻辑
template<typename F>
auto submit_auto(F&& f) {
    TaskOptions options;
    auto chars = TaskCharacteristics::analyze(f, options.name);
    
    // 自动推断意图（仅在用户未显式设置时）
    if (!options.intent_explicitly_set) {
        if (chars.is_blocking_io) {
            // 建议使用 BlockingWorker，记录诊断
            record_auto_intent_suggestion("blocking_io_detected");
        } else if (chars.needs_low_latency && chars.estimated_duration_us < 100) {
            // 历史显示适合 LockFree
            options.intent = ExecutionIntent::LowLatency;
            options.preferred_executor = select_best_lockfree_executor();
            record_adaptive_decision("auto_selected_lockfree");
        }
    }
    
    return route_and_submit(options, std::forward<F>(f));
}
```

**推断策略**：
- **阻塞 I/O 检测**：通过任务名称模式（`*_io_*`, `*_read_*`, `*_write_*`）和历史执行时长
  （如果 execution_duration > 10ms 且 CPU 占用率低）启发式判断
- **低延迟需求**：基于任务名称（`*_realtime_*`, `*_control_*`）和历史 queue_wait 敏感度
- **周期性检测**：同名任务的提交时间间隔稳定（标准差 < 均值的 10%）

**边界与限制**：
- 自动推断**仅在用户未显式设置 intent 时生效**（与 0.7.0 原则一致：opt-in 而非静默）
- 推断结果记录到 `RoutingDecision::diagnostics` 新增位 `AutoInferredIntent`
- 提供 `disable_auto_inference()` 配置选项，完全关闭推断回退到纯意图路由

#### 3.0.3 AdaptiveScheduler 的进一步扩展（与 3.0.2 并行）✅ 已落地（2026-10-14）

> 落地形态说明：①执行器选择优化经新虚方法
> `IScheduler::select_bounded_executor` 生效（AdaptiveScheduler 按能力
> 快照瞬时队列负载选 lockfree，并列取名字序；lockfree 反馈在 v0.7.2
> 接入前不做历史负载——冷启动即安全）；②动态意图调整建议并入 §3.0.2
> 的建议注解形态；③冷启动保守启发式由 M3 既有行为覆盖（min_samples
> 不足回退 0.6.1 启发式）。验收线偏差："多 lockfree p99 >20%"依赖
> lockfree queue_wait 测量（v0.7.2 §4.1），本阶段以最小负载当选的
> 确定性断言替代；"热路径 <80ns"由 §3.0.1 缓存命中路径覆盖。见
> `docs/design/scheduling_runtime.md` §13。

扩展 0.7.0 M3 的 `AdaptiveScheduler`，增加以下能力：

1. **执行器选择优化**：对于 `LowLatency` 意图，当有多个 lockfree executor 时，
   基于历史负载自动选择最优的 `preferred_executor`（当前必须用户显式指定）
2. **动态意图调整建议**：当检测到任务长期不适合当前路径时（如 CPU 任务实际
   queue_wait p99 < 1ms，适合 lockfree），记录建议到 `RoutingDecision::detail`，
   而不是静默切换
3. **冷启动优化**：在 min_samples 未达到前，使用更保守的启发式（如 GPU 优先
   仅在任务名称明确标记时才选择）

**与 0.7.0 原则的一致性**：
- 自动选择仍是 **opt-in**：`AdaptiveScheduler` 注入时开启，`DefaultScheduler` 保持 0.6.1 行为
- 每个自动决策可解释：新增 `RoutingDiagnostics::AutoExecutorSelection` 诊断位
- 不会静默切换语义不同的后端：建议优先于自动切换

**验收线**：
- 多 lockfree executor 场景下，负载不均时自动选择将整体 p99 queue_wait 降低 > 20%
- 冷启动期间（< min_samples）误判率 < 5%（通过合成负载矩阵验证）
- 热路径开销：能力缓存命中时 route 路径 < 80ns（相比 M3 的 ~120ns 进一步优化）

### 3.1 ResourceLease / ReservationToken ✅ 已落地（2026-10-14）

闭合 §6.4 记录的 TOCTOU 窗口：admission 时就真正保留资源，而不是只做可行性判断。

> 落地形态说明：配额为**显式静态声明**（`ExecutorConfig::resource_quota`，
> opt-in 默认关闭），而非能力快照读数——预留是 facade 级确定性簿记，
> 闭合的是多提交对同一资源声明维度的超卖窗口；硬件层弱一致契约不变。
> 终态释放复用 admission 释放链（CAS 恰好一次 + 析构兜底，CR-010/011
> 级异常路径覆盖原样复用）。首批维度：GPU 显存额度、GPU 并发槽、
> CPU 独占核（静态配额式，非 §5 的"按绑核独占"形态——后者需池
> 内核亲和调度支持，留待后续）。设计语义与验收见
> `docs/design/scheduling_runtime.md` §12。

- 资源类型（首批）：GPU 显存额度、GPU 并发槽、CPU 独占核（§5 "独占核 reservation"）。
- 语义：`try_reserve(ResourceRequirements) -> expected<ResourceLease, ReserveError>`；lease 采用
  RAII，任务终态（成功、异常、取消、shutdown 排空）都必须释放。这是本版最主要的测试面，
  需要覆盖 0.5.3 修过的所有异常路径（CR-010/011 类泄漏不能复现）。
- 显式 opt-in：没有声明 `ResourceRequirements` 的提交路径行为不变，也不付任何代价。
- 与 0.7.0 联动：调度器可以把 lease 的可得性作为 candidate filter 输入。
- **与 3.0 联动**：缓存的能力快照包含资源可用量，auto 路由可以预先过滤资源不足的候选。

### 3.2 deadline policy ✅ 已落地（2026-10-14）

在已有的 admit → EDF → miss-observe 之上，增加 per-task 策略：

> 落地形态说明：裁决点为开始执行时（facade 包装层，与 0.6.1
> miss-observe 同点同边界），而非池出队点——策略经由 facade 的
> deadline 包装表达，覆盖 submit_auto(TaskBuilder) 与新增的
> submit_after/submit_after_with_handle TaskBuilder 重载（依赖传播
> 由此成为真实可测路径）；`DropIfLate` 终态复用取消类约定
> （`TaskCancelled(DeadlineMissed)`，依赖方经既有归类规则收到
> `DependencyCancelled`）；`CancelIfLate` 的令牌经新增
> `kairo::get_stop_token()`（thread_local + RAII）暴露。设计语义与
> 验收矩阵见 `docs/design/scheduling_runtime.md` §11。

- `RunAnyway`（默认，等于 0.6.x 行为）/ `DropIfLate`（出队时已过期则不执行，以
  `DeadlineMissed` 终态结算）/ `CancelIfLate`（向任务的 StopToken 发协作式取消请求，**不是抢占**）。
- 与依赖图交互：上游 DropIfLate 时，下游按既有的 `DependencyCancelled` 分类传播。
- 验收：deadline 边界（相等即接受）不漂移；三种策略 × 依赖传播的契约矩阵测试。

---

## 4. v0.7.2 — 调度语义清理 + 堆分配优化（阶段一）+ 反馈覆盖扩展 + 生命周期可靠性收尾

### 4.0 优先级语义从执行器转移到调度器（新增，来自 2026-10-14 架构评审）

**问题分析**：当前 `submit_priority` 存在语义不一致：

- **隐式优先级依赖**：submit_priority 与 submit_auto 共享 PriorityScheduler 队列系统，
  因此隐式携带了优先级调度属性。这与 submit 系列的定位（§10.2："从不产出路由决策"
  的快速提交路径）相矛盾。
- **调度器抽象不完整**：优先级本质上是调度策略维度（与 deadline/QoS/affinity/resource
  正交），但当前作为独立提交 API 暴露，绕过了 IScheduler 抽象。
- **与演进方向不一致**：0.7.0 M3 已建立 `effective_priority_for()` 机制，实现了
  调度器对优先级的动态咨询；0.7.1 的 deadline/QoS 都通过 TaskBuilder/TaskOptions
  表达，而优先级仍保留独立 API 路径。

**重构方向**：将优先级语义完全转移到调度器层，删除 `submit_priority` API。

#### 4.0.1 语义重定向（第一阶段）

优先级调度通过 TaskBuilder 表达：

```cpp
// 旧 API（0.7.1 及之前）
executor.submit_priority(TaskPriority::HIGH, []{ work(); });

// 新 API（0.7.2+）
executor.submit_auto(TaskBuilder([]{ work(); }).priority(TaskPriority::HIGH));
```

**实现要点**：
- TaskBuilder 已有 `priority()` 方法，TaskOptions 已有 `priority_set` 标记
- `effective_priority_for()` 咨询机制保持不变（显式 priority 永不被调度器覆盖）
- submit_auto 的优先级路径与 QoS/deadline 共享同一调度协议
  （`submit_priority_scheduled` 内部实现复用）

#### 4.0.2 API 弃用与移除（破坏性变更，仅此一项）

```cpp
// include/kairo/executor.hpp
template<typename F, typename... Args>
[[deprecated("Use submit_auto(TaskBuilder(f).priority(p)) instead. "
             "Priority is a scheduling dimension, not a submission protocol. "
             "See MIGRATION.md v0.7.2 for details.")]]
auto submit_priority(int priority, F&& f, Args&&... args);
```

**时间线**（0.7.2 是 0.7.x 唯一破坏性变更窗口）：
- 0.7.2-rc1：标记 deprecated，文档与示例全部迁移
- 0.7.2-rc2：编译期警告验证（所有示例/测试零警告）
- 0.7.2 正式版：移除 API（保留内部 `submit_priority_scheduled` 实现供 facade 复用）

#### 4.0.3 迁移支持

**自动化脚本**（`scripts/migrate_submit_priority.sh`）：
```bash
# 简单替换模式（无捕获）
submit_priority(p, fn)  →  submit_auto(TaskBuilder(fn).priority(p))

# 带参数捕获的 lambda
submit_priority(p, [&]{ ... })  →  submit_auto(TaskBuilder([&]{ ... }).priority(p))
```

**迁移指南**（`docs/MIGRATION.md` 0.7.2 节）：
- 语义等价性证明（优先级 + EDF + FIFO 逻辑完全相同）
- 性能对比（submit_auto 裸 builder 已在 §10.3 验收线内，无回归）
- 特殊场景处理（tracked 依赖图中的优先级已在 0.7.1 §3.2 通过 TaskBuilder 重载解决）

#### 4.0.4 验收

> 计数勘误（2026-10-19 实施时复查）：规划阶段估计的"256 处测试代码"
> 与代码库现状不符——facade 层实际调用点为 **8 处**（分布于
> test_executor_facade / test_performance / test_task_cancellation /
> test_executor_failure_observability 四个测试），示例 **4 处**
> （basic_submit ×2、tutorial/02_priority ×2）。测试中的
> `ThreadPool::submit_priority`（test_thread_pool*）属内部层，按计划
> 保留。验收项按实际计数更新。

- [x] facade 层 8 处测试调用迁移完成，零 deprecated 警告（rc1，2026-10-19）
- [x] 4 处示例代码更新，文档同步（rc1，2026-10-19）
- [ ] 配对 A/B 基准：迁移前后端到端吞吐 ±2% 内
- [ ] 契约测试：优先级 + EDF 排序、aging、effective_priority_for 咨询逻辑不变
- [x] `docs/API.md` §3.2 改写为弃用注记 + TaskBuilder 优先级示例（rc1）
- [x] website 教程页（priority.md）全面改写，中英版本页新增 0.7.2 开发快照（rc1）
- [ ] `scripts/check_docs_drift.sh` 通过（随 rc1 提交验证）

**风险与边界**：
- 这是 0.7.x **唯一的破坏性变更**（与 roadmap §1 "0.7.2 additive + 修复"
  冲突，需明确标注为例外）
- 不影响内部实现：PriorityScheduler 保持不变，四队列结构与 EDF 逻辑复用
- 不改变调度行为：迁移后的任务执行顺序与 0.7.1 逐位一致

---

### 4.1 普通执行路径堆分配优化（新增，来自 2026-10-13 设计评审）

**问题**：CR-107 M0 已将 submit 主路径从 10 → 9 allocs/task，但剩余 9 次分配仍有优化空间。

**目标**：v0.7.2 实现阶段一优化（9 → 7 allocs/task），为后续阶段奠定基础。

**完整设计**：见 [allocation_optimization_proposal.md](allocation_optimization_proposal.md)

#### 4.0.1 Task 对象池（优先级最高）

- **问题**：每个任务都分配 Task 对象，只有 LockFreeTaskExecutor 有 wrapper 池。
- **方案**：扩展 ThreadPool 的 per-worker Task 对象池（lock-free LIFO 栈，容量 256）：
  ```cpp
  Task* acquire_task(size_t worker_id) {
      if (Task* task = task_pools_[worker_id].acquire()) {
          return task;  // 从池获取，零堆分配
      }
      return new Task{};  // 回退
  }
  ```
- **预期**：稳态 **-1 alloc/task**，命中率 > 95%。

#### 4.0.2 小对象 std::function 内联存储（SBO）

- **问题**：`std::function` 对小闭包（<= 16 bytes）也会堆分配。
- **方案**：实现 `InlineFunction<R(Args...), 32>` 用于 Task（32 字节内联存储）：
  - 限制：trivially-destructible 闭包，大小 <= 32 bytes
  - 超限回退到 `std::function`（保持兼容）
- **预期**：简单 lambda **-1 alloc/task**，覆盖率 70-80%。

#### 4.0.3 DrainBag 侵入式引用计数

- **问题**：DrainBag × 2 次堆分配（引用计数块）。
- **方案**：使用侵入式引用计数（intrusive_ptr），将计数嵌入对象：
  ```cpp
  class DrainBag {
      std::atomic<size_t> ref_count_;  // 内联计数，无额外分配
      // ...
  };
  ```
- **预期**：**-1 alloc/task**（引用计数块从 2 次降到 1 次）。

**验收线（阶段一）**（2026-10-19 落地，实测画像与偏差清单见
`allocation_optimization_proposal.md` §9：Task 池为 PriorityScheduler
内单一 Vyukov MPMC 空闲表（实施中经独立评审修正两处并发缺陷）；
InlineFunction 草图改为 facade 单控制块整合；DrainBag 前提不复现不
实施）：
- [x] `benchmark_submit_allocations`：9 → **7 allocs/task** (-22%)（实测 7.00）
- [x] 字节数：768 → **512.5 bytes/task**（-33%，优于 ~600 预期）
- [x] 配对 A/B 吞吐基准：不弱于 0.7.0 ±5%（独立验证 10 轮交错：p=1
      +22.6% 转正，p=2..16 全部容差内 -2.4%~+3.6%；分发回收链修复前
      曾测得 -8.6%/-12.7%，根因与回正机制见提案 §9.2.1）
- [x] Task 池命中率 > 95%（稳态，通过 `get_allocation_metrics()` 监控；
      串行 100%，并发形态 99.1%~100%）
- [x] ASAN/TSAN 全绿（独立验证：TSAN 15/15 零报告、ASAN 6/6 零报告；
      全量 ctest 175/180，5 失败均定性非本次引入——3× test_api_doc
      构建目录层级环境问题、benchmark_scheduling_paths 预存 flaky、
      test_thread_pool 负载敏感 flake 基线同类）

**可观测性**：新增诊断 API
```cpp
struct AllocationMetrics {
    std::atomic<uint64_t> task_pool_hit_count{0};
    std::atomic<uint64_t> task_pool_miss_count{0};
    std::atomic<uint64_t> inline_function_count{0};
    std::atomic<uint64_t> std_function_fallback{0};
};

class Executor {
    AllocationMetrics get_allocation_metrics() const;
    double get_task_pool_hit_rate() const;
};
```

### 4.2 反馈接入其他后端 ✅ 已落地（2026-10-19，实施记录见
scheduling_runtime.md §15）

0.6.1 的反馈只覆盖默认异步池。本版把 measurement contract 扩展到：

- **GPU**：以 stream 完成回调计时（注意 CR-153：回调内不得调用 CUDA API，计时数据要转交出去处理）；
- **lockfree / realtime**：只提供聚合级反馈（周期内执行数、超预算次数），**不提供逐任务回调**，
  不破坏 RT 路径的无分配约束（随 NN-03 一起，把 realtime allocation guard 纳入 CI 构建矩阵）。

**验收（2026-10-19 独立验证）**：
- [x] 聚合反馈：`AggregateExecutionStats` + facade 快照 API；
      realtime（executed/超周期/周期，与 get_status 同源）、lockfree
      （opt-in 执行预算，默认零新增采样）——11 用例
- [x] GPU 计时：`GpuKernelTimingRecord` + `submit_kernel_with_timing`
      sink 通道；CR-153 由"探针回调只做时间戳+转交，sink 一律执行器
      worker 线程"结构性履行；每提交恰一条记录（失败/停止路径逐点
      交付）——facade 组装层 5 用例 + CUDA 失败路径 3 用例（stub 编译）
- [x] 无逐任务回调、RT 无分配约束不破坏：RT 线程内仅 relaxed 原子
      计数；TSAN 子集零告警
- [x] NN-03：CI guard-ON job 全量构建 + realtime/lockfree/object_pool
      套件（本地复刻 24/24；随动修复 benchmark 全局 new 冲突与 500ns
      时延断言的分配器敏感性）
- [x] 全量回归 178/182（4 失败经 A/B 定性为预存容差 flaky 与 doc 测试
      cwd 环境问题，非本次引入）
- 残余风险：无 GPU 硬件，async 探针/sync 交付/晚到回调自弃仅 stub
  编译 + 契约层验证；OpenCL 计时 parity 属后续项

### 4.3 未结的可靠性项（来自 2026-09-30 评审）

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

## 5. v0.7.3 — 堆分配优化（阶段二）+ 返回值语义补齐

### 5.0 Promise/Future 优化（堆分配优化阶段二）

**目标**：在 v0.7.2（7 allocs/task）基础上进一步优化到 5-6 allocs/task。

#### 5.0.1 自定义轻量 Future 实现

- **问题**：`std::promise/future` 实现通常有 3-4 次堆分配（共享状态 + 控制块）。
- **方案**：实现 `kairo::TaskResult<T>` 作为新返回值类型（单次分配）：
  ```cpp
  // 新 API（opt-in）
  TaskResult<T> submit_v2(F&& f);
  
  // 旧 API（保持兼容，内部适配）
  std::future<T> submit(F&& f);
  ```
- **预期**：**-2 allocs/task**（共享状态从 3-4 次降到 1 次）。

#### 5.0.2 无等待快速路径

- **方案**：`TaskResult::get()` 添加快速路径（spin-wait + atomic）：
  - 快速路径：任务已完成，无需加锁（~10-20ns）
  - 短暂 spin：100 次 yield（几微秒）
  - 慢速路径：mutex + cv（与 std::future 相当）
- **预期**：快速路径命中率 > 60%（取决于任务粒度）。

**验收线（阶段二）**：
- [ ] `benchmark_submit_allocations`：7 → **5-6 allocs/task** (-17-29%)
- [ ] 字节数：~600 → ~400-500 bytes/task
- [ ] `TaskResult::get()` 快速路径命中率 > 60%
- [ ] 配对 A/B 吞吐基准：不弱于 v0.7.2 ±5%
- [ ] 全量测试 + 向后兼容性验证

**迁移策略**：
- 新 API 作为 opt-in，旧 API 保持兼容
- 0.8.0 前标记 `submit()` 为 deprecated
- 0.9.0 完全移除旧 API

### 5.1 返回值语义补齐

- `CpuGpuTask<T>` 带返回值 + 显式 `ResultAdapter<T>`（`unified_facade_and_auto_routing.md`
  第一阶段延后项），`ExecutionReport<T>` 报告实际选择的路径。以新 overload 形式加入，
  不改变 `void` 版本。

### 5.2 系列稳定化

- 0.7 系列整体回归：长时间 soak（自适应开启，混合负载 ≥ 24h）、ASAN/TSAN/UBSAN 全矩阵、
  Android NDK smoke、Windows MinGW/MSVC。
- 冻结 0.7 调度接口，为 0.8 预留的方向写进设计文档（NUMA topology、跨 pool locality、transfer cost）。

---

## 6. v0.8.0 — 堆分配优化（阶段三，实验性）+ 0.7 系列收尾

### 6.0 协程版本零分配模式（可选特性）

**前提**：C++20 协程支持，作为实验性特性。

- **方案**：利用协程栈上状态，避免多次堆分配：
  ```cpp
  // 协程版本 submit（零额外堆分配）
  template<typename F>
  auto submit_coro(F&& f) -> CoroTask<typename std::invoke_result<F>::type> {
      co_return f();  // 单次分配：协程帧本身
  }
  ```
- **预期**：**3-4 allocs/task**（协程帧替代 promise/future/task 结构）。
- **限制**：
  - 需要 C++20 编译器
  - 协程帧本身仍有一次堆分配
  - 仅适用于 CPU 任务（不支持跨线程调度）

**验收线（阶段三）**：
- [ ] `benchmark_submit_allocations`：**3-4 allocs/task** (-56-67% vs v0.7.0)
- [ ] 协程版本作为 opt-in 特性（`KAIRO_ENABLE_CORO_TASK`）
- [ ] 性能不弱于 `submit()`
- [ ] 文档说明使用场景和限制

### 6.1 0.7 系列总结

- 最终性能报告与基准记录
- 完整的迁移指南（0.6 → 0.7 → 0.8）
- 用户反馈收集与下一版本规划

---

## 7. 风险与待决事项

| 风险 / 决策点 | 建议 |
|---|---|
| 自适应引入不可复现的行为，用户难以排障 | 默认关闭；提供 "冻结快照" 模式，把聚合快照固定下来以便复现；决策全量结构化记录 |
| 自适应与 ThreadPoolResizer 形成两个控制回路，互相打架 | 0.7.x 的自适应不调整线程数，只做路由、admission 和优先级 |
| 反馈在 worker 线程同步执行，用户自定义调度器可能阻塞 worker | 保留 0.6.1 契约；文档和 debug 构建中加入耗时断言 |
| 0.7.0 是否允许破坏性变更 | 建议**不破坏**：0.6.0 刚完成清理窗口，所有新能力都可以 additive 落地 |
| executor.cpp 拆分与新特性并行开发会冲突 | 拆分作为 0.7.0 的第一个 PR 单独合入，之后再开特性分支 |
| submit_auto 自动推断可能导致意外行为变化 | **仅在未显式设置 intent 时生效**；提供 `disable_auto_inference()` 完全关闭；推断结果记录到诊断位可复原 |
| 能力快照缓存可能导致过时信息 | TTL 设置保守（100ms）；后端状态变化（stop/start）主动失效缓存；TOCTOU 窗口仍由 3.1 ResourceLease 彻底闭合 |
| 任务特征推断的准确率依赖历史数据 | 冷启动期使用保守启发式；提供 min_history_samples 阈值（默认 32）；误判记录到 SchedulingMetrics 可观测 |
| submit_fast() 绕过路由可能被滥用 | 仅推荐给确定的 CPU-only 无约束场景；文档明确说明使用边界；不作为默认推荐 API |
| Task 对象池未正确重置状态导致泄漏 | 严格的重置协议；ASAN 持续验证；单元测试覆盖池行为 |
| InlineFunction 与 std::function 行为差异 | 限制在 trivial-destructor 闭包；回退机制；充分测试 |
| 自定义 TaskResult 破坏 std::future 语义 | 新 API（submit_v2）；旧 API 保持兼容；逐步迁移策略 |
| 堆分配优化引入性能回归 | 每阶段配对 A/B 基准；验收线：不弱于前一阶段 ±5% |
| **submit_priority 移除是 0.7.x 唯一破坏性变更** | **0.7.2 明确标注为例外**；完整迁移指南 + 自动化脚本；deprecated 警告周期（rc1→rc2）；0.7.0 承诺的"破坏性只放在本版"被 0.7.2 打破——但该重构正交于调度模型演进，延后至 0.8.0 会让 0.7.x 的调度语义内部不一致，造成更大的迁移负担 |

## 7. 性能与可观测性目标（0.7.1 新增）

### 7.1 submit_auto 性能改进目标

| 路径 | 当前（0.7.0） | 目标（0.7.1） | 改进 |
|------|--------------|--------------|------|
| CPU-only 纯策略 | ~50ns | 保留 `submit`（评审后 submit_fast 移除） | - |
| CPU/GPU 选择（冷启动） | ~120ns | ~120ns（首次无改进） | - |
| CPU/GPU 选择（缓存命中） | ~120ns | ~60ns（缓存读取） | 50% |
| 自适应路由（AdaptiveScheduler） | ~150ns | ~80ns（缓存优化） | 47% |
| 多 lockfree executor 选择 | N/A（手动指定） | ~100ns（自动选择） | 新能力 |

### 7.2 可观测性增强

**新增诊断位**（`RoutingDiagnostics` 扩展）：
- `AutoInferredIntent` (0x20)：意图由自动推断产生
- `AutoExecutorSelection` (0x40)：执行器由历史负载自动选择
- `CapabilityCacheHit` (0x80)：使用了缓存的能力快照

**新增指标**（`SchedulingMetrics` 扩展）：
- `auto_inferred_count`：自动推断意图的次数
- `capability_cache_hit_count`：能力缓存命中次数
- `capability_cache_miss_count`：能力缓存未命中次数
- `auto_inference_rejected_count`：推断结果被用户显式设置覆盖的次数

**RoutingDecision 增强**：
```cpp
struct RoutingDecision {
    // ... 现有字段
    
    // 0.7.1 新增
    bool capability_from_cache = false;      // 能力来自缓存而非实时采集
    std::string inferred_reason;             // 自动推断的原因（如 "blocking_io_pattern"）
    std::optional<ExecutionIntent> original_intent;  // 推断前的原始意图
};
```

### 7.3 文档与迁移准备

**文档更新清单**（随 3.0.x 各阶段落地）：
- [x] `docs/API.md` §3.7：submit_fast 经评审移除，未新增（submit 承载）
- [x] `website/{zh,en}/guides/choosing-submit-api.md`：评审结论（submit_fast 移除）已同步
- [ ] `website/{zh,en}/guides/adaptive-scheduling.md`：补充自动推断章节，说明 opt-in 机制
- [ ] `docs/design/scheduling_runtime.md` §9：追加能力缓存设计语义
- [ ] `docs/skill/kairo-integration/references/scheduling.md`：同步新 API 和诊断位
- [ ] `MIGRATION.md` 0.7.1 节：确认无破坏性变更，记录新增 opt-in 行为

**基准与验证**：
- 配对 A/B 基准矩阵：submit vs submit_auto（纯策略/缓存/自适应）
- 合成负载矩阵：CPU-only / CPU+GPU / 多 lockfree / 阻塞 I/O 检测准确率
- 长时间 soak：自动推断开启，混合负载 ≥ 24h，监控误判率和震荡
