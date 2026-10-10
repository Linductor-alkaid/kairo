# M0 前置清债基准结果（2026-10-10）

v0.7.0 M0（`docs/design/roadmap_v0.7.md` §2.1）五项清债的前后基准对比。
环境：14 核 Linux 桌面（有背景负载，噪声显著）；所有回归判定采用
绑核配对（taskset + ABBA 交替）或多轮中位，而非单点对比。

## 基线（master @ 0342abf，清债前）

`benchmark_scheduling_paths --json`（KAIRO_BENCHMARK_TASKS=20000，3 轮中位，ns/op）：

| case | 中位 |
|---|---|
| submit_auto bare builder | 4208 |
| submit_auto full spec | 4572 |
| submit_auto feedback wrapper | 3599 |
| EDF enqueue (mixed deadlines) | 127 |
| EDF dequeue | 119 |
| route policy-only | 44 |
| route affinity+capabilities | 44 |
| get_executor_capabilities | 194 |

`benchmark_thread_pool_hotpath --json`（8 worker，3 轮中位，tasks/s）：
p1=430k / p2=329k / p4=303k / p8=284k / p12=248k / p16=246k。
（本基准 run 间方差大：p1 单点 376k–632k。回归判定以配对 A/B 为准。）

## 各项结果

### CR-071/163 TaskMonitor 分片 + 采样 0 零取锁

验收线（roadmap）：多线程提交吞吐在采样率为 0 时与关闭监控持平 ±5%。

`review_verification/cr071_submit_throughput_gating.cpp`：4 生产者 × 5 万
任务，绑核，ABBA 配对协议（每轮 A,B,B,A 取同轮配对样本）：

| 轮次 | 配对差 (开启门控 − 禁用监控)/禁用 |
|---|---|
| 1（14 配对样本） | −0.42% |
| 2（14 配对样本） | +1.18% |

**通过**（±5% 线内）。参照组：默认全采样提交窗口吞吐低 5-11%，
worker 侧监控成本可见——分片+门控让"关采样"的监控不再收这笔税。

### CR-107 提交路径堆分配（新增 `benchmark_submit_allocations`）

替换全局 operator new 的确定性计数（submit+run 全生命周期，单生产者）：

| case | 清债前 allocs/task | 清债后 | 字节/task |
|---|---|---|---|
| submit void no-args | 10.00 | **9.00** | 816 → 768 |
| submit value-returning | 10.00 | **9.00** | 824 → 776 |
| submit with runtime args | 10.00 | **9.00** | 832 → 784 |
| tracked no-args | 19.13 | 19.13 | 1737 |
| tracked with args | 19.13 | 19.13 | 1756 |

剩余分配为结构性约束（move-only 可调用间接层、双 std::function 接口、
Task 含 atomic、快照字符串公开契约），记录在案留待接口层调整。

### CR-024 可选 aging（默认关闭）

关闭路径唯一新增成本是 dequeue 入口一次 atomic acquire load：
EDF dequeue 配对偏差 −5.0% / +3.5%（噪声带内）。
开启路径的语义测试见 `test_priority_scheduler_aging`（8 用例）。

### CR-052 依赖图增量拓扑序

正序链建链（评审复现形态，`test_task_dependency_topo` 规模用例）：

| n | 旧实现 | 新实现 |
|---|---|---|
| 4000 | ~3900ms | <1ms |
| 20000 | 外推 ~97s | **21–36ms** |

closing-edge 环检测、remove/prune 后重建、固定种子随机对拍
（50×200 与 30×240 ops，两套种子）全部与暴力 DFS 参考一致。
逆序建链（插入序与依赖序完全相反的最坏形态）保持 O(n²) 纯内存遍历，
与旧实现同阶；facade 真实形态（父任务先注册）全部走快路径。

### executor.cpp 拆分（纯结构）

117 个 Executor/WorkerHandle 方法定义逐一核验与拆分前一致；
导出符号面一致（nm 对比，差异仅为隐式成员内联排放位置）；
全量 167/167 通过；受控绑核配对 A/B：scheduling_paths 8/8 case
偏差 ≤ ±2.3%，hotpath 6/6 配置 +0.2% ~ +12.3%（方向中性偏快）。

## 终态全量验证（M0 全部合入后）

- `ctest -j 8`：**171/171 通过**（新增 4 个测试文件：gating、aging、
  topo、submit_allocations 基准注册）。
- TSAN（`KAIRO_ENABLE_TSAN=ON`）：task_monitor/gating/sampling/
  statistics/thread_pool 5 个目标零 race 报告。
