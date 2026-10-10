---
title: 何时使用 AdaptiveScheduler
description: 0.7.0 开发快照能力：反馈驱动的 CPU/GPU 选择、QoS 降载与优先级提升的适用条件，以及震荡、冷启动与不可复现性三类风险的边界。
---

# 何时使用 AdaptiveScheduler

> **0.7.0 开发快照能力（未发布）**：本文描述 `master` 上已存在、尚未包含在任何稳定 tag 中的行为。稳定基线 v0.6.1 没有任何自适应调度——不注入 `AdaptiveScheduler` 时，本文所述内容全部不适用，`DefaultScheduler` 与 0.6.1 逐项一致。

`AdaptiveScheduler`（`include/kairo/adaptive_scheduler.hpp`）是经 `set_scheduler()` 注入的 `IScheduler` 实现：先复用 0.6.1 基线路由产出决策，再叠加三类可解释的反馈决策。选择注入即选择开启；每个决策都带结构化解释通道。

```cpp
#include <kairo/adaptive_scheduler.hpp>

kairo::AdaptiveSchedulerConfig config;
// 各 QoS 类的 queue wait p99 目标（按 BestEffort/Standard/Interactive/Critical
// 枚举序；0 = 该类不评估）。其余参数先用默认值。
config.queue_wait_p99_target_ns = {0, 100'000'000, 10'000'000, 2'000'000};
// 必须在首次提交前注入（与所有自定义调度器相同）。
executor.set_scheduler(
    std::make_unique<kairo::AdaptiveScheduler>(std::move(config)));
```

## 三类决策

| 决策 | 触发条件 | 如何解释 |
| --- | --- | --- |
| CPU/GPU 历史选择 | `CpuOrGpu` 意图；CPU/GPU 两侧样本都达到 `min_samples`（默认 8）后，按端到端时延 EWMA（排队 + 执行）对比 | 决策 `reason = AdaptiveHistory` + 诊断位，`detail` 携带两侧数值；样本不足时回落 0.6.1 启发式，决策与基线逐字段一致 |
| QoS 感知降载 | 某类 queue wait p99 连续 `shed_breach_windows`（默认 3）个合并窗口超过目标 | 对**严格低于**超阈类的提交返回 `reason = LoadShedding` 结构化拒绝；超阈类自身与更高类不受影响 |
| QoS → priority 有界提升 | 同一类连续 `promotion_windows`（默认 6）个窗口超阈 | 该类默认排队优先级 +1 级（封顶 CRITICAL）；用户显式 `priority()` 永不被覆盖 |

默认路径的额外开销有验收线约束（route 纯策略配对差 ≤150ns，实测 19-49ns；完整数据见 `docs/performance/m3_adaptive_scheduler_results.md`）。派生状态每 `merge_interval`（默认 100ms）至多重评估一次，提交侧读路径只付原子读。

## 何时该用

- **负载有持续差异且有足够寿命**：CPU/GPU 选择需要两侧各 ≥8 个样本才开始学习，降载需要连续 3 个窗口（默认下约 300ms）确认超阈。秒级以上存续、形态稳定的负载才能摊薄学习成本。
- **过载模式是"低价值流量挤占高价值流量"**：降载不是扩容——它让低 QoS 提交方尽早拿到 `LoadShedding` 拒绝并自行退避，把容量让给高 QoS。前提是你的调用方**能处理结构化拒绝**（退避、降级或转移），否则拒绝只是提前失败。
- **QoS 等级在你的业务里真实分层**：三类 QoS 决策全部按 `BestEffort < Standard < Interactive < Critical` 的类边界工作。如果你只用单一 QoS，降载与提升没有作用对象。
- **需要"关闭即回到今天"的退出通道**：`set_scheduler(nullptr)` 恢复 `DefaultScheduler`，行为与 0.6.1 一致；所有计数与决策通道都是 additive 的。

## 何时不该用

- **短命或突发进程**：学习期（冷启动）内 CPU/GPU 决策就是启发式本身，进程在学到任何东西之前就结束了。
- **确定性优先于平均收益的测试与回放**：自适应决策依赖样本到达时序，见下文"不可复现性"。
- **调用方无法响应拒绝**：降载会主动拒绝低 QoS 提交；没有退避逻辑的调用方会把它当成新的失败模式。
- **期望它做容量管理**：明确不做 NUMA、跨 pool 迁移、基于利用率的线程数调整（与 `ThreadPoolResizer` 形成双控制回路）。池确实太小时应扩容或降载调用方，而不是等调度器学习。

## 三类固有风险

### 冷启动

样本不足时一切回落基线：这是特性而不是缺陷，但意味着**启动后的前几百个任务不会受益**。不要在冷启动阶段做 A/B 结论；`benchmark_adaptive_scheduler` 的降载场景同样排除了前 200ms 学习期。缩短学习期的正确方式是降低 `min_samples` / `merge_interval`（付出噪声代价），而不是加"热身期特殊逻辑"。

### 震荡

CPU/GPU 切换要求挑战侧 EWMA 低于在用侧 `(1 - hysteresis_margin)`（默认 25%）；降载开闸与关闸分别需要连续 3/5 个窗口，且关闸阈值是目标的 50%（`recovery_ratio`）。这些滞回把"交替负载零翻转"写进了测试（`tests/test_adaptive_scheduler.cpp`），但不能证明你的负载不会在更长时间尺度上摇摆——上线后在 `get_scheduling_metrics()` 的 `adaptive_history_count` 上设告警，突增说明策略在频繁改主意。

### 不可复现性

同一程序两次运行，决策序列可以不同：样本到达时序、worker 调度、外部负载都进入反馈。排障通道因此是结构化的：单条决策看 `RoutingDecision`（reason / diagnostics / detail），趋势看 `SchedulingMetrics` 三个新计数，状态机看 `load_shed_active()` / `priority_promoted()` / `feedback_snapshot()` / `format_state_text()`。需要复现 bug 时，先怀疑自适应层：用 `DefaultScheduler` 复跑一遍，若现象消失再冻结聚合快照定位。

## 学习粒度与边界

- 学习键是 `(backend, executor_name, qos)`：同 QoS 内异构任务的时延混在同一键下。MVP 不做按任务类型细分；同 QoS 混合轻重负载时，历史反映的是混合均值。
- `FallbackPolicy::RequireRequestedBackend` 的请求不做历史翻转，显式 `priority()` 不被提升覆盖——用户显式约束永远优先于历史。
- 降载与 `max_in_flight_tasks` 共存：硬上界是最后的总量阀门，降载是更早、更有区分度的那道。
- 内嵌聚合器与 `Executor::get_feedback_snapshot()` 的诊断聚合器相互独立；诊断入口见[监控与采样](/zh/reliability/monitoring)与 `get_snapshot_text()` 的 `scheduling_feedback.*` 段。

## 下一步阅读

调度模型基础见[声明任务的期限、优先级与资源](/zh/tutorial/scheduling-runtime)；CPU/GPU 提交与降级路径见[CPU/GPU 自动选择](/zh/gpu/automatic-scheduling)；设计语义与验收数据见 `docs/design/scheduling_runtime.md` §8 与 `docs/performance/m3_adaptive_scheduler_results.md`。
