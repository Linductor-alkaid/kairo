---
title: 容量评估与告警处理
description: 从通信和实时累计统计计算窗口速率、队列占用、消费余量与周期超限，并把告警连接到明确处置动作。
---

# 容量评估与告警处理

本页面向已选择有界通信或实时队列的专家。普通后台任务无需据此选择后端；先从[执行模型与路由边界](/zh/guides/execution-models-and-routing)确认业务需要的是 completion 还是 admission。`dispatch_auto(RealtimeQueue)` 的 `accepted` 是本次入队结果，不能用它替代本页的周期、drop 和容量监控。

## 先回答三个容量问题

容量配置不是“队列够不够大”一个问题。上线前分别回答：

1. **稳态是否可持续**：消费者长期处理速率是否高于生产速率。
2. **突发是否可吸收**：容量能否覆盖允许的突发时长，并仍满足数据新鲜度。
3. **过载时丢什么**：拒绝、覆盖、超时或错过阶段是否符合业务协议。

任何告警都要绑定数据语义。同一个 `overwritten_count`，对“只要最新控制目标”的 mailbox 可以正常，对审计事件则说明组件选择错误。

## 累计值必须转换成窗口值

`CommStats` 和 `RealtimeExecutorStatus` 中多数计数从组件创建后单调累计。不要对“累计值大于 100”告警；服务运行越久越容易误报。每次采样保存时间戳和上一次快照，用差值计算窗口信号：

```cpp
struct CommWindow {
    double accepted_per_second = 0.0;
    double received_per_second = 0.0;
    double drop_events_per_second = 0.0;
    double depth_ratio = 0.0;
};

CommWindow calculate_window(const kairo::comm::CommStats& before,
                            const kairo::comm::CommStats& after,
                            std::chrono::duration<double> elapsed) {
    const double seconds = elapsed.count();
    const auto delta = [](uint64_t old_value, uint64_t new_value) {
        return new_value >= old_value ? new_value - old_value : 0;
    };

    return {
        delta(before.sent_count, after.sent_count) / seconds,
        delta(before.received_count, after.received_count) / seconds,
        delta(before.dropped_count, after.dropped_count) / seconds,
        after.capacity == 0
            ? 0.0
            : static_cast<double>(after.current_depth) / after.capacity,
    };
}
```

生产代码还应处理 `seconds <= 0`、组件重建和计数回绕。组件实例重建后计数会从零开始；监控标签至少包含组件名、实例或进程启动标识，避免把重启误判为负增量。

### 不要误读 `sent_count`

`sent_count` 是成功进入组件的数据，不一定等于生产者尝试量：

- `RejectNewest` 满队列时，新值被拒绝并增加 `dropped_count`，不会增加 `sent_count`。
- `DropOldest` 满队列时，旧值被丢弃，但新值仍进入队列并增加 `sent_count`。
- `KeepLatest` 会清空旧积压、增加 `overwritten_count`，再接受新值。

因此无法只用 `sent_count + dropped_count` 为所有策略统一还原入口请求数。若业务需要准确的尝试率和拒绝率，在调用 `try_send()`/`send_for()` 的边界另计 attempted、accepted 和 rejected。

## 默认异步提交的总量 admission

通信与实时组件之外，普通 `submit()` / `submit_batch()` 等默认异步提交默认**没有**
总量上限：`queue_capacity` 只构造每 worker 本地队列，本地队列满时任务回退到
scheduler 全局队列，过载表现为无界积压。需要结构化过载拒绝时显式配置
`max_in_flight_tasks`：

```cpp
kairo::ExecutorConfig config;
config.max_in_flight_tasks = 256;   // 已接纳未结算的提交总量
```

- 达到上限时提交不抛出：future 立即以 `CapacityExhaustedException` 就绪，
  并记录 `FailureKind::CapacityExhausted`（`capacity_exhausted_count` 可窗口化
  告警），与 executor 停止、非法输入可区分。
- 每个接纳提交在正常完成、异常、排队取消、执行前超时、拒绝或丢弃时恰好
  释放一次容量；future 就绪时计数已最终化。
- 覆盖普通/优先级/批量/依赖/可取消/串行（`submit_on*`）提交；timer 派发
  （`submit_delayed*`/`submit_periodic*`）与 realtime/GPU 不在范围——后者的
  背压语义见本页后续各节。
- `get_in_flight_submissions()` 是瞬时水位，与 `capacity_exhausted_count` 的
  窗口增量配合使用：水位贴顶 + 拒绝增长 = 持续过载；水位贴顶 + 零拒绝 =
  容量恰好，无回压需求。

告警处置与通信组件一致：先确认是入口过量、消费变慢还是泄漏（在途计数不
随完成下降），再决定限流、扩容或降级；不要用调大 `max_in_flight_tasks`
掩盖消费端停顿。

## MpscChannel：每条消息都要处理

### 计算可持续性

对 FIFO 消息流，在稳定采样窗口中比较：

