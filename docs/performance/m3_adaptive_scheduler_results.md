# M3 AdaptiveScheduler 基准结果（2026-10-10）

v0.7.0 M3（`docs/design/roadmap_v0.7.md` §2.4）三类自适应决策的
验收基准。环境：14 核 Linux 桌面（有背景负载，噪声显著）；微基准
采用绑核（taskset -c 0-3）+ 配对 ABBA 中位，端到端场景采用 default /
adaptive 交替（ABBA）取中位。工具：`tests/benchmark_adaptive_scheduler`
（验收线内置，任一线超限以非零码退出）。

## 预算先定（roadmap §0 原则 3）

预算在实现前写入基准：AdaptiveScheduler 为 opt-in 注入，DefaultScheduler
路径零行为变化；开启时的每决策成本验收线如下。

## 验收线结果（绑核，修复后复验轮）

| 验收线 | 预算 | 实测（中位，跨轮区间） | 判定 |
|---|---|---|---|
| route() 纯策略路径（Auto intent）配对差 vs DefaultScheduler | ≤ 150ns | 18.8 - 49.1ns | ✅ |
| route() CpuOrGpu 冷路径（无样本，启发式回退）配对差 | ≤ 250ns | 30.5 - 40.0ns | ✅ |
| on_task_completed() 单条转发（内嵌聚合器 record） | ≤ 150ns | 22.5 - 63.5ns | ✅ |
| effective_priority_for()（无提升态，submit 热路径新增虚调用） | ≤ 50ns | 0.7 - 2.4ns | ✅ |

route 纯策略路径的新增成本构成：refresh_if_stale 时间门限（1 次原子读 +
steady 时钟读）+ 评估门限/降载掩码两次原子读；评估本体（互斥锁内窗口
差分 + 状态机）每 merge_interval（默认 100ms）至多一次，不占热路径。

## 降载场景（端到端，绑核 ABBA ×2 轮取中位）

场景：2 worker 池；两条 BestEffort 生产线程以 12ms 自旋任务持续填充
（收到 `LoadShedding` 结构化拒绝后按 150ms 退避——结构化拒绝的设计
契约）；Interactive 探针（1ms 自旋）以固定到达率（~3.2ms）提交，
端到端计时；学习期（前 200ms，状态机识别负载并开闸）不计入统计。
adaptive 配置：Interactive/Standard 目标 5ms、breach 2 窗、窗口样本
门槛 4、merge_interval 25ms、提升关闭（隔离降载决策）。

| 指标 | DefaultScheduler | AdaptiveScheduler |
|---|---|---|
| Interactive 探针端到端 p99 | 10.82 - 16.87ms | **1.29 - 4.16ms**（4 轮） |
| 结构化拒绝（LoadShedding） | 0 | 160 - 168 次/两轮 |
| 开闸态复核（load_shed_active） | — | 采样结束时不闸（close_windows 足额大） |

结论：结构化拒绝 + 调用方退避使高 QoS 探针的 p99 改善 ~4-7×（1.3-4.2ms vs
10.8-16.9ms）。DefaultScheduler 下探针（HIGH 排队优先级）仍须等待 worker 上
在途的 BestEffort 长任务腾出；开闸后入流被提前拦截，池内只剩高 QoS 工作。
开关循环的状态机语义（滞回、fail-open、类边界）由
`tests/test_adaptive_scheduler.cpp` 在受控快照下验证，不在本场景内。

## 默认路径无回归（配对 A/B vs M2 head aab439e）

git worktree 基线独立构建，taskset 绑核 ABBA ×4 轮取中位：

| 用例（benchmark_scheduling_paths） | 基线 ns | 本分支 ns | 配对差 | 线 ±5% |
|---|---|---|---|---|
| submit_auto bare builder | 2136.9 | 2113.0 | −1.70% | ✅ |
| submit_auto full spec | 2618.9 | 2577.1 | −1.59% | ✅ |
| submit_auto with feedback wrapper | 2222.1 | 2263.0 | +0.97% | ✅ |
| DefaultScheduler route policy-only | 35.5 | 35.1 | −0.44% | ✅ |

submit_auto(TaskBuilder) 新增的一次 `effective_priority_for` 虚调用
（默认实现原样返回）在 ±5% 线内不可分辨。

## 验证纪律

- 行为测试 44 用例由 Independent-Verification-Agent 独立编写与执行
  （含合成负载下 CPU/GPU 收敛/防翻转的显式阈值、降载状态机滞回与
  fail-open 活性、提升有界性、D1-D3 缺陷回归守卫、Executor 集成与
  0.6.1 契约回归）；首轮验证报告 3 个产品缺陷（聚合器配置未接线、
  累计直方图阻塞 fail-open、RequireRequestedBackend 翻转），修复后
  复验因验证代理配额耗尽改由主循环执行——代理在配额耗尽前已补齐
  三个修复的契约用例（MergeIntervalZeroAdvancesMergeCount /
  TrafficStopsMustCloseValveFailOpen / RequireRequestedBackendNeverFlipsToCpu
  等 5 个）。
- 全量回归 176/176（已知环境 flake：`benchmark_lockfree_task_executor` /
  `test_lockfree_worker_queue` 仅并行压测下偶发，单跑通过）；
- TSAN 白名单（adaptive / feedback_aggregator / contract_v061 /
  pipeline / executor_snapshot）0 failure 0 warning（`setarch -R`）。
