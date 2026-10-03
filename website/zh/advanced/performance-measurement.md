---
title: 性能测量与回归检查
description: 用统一实验协议分别测量吞吐、尾延迟、jitter 和正确性，并按环境稳定性选择 PR、夜间与发布门禁。
---

# 性能测量与回归检查

## 先写问题，再运行 benchmark

性能测试必须回答一个具体用户问题，例如：

- 目标机器每秒能完成多少个指定大小的解析任务？
- 99% 的请求能否在 5ms 内从提交走到业务结果？
- 1ms 控制周期在目标权限和负载下的 p99 jitter 是多少？
- 持续过载时是否按协议拒绝，并且不会丢失已接受任务？

“Kairo 快不快”无法形成实验。至少固定工作负载、并发模型、输入规模、队列策略、目标硬件和通过条件，再选择指标。

## 五类结果不能互相替代

| 维度 | 回答的问题 | 推荐结果 | 不能证明 |
| --- | --- | --- | --- |
| 正确性 | 接受、执行、失败、拒绝和关闭是否符合契约 | 完成数、异常数、拒绝数、数据校验、TSAN/测试结果 | 延迟达标 |
| 吞吐 | 稳定窗口内完成多少有效工作 | completed operations/s、有效 bytes/s | 单个请求及时完成 |
| 尾延迟 | 大多数与最慢一部分请求等待多久 | p50、p95、p99、max、样本数 | 周期唤醒稳定 |
| jitter | 实际时间点偏离期望时间点多少 | 偏差的 p50/p95/p99/max、正负方向 | 任务本身执行够快 |
| 资源与背压 | 达到结果付出了什么代价，过载如何表现 | CPU、内存、队列深度、drop/timeout 比例 | 业务结果正确 |

吞吐必须以**有效完成**为分子。若一百万次 push 中九十万次失败，只报告一百万次尝试的速度没有业务意义。尾延迟报告也必须同时给出超时和丢弃数；没有完成的请求不能从样本中悄悄消失。

## 正确性先于性能

性能变快前先证明测试没有少做工作。每次实验至少断言：

```text
attempted = accepted + rejected
accepted  = completed + failed + still_pending
```

关闭后 `still_pending` 应按测试协议归零或明确计入放弃结果。通信覆盖策略还要单独记录 overwritten；实时入口的 `dropped_task_count` 与细分拒绝计数存在包含关系，不能重复相加。

以下检查不应因为 benchmark “太慢”而省略：

- 用单元/集成测试验证返回值、异常、依赖和关闭语义；
- 用 TSAN 或并发压力测试发现数据竞争和生命周期错误；
- 对结果数据做 checksum 或业务不变量校验；
- 在满队列、handler 异常和停止竞争下核对计数；
- 确认编译器没有把空工作负载完全优化掉。

正确性失败时不比较性能数字。错误实现的高吞吐没有发布价值。

## 统一实验协议

### 1. 固定环境

每次结果记录：

```text
commit / dirty state:
OS / kernel / architecture:
CPU model / sockets / physical and logical cores:
allowed CPU set / affinity:
memory:
compiler and version:
CMake version:
build type and relevant flags:
Kairo configuration:
power governor / virtualization / container limits:
realtime permissions and applied status:
GPU backend / driver / device, if used:
```

Release 与 Debug、裸机与共享 CI runner、不同 CPU SKU 的绝对值不可直接合并成一条趋势。容器内还应记录实际 cpuset，而不只记录宿主机型号。

### 2. 固定负载

记录任务体、输入大小、producer/consumer 数、线程数、队列容量、批量大小、周期、单周期预算、drop policy 和运行时长。比较实现时除待测变量外保持其他条件一致。

空 lambda 适合测调度开销，不代表真实业务。至少补一组接近生产计算量、内存访问和 I/O 边界的工作负载。

### 3. 预热与重复

先运行不计入结果的预热，使线程、内存页和动态库进入稳定状态。正式实验至少多次重复，保留每次原始结果；默认比较重复结果的中位数，同时报告最差一次和离散程度。