```text
accepted_rate = Δsent_count / Δt
receive_rate  = Δreceived_count / Δt
net_rate      = accepted_rate - receive_rate
depth_ratio   = current_depth / capacity
drain_time    ≈ current_depth / receive_rate
```

`net_rate > 0` 且连续多个窗口成立，表示积压不可持续。`drain_time` 只在近期消费速率稳定且大于零时有参考价值；消费者阻塞或输入特征改变时，它不是完成保证。

队列容量的初始估算可以写成：

```text
capacity >= 峰值净流入速率 × 允许突发时长 + 安全余量
```

然后还要检查最旧消息等待时间是否小于业务 deadline。容量能容纳十分钟积压，不代表用户愿意处理十分钟前的数据。

### 告警起点

以下是落地监控的**初始模板**，不是库承诺的通用阈值：

| 级别 | 窗口条件 | 处置 |
| --- | --- | --- |
| 提示 | `depth_ratio > 0.5` 持续多个窗口，或 `net_rate > 0` | 检查入口变化、消费者耗时和下游依赖。 |
| 警告 | `depth_ratio > 0.8`，或预计 `drain_time` 接近新鲜度预算 | 限流、扩展消费者或降级非关键输入。 |
| 严重 | 必须不丢的流出现 `Δdropped_count > 0`，或 `Δtimeout_count > 0` 影响请求 | 立即保护入口，保留失败输入并按幂等协议恢复。 |
| 生命周期故障 | 运行期出现 `Δclosed_send_count > 0` | 检查 producer 与 channel 的关闭顺序。 |

阈值最终应由流量分布、消息 deadline 和恢复时间目标校准。若业务允许丢弃，严重级别也应基于丢弃比例和用户影响，而不是照抄“任何 drop”。

## RealtimeChannel：消费能力由周期预算限定

`RealtimeChannel::drain_for_cycle()` 每周期最多消费 `max_items_per_cycle` 条；配置为 `0` 表示不限，但无限消费可能破坏周期确定性。

在忽略 handler 成本的理论上限下：

```text
theoretical_items_per_second
    = max_items_per_cycle × 1,000,000,000 / cycle_period_ns
```

这只是入口筛选值，不是容量承诺。真实能力还受周期 callback、每条 handler 耗时、操作系统调度和同周期其他工作影响。应在目标硬件上测出每周期实际消费分布，并同时满足：

- 输入率低于可持续消费率；
- handler 加 callback 的尾延迟不突破周期预算；
- `current_depth` 在突发后能下降；
- `dropped_count` 和 `handler_exception_count` 不发生不可接受增量。

如果提高 `max_items_per_cycle` 后 drop 下降、但实时线程 `cycle_timeout_count` 上升，系统只是把“数据丢失”换成了“周期失约”。正确动作通常是减少每条工作、在实时线程外预处理、降低输入率，或重新定义哪些数据允许覆盖。

## 实时执行器：将队列压力与周期压力分开

`RealtimeExecutorStatus` 同时包含任务入口背压和周期执行统计。用两个窗口快照计算：

```text
cycle_timeout_ratio = Δcycle_timeout_count / Δcycle_count
drop_rate           = Δdropped_task_count / Δt
queue_full_rate     = Δqueue_full_count / Δt
pool_exhausted_rate = Δpool_exhausted_count / Δt
cycle_load_ema      = avg_cycle_time_ns / cycle_period_ns
max_cycle_load      = max_cycle_time_ns / cycle_period_ns
```

`avg_cycle_time_ns` 是指数移动平均，适合看近期负载趋势；`max_cycle_time_ns` 是实例生命周期最大值，适合保留最坏现场，但恢复后不会自动下降。两者都不是 p99。

### 按根因告警

| 信号 | 含义 | 优先动作 |
| --- | --- | --- |
| `Δrejected_not_running_count > 0` | 启动前、停止后或关闭竞争中仍在推送 | 修正 producer 生命周期，不扩容。 |
| `Δrejected_empty_task_count > 0` | 调用了空任务 | 修复调用方校验，不扩容。 |
| `Δqueue_full_count > 0` | 有界队列没有入口余量 | 降低生产率、减少工作或评估容量。 |
| `Δpool_exhausted_count > 0` | 预分配 wrapper 已耗尽 | 检查在途峰值和消费预算。 |
| `cycle_timeout_ratio` 增长 | 一个或多个周期工作超过 period | 缩短 callback/任务，移除锁、分配和不可控 I/O。 |
| `cycle_load_ema` 长期接近 1 | 稳态余量不足 | 在出现 timeout 前处理，目标余量由 jitter SLO 决定。 |

`dropped_task_count` 是上述所有实时拒绝的总入口，适合统一服务健康信号；细分计数用于路由处置。不要把 `dropped_task_count` 与 `queue_full_count` 相加，它们存在包含关系。

`failed_pushes` 和 `peak_queue_size` 只有启用底层队列统计时才有意义。`peak_queue_size / queue_capacity` 是生命周期峰值占用，不是当前占用；接近 1 说明历史上曾吃完余量，不能证明现在仍拥塞。

### 建议的分级

