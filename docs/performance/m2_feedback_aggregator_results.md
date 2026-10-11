# M2 反馈聚合层基准结果（2026-10-11）

v0.7.0 M2（`docs/design/roadmap_v0.7.md` §2.3）FeedbackAggregator 的
验收基准。环境：14 核 Linux 桌面（有背景负载，噪声显著）；判定采用
绑核（taskset -c 0-3）多轮中位，默认路径零回归采用 ABBA 配对。
以下为缺陷修复（D1–D6）后独立验证代理第二轮复验的终值。

## 验收线（roadmap §2.3）

- 开启聚合时每任务额外开销 ≤ 100ns；
- TSAN 全绿；
- 默认调度器路径零变化（`wants_feedback() == false` 时不流入样本，
  提交/路由热路径 ±5% 内）。

## record 快路径（`benchmark_feedback_aggregator`）

KAIRO_BENCHMARK_TASKS=20000，绑核，2 轮（复验轮）：

| case | 轮 1 | 轮 2 |
|---|---|---|
| record 单键单线程（acceptance 行） | **23.8** | **23.9** |
| record 交替 4 键 | 61.5 | 24.5 |
| record 4 线程同键（合计吞吐折算） | 34.4 | 7.8 |
| snapshot() RCU 读 | 47.1 | 18.3 |
| refresh() 全量重合并（8 键 × 32 分片） | 1505 | 592 ns/次 |

验收线内置在基准里（最后一行 acceptance，超线退出码非 0）：
23.8/23.9 ns/task ≪ 100 ns 预算，两轮均 PASS，**通过**。

## facade 端到端（提交 → worker 反馈 → 聚合转发）

同基准内的配对用例：CountingFeedbackScheduler（只计数，0.6.1 形态）
vs AggregatingFeedbackScheduler（on_task_completed 转发进聚合器，
M3 AdaptiveScheduler 形态），交替各 5 轮取中位。复验轮差值
（聚合 − 只计数）：**−11.2 / +12.5 ns/task**——在调度噪声内不可
分辨，远小于 100ns 预算，**通过**（裸提交约 2.2µs/task，全链路
量级不变）。

## 默认路径零回归（配对 A/B，ABBA × 4 轮，验证代理第一轮采集）

基线 = M1 提交 8edae98 的 git worktree 构建。本分支相对基线中位：

| case | 基线 | 本分支 | 差 |
|---|---|---|---|
| submit_auto bare builder | 2466.3 ns/op | 2476.7 ns/op | **+0.4%** |
| route policy-only | 35.63 ns/op | 35.22 ns/op | **−1.1%** |

均在 ±5% 线内：`report_scheduling_feedback()` 新增的
`feedback_aggregator_.record()` 调用只存在于反馈开启路径，默认路径
无额外成本（成员为普通对象，无热路径读）。缺陷修复（record 热路径
新增一次长度夹取）后 acceptance 中位数与修复前持平（23.8 → 23.9）。

## 备注

- `snapshot()` 读经由 `std::atomic<std::shared_ptr>`（libstdc++ 实现
  带内部锁池 + 引用计数），实测 18–47 ns/op：对 route() 侧低频读足够；
  M3 若在评分阶段高频读快照，应缓存 shared_ptr 副本而非每次加载。
- TSAN：复验轮白名单 5 目标（feedback_aggregator / contract_v061 /
  pipeline / runtime / executor_snapshot）全过且 0 告警；第一轮发现的
  双直方图合并 OOB（D1）经独立 TSAN+ASan 复现程序确认不再复现，
  附带收益：refresh 合并从 ~5–9µs 降到 ~0.6–1.5µs。
- 最终测试面：`test_feedback_aggregator` 29 用例（11 组），全量回归
  174/174。