不要只重复单个 task 然后把样本当成独立实验。一次运行内的请求共享调度和系统噪声；运行间差异同样需要保留。

### 4. 隔离噪声

在专用 runner 上限制后台任务、固定 CPU 集合和功耗策略；实时实验同时记录调优字段是否真正应用。共享 CI 上出现的 p99 尖峰先作为调查信号，不要立即写成跨硬件承诺。

### 5. 保存原始输出

优先保存 JSON、命令、stdout/stderr 和环境记录。报告中的表格是整理后的结果，不应作为唯一依据。原始数据使后续能够重新计算百分位、检查样本数和解释检查规则的变化。

## 仓库现有 benchmark 如何使用

| 用户问题 | 现有入口 | 主要输出 | 适合的用途 |
| --- | --- | --- | --- |
| 普通 Facade 提交与完成能力 | `benchmark_baseline` | 提交吞吐、future 等待分布、端到端吞吐，支持 JSON | 固定 runner 上的版本对比和配置扫描 |
| 延迟/软周期定时精度 | `benchmark_timer_precision` | delayed/periodic jitter p50/p95/p99 | 后台定时路径的环境基线 |
| 专用实时线程周期精度 | `benchmark_realtime_precision` | 多周期 realtime jitter p50/p95/p99 | 目标平台和权限下的周期验收 |
| MPSC 竞争与失败率 | `benchmark_lockfree_mpsc`、`benchmark_lockfree_mpsc_full` | producer 并发下的吞吐、延迟、失败率 | 内部队列优化研究 |
| 单消费者任务聚合 | `benchmark_lockfree_task_executor` | 提交/执行吞吐和延迟 | 评估高级执行器是否适合特定场景 |
| 批量策略 | `benchmark_batch_*` | 不同批量和 producer 模型的吞吐 | 在固定任务体下比较批量参数 |

现有 `docs/performance/` 报告记录了历史实验，适合了解优化背景，不是当前机器的基线，也不是公开 API 的性能保证。

### `benchmark_baseline` 的测量边界

它的 submission throughput 只覆盖提交循环；e2e throughput 覆盖提交、任务执行和等待全部完成。当前名为 `round_trip_latency` 的部分先批量提交全部任务，再对每个 future 记录 `get()` 的等待时间，因此它回答“调用 get 时还要等多久”，不等于每项从 submit 到完成的完整延迟。

若业务 SLO 是请求端到端延迟，应在每项提交前记录时间，在任务结果被业务 owner 消费时结束计时，并为每项保存样本。不要把提交吞吐的倒数称为平均请求延迟。

### jitter 的测量边界

timer/realtime precision benchmark 计算实际触发时间相对期望时间点的偏差。它们默认周期和样本量适合快速基线，不足以证明长时间实时保证。正式验收应增加周期数、在目标负载下运行，并同时记录：

- `cycle_timeout_count` 和 drop；
- `priority_applied`、`cpu_affinity_applied` 等平台状态；
- callback/handler 的真实工作量；
- 系统级中断、CPU steal 或调度噪声。

## 可复制的本地基线

先构建三个通用入口：

```bash
cmake -S . -B build-perf \
  -DCMAKE_BUILD_TYPE=Release \
  -DKAIRO_BUILD_TESTS=ON \
  -DKAIRO_BUILD_EXAMPLES=OFF \
  -DKAIRO_ENABLE_GPU=OFF
cmake --build build-perf -j --target \
  benchmark_baseline \
  benchmark_timer_precision \
  benchmark_realtime_precision
```

运行并保留 JSON：

```bash
./build-perf/tests/benchmark_baseline \
  --json --tasks 50000 --min-threads 4 --max-threads 8 \
  --queue-capacity 10000

./build-perf/tests/benchmark_timer_precision \
  --json --tasks-per-period 200 --cycles-per-period 200

./build-perf/tests/benchmark_realtime_precision \
  --json --cycles-per-period 1000
```