- **立即严重**：安全关键任务出现任何新增 drop，或 `is_running=false` 但控制 producer 仍活跃。
- **警告**：窗口内 `cycle_timeout_ratio` 超过业务容忍值，或 `cycle_load_ema` 连续逼近预留上限。
- **容量预警**：峰值队列占用进入保留余量，尚未发生 drop；需要结合流量窗口验证是否为一次性启动突发。
- **部署故障**：业务要求的 `priority_applied`、`cpu_affinity_applied` 等字段为 false；这与容量告警分开处理。

不要为所有项目写死同一个百分比。1 kHz 控制循环和 1 Hz 后台采样的故障预算完全不同；告警规则必须注明周期、业务后果和允许连续窗口数。

## LatestMailbox 与 DoubleBuffer：关注新鲜度，不看队列占用

这两类组件表达“最新状态”，容量固定且旧值不需要逐条消费。`current_depth / capacity` 长期为 1 并不表示拥塞。

### LatestMailbox

- `overwritten_count` 增长表示上一个值在下次 publish 前仍存在。若消费者只要最新值，这是设计语义。
- `stale_read_count` 增长表示 `try_load_newer_than()` 没有读到更高 sequence；轮询比发布快时也会自然发生。
- `max_latency` 与 `avg_latency` 是已读取值从发布到读取的年龄，应该与业务最大允许新鲜度比较。
- `consumer_lag` 在成功读取新值时表达跨过的 sequence 数，不是队列长度。

因此建议直接记录“当前值年龄”和 sequence 推进速度。只有当年龄超过 deadline、sequence 停止增长，或跨过版本意味着遗漏业务动作时才告警。

### DoubleBuffer

读者获得的是一致快照，不是事件流。重点观察发布 sequence 是否推进、读者是否持续读旧版本以及读取延迟。若每个中间状态都必须处理，应改用 `MpscChannel`，而不是为覆盖次数设置更激进告警。

## PhaseGate 与 Sequencer：按协议违约告警

阶段和序号组件中的 lag 字段也不是队列深度。`producer_lag` 可表示当前 phase 或已发布 ticket，`consumer_lag` 可表示 waiter 数量；必须按具体组件解释。

推荐规则：

- `Δmissed_phase_count > 0`：调用方请求的阶段或序号已被越过，通常是协议错误，直接告警并保留期望值/实际值。
- `Δtimeout_count > 0`：先按业务 deadline 判断严重性，再检查对应 producer 是否仍健康。
- `Δclosed_send_count > 0`：关闭后仍有发布或等待竞争，检查生命周期。
- waiter 数持续增加且 phase/ticket 不推进：生产方可能卡住；同时告警生产者健康，而不只扩大等待超时。

## 延迟统计的边界

`CommStats::avg_latency` 是组件生命周期内已接收数据的累计平均值，短时恶化可能被大量历史正常样本稀释；`max_latency` 是生命周期最大值，恢复后仍保持高值。它们适合状态快照和现场诊断，但不能替代尾延迟分布。

若延迟是 SLO：

1. 在业务边界记录每条或采样延迟到直方图；
2. 用 p50、p95、p99 和最大值观察分布；
3. 按固定窗口聚合，保留数据量和丢弃量；
4. 将“没有收到数据”作为独立可用性事件，不要从延迟样本中消失。

## 从告警到处置

每条规则至少写清五件事：

| 项目 | 示例 |
| --- | --- |
| 数据语义 | `camera-frames` 允许丢旧帧，只要求最新帧年龄小于 100ms。 |
| 窗口和阈值 | 连续 3 个 30s 窗口的最新值年龄超过 100ms。 |
| 用户影响 | 感知使用陈旧输入，控制结果不再可信。 |
| 自动动作 | 停止发布控制输出，切换安全状态。 |
| 人工排查 | 检查 producer sequence、handler 延迟、实时周期和设备状态。 |

只有数值没有动作的告警只是噪声。扩容也不是统一动作：生命周期错误、空任务、handler 异常和 missed phase 都不能通过扩大队列修复。

## 上线前容量演练

至少执行以下负载阶段，并保存每阶段前后快照：

1. **空闲基线**：确认计数不自行增长，关闭后发送除外。
2. **稳态目标流量**：运行足够长时间，队列深度不持续上升。
3. **短时突发**：验证峰值占用、恢复时间和数据新鲜度。
4. **持续过载**：确认 drop policy、返回值、告警和降级符合协议。
5. **消费者变慢/抛异常**：验证 handler failure 与积压能够区分。
6. **关闭竞争**：停止期间继续制造少量输入，确认拒绝可见且不会卡住。

验收结果应包含配置、输入分布、窗口公式、告警触发时间、自动动作和恢复耗时。只记录“未崩溃”不能证明容量设计有效。

## 下一步

先在[通信可观察性](/zh/realtime-and-communication/observability)确认字段来源；遇到实际 drop、周期超限或关闭异常时使用[按症状排查运行故障](/zh/reliability/troubleshooting)。平台权限与调优状态见[Linux、Windows 与 Android 部署核对](/zh/reliability/platform-deployment)。