Windows 多配置生成器使用 `cmake --build build-perf --config Release`，可执行文件通常位于配置子目录；CTest 命令补充 `-C Release`。实时数据必须在最终部署账号和权限下重测。

也可以检查 benchmark 是否注册：

```bash
ctest --test-dir build-perf -N -L benchmark
```

直接 `ctest -L benchmark` 适合验证 benchmark 能运行，但无法统一传入每个可执行文件的实验参数；正式结果仍应保存明确命令。

## 指标计算约定

### 吞吐

```text
throughput = completed_valid_work / measured_seconds
```

明确 measured window 是否包含初始化、预热、提交、排空和关闭。submission throughput 与 end-to-end throughput 必须分开命名。

### 百分位延迟

对同一语义的单项样本排序后计算 p50/p95/p99，同时报告样本数、超时数和最大值。比较前确认两边使用相同 percentile 算法；小样本的 p99 可能只代表一两个观测值。

### jitter

```text
jitter = actual_timestamp - expected_timestamp
```

保留符号可区分提前与延后；如果业务只关心绝对偏差，另算 absolute jitter，不要悄悄改变定义。周期测试还要说明 expected 是相对首次周期、上一次实际周期，还是绝对 schedule。

### 回归比例

对于“越大越好”的吞吐：

```text
regression = (baseline - candidate) / baseline
```

对于“越小越好”的延迟或 jitter：

```text
regression = (candidate - baseline) / baseline
```

baseline 必须来自相同 runner、相同配置和可追溯 commit。只在变化超过历史噪声带时失败；阈值应由稳定运行的分布校准，不应凭直觉写成固定 5%。

## 三层回归门禁

### PR：正确性硬门禁

普通 PR 稳定执行：

- 单元、集成、并发和教程 smoke tests；
- 必要的 TSAN/ASAN 专项；
- benchmark 编译和最小样本运行；
- 完成数、拒绝数、异常与关闭不变量。

共享 runner 的绝对吞吐和 p99 通常只记录、不阻塞。只有测试本身包含跨 runner 仍稳定的宽松安全界限时，才作为硬门禁。

### 夜间或专用 runner：趋势门禁

固定硬件、镜像和 CPU 策略，重复运行基准并保存时间序列。候选结果连续多个运行越过历史噪声带时告警；单次尖峰先重跑并检查宿主噪声。吞吐、尾延迟、drop 和 CPU 必须一起比较，避免用更多资源换取表面改善。

### 发布：用户 SLO 门禁

在目标 SKU、权限、驱动和近生产负载下验证绝对门槛：端到端延迟、jitter、持续吞吐、过载行为、资源上限和关闭预算。目标环境不一致时，夜间 runner 的相对趋势不能替代发布验收。

## 结果记录模板

每次有结论的实验复制以下结构：

```markdown
## 问题与结论

- 用户问题：
- 结论：
- 适用边界：

## 环境

- commit / dirty state：
- OS / CPU / allowed cpuset / memory：
- compiler / CMake / Release flags：
- permissions / GPU：

## 工作负载

- task body / input size：
- producers / consumers / threads：
- queue / batch / period / drop policy：
- warm-up / duration / repetitions / samples：

## 正确性

- attempted / accepted / completed / failed / rejected：
- checksum / invariants / sanitizer：

## 性能结果

- throughput：
- p50 / p95 / p99 / max / timeout：
- jitter definition and percentiles：
- CPU / memory / queue / drop：

## 比较与门禁

- baseline commit and raw artifact：
- per-run results and aggregate：
- noise band / threshold / decision：
- follow-up：
```

报告必须链接原始输出和复现命令。只保留最优一次、缺少正确性对账或未说明环境的结果，不应用来修改公开性能声明。

## 下一步

评估无锁高级执行器时回到[无锁与性能实验](/zh/advanced/lockfree-and-performance)；把测得的容量转成运行期告警时阅读[容量评估与告警处理](/zh/realtime-and-communication/capacity-and-alerting)，目标平台核对见[Linux、Windows 与 Android 部署核对](/zh/reliability/platform-deployment)。
