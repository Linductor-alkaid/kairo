---
title: 按症状排查运行故障
description: 从任务不执行、排队变长、等待超时、关闭卡住、实时丢任务和 GPU 不可用出发，按固定顺序取得状态、定位责任并验证恢复。
---

# 按症状排查运行故障

## 先保留事实，再修改配置

发生故障时，不要先增加线程数、扩大队列或改成更高优先级。先记录同一时刻的生命周期、工作量和失败信息；否则修改配置后，最有价值的现场也会消失。

优先采集完整 Kairo 现场：

```cpp
const auto snapshot = executor.get_snapshot();
```

记录 `snapshot.lifecycle`、`partial`、`consistency_note`、completion/async 状态、具名 realtime/Blocking I/O/GPU 状态、聚合计数、失败状态和最近事件。`CompletionStatus` 适合判断默认异步执行器中“还有多少已接受工作没有结束”，后端状态用于判断运行和积压，失败状态与最近事件负责解释类别和上下文。`partial=true` 时应保留这一事实，不能把缺失数据当作零值。单项任务的确定结果仍以对应 `future` 为准。

生产服务还应一起记录版本、配置摘要、进程启动时间、最近一次部署、输入速率和下游依赖状态。单个快照只能说明当下；告警应比较一段时间内的增量和趋势。

## 症状一：任务提交了，但没有执行

### 立即检查

1. 保留并等待该次提交返回的 `future`，用有界 `wait_for()` 区分“尚未完成”和“已经以异常结束”。
2. 检查 `CompletionStatus::is_initialized` 与 `is_running`。
3. 检查 `submit_rejected_count` 是否增长，并读取最近的 `SubmitRejected` 事件。
4. 如果使用任务依赖，确认所有 `TaskHandle` 有效、来自同一个 Kairo，且前置任务本身能够结束。
5. 如果使用周期任务，改查 `get_periodic_task_status()` 的 `is_running`、`execution_count`、`failed_count` 和 `last_error_message`。

### 如何判读

| 观察结果 | 更可能的原因 | 下一步 |
| --- | --- | --- |
| `is_initialized=false` | 尚未初始化，或首次提交路径没有成功建立默认执行器 | 在第一次提交前调用 `initialize()` 并检查 `error_code`、`message`。 |
| `is_running=false` | 已关闭，或初始化失败 | 不要复用已 shutdown 的实例；重建拥有独立 Kairo 的业务组件。 |
| 拒绝计数增长 | 空任务、停止后提交或入口不可用 | 从最近失败事件定位调用点；让请求层返回明确失败。 |
| `queued_tasks>0` 且 `active_tasks` 长期不变 | worker 被阻塞，或任务图等待无法满足的前置 | 检查正在运行任务的 I/O、锁和依赖所有权。 |
| future 已就绪但 `get()` 抛异常 | 任务执行过，并非“没有执行” | 按业务异常处理，不要通过重复提交掩盖根因。 |

不要丢弃 future 后用“没有日志”推断任务未执行。任务可能已失败、排队超时，或日志本身没有刷新。

## 症状二：排队时间持续变长

### 立即检查

连续采样 `AsyncExecutorStatus::queue_size`、`active_tasks`、`completed_tasks` 和 `avg_task_time_ms`，同时记录入口提交速率。至少比较三个窗口，而不是只看一次峰值。

```text
输入速率 > 可持续完成速率
        ↓
queue_size 持续增长
        ↓
端到端等待和软超时增加
        ↓
扩大队列只会推迟拒绝，并增加陈旧工作
```

### 如何判读

- `queue_size` 上升、`active_tasks` 接近线程数、CPU 很高：工作量超过计算容量，或任务粒度过小导致调度开销显著。
- `queue_size` 上升、`active_tasks` 接近线程数、CPU 不高：任务很可能在等待锁、网络、文件或设备 I/O。
- `queue_size` 周期性尖峰后能回落：可能是可接受突发；仍需验证最大端到端延迟和队列内任务是否会过期。
- 高优先级任务正常、普通任务长期不前进：检查持续高优先级流量造成的饥饿，不要把更多任务改为 `CRITICAL`。

### 恢复动作

先限制入口、合并细粒度工作、为 I/O 设置超时，并移出永久循环或长期阻塞任务。只有测得单任务成本和目标延迟后，才调整线程数与队列容量。恢复后验证队列能在预定时间内回到基线，拒绝和超时增量停止增长。

## 症状三：等待超时

优先使用 `wait_for_completion(timeout)`，不要只记录一个 `false`；在选择降级策略前同时采集 `get_snapshot()`：

```cpp
const auto result = executor.wait_for_completion(shutdown_budget);
if (!result.completed) {
    const auto snapshot = executor.get_snapshot();
    // 记录 result.message、result.status 和 snapshot，再执行预先定义的降级策略。
}
```

### 根据快照分流

| 超时快照 | 含义 | 处理方向 |
| --- | --- | --- |
| `active_tasks>0`, `queued_tasks=0` | 已开始的任务没有在预算内结束 | 检查业务 deadline、锁、阻塞 I/O 和协作停止点。 |
| `active_tasks>0`, `queued_tasks>0` | 既有长任务，也有积压 | 先停止新输入，再判断是否继续排空或持久化剩余工作。 |
| `active_tasks=0`, `pending_tasks>0` | 仍有尚未结算的工作关系 | 检查依赖链、提交竞争和对应 future。 |
| `is_running=false` | 执行器已停止 | 不应继续等待或提交；检查生命周期顺序。 |

等待超时不会安全终止任意 C++ 函数，也不代表任务副作用已经回滚。调用方必须决定：继续等、放弃响应但允许后台完成、将输入持久化后重试，还是接受快速关闭的数据后果。所有可重试副作用都应具有幂等键。

## 症状四：关闭过程卡住

### 先确认关闭顺序

```mermaid
flowchart TD
    A[停止接收新请求和外部回调] --> B[通知长期循环与阻塞 I/O 停止]
    B --> C[等待业务 producer 退出]
    C --> D[有界等待已接受任务]
    D --> E[根据快照选择<br/>shutdown(true) 或 shutdown(false)]
    E --> F[销毁任务捕获的业务对象]
```

最常见的原因不是 Kairo 自身在“死锁”，而是业务任务永久阻塞、producer 在排空期间继续提交，或任务捕获对象先于任务被析构。

### 现场检查

1. 在调用 `shutdown()` 前执行一次短预算的 `wait_for_completion()` 并记录快照。
2. 确认 HTTP handler、设备 callback、timer 和消息 consumer 已停止产生新任务。
3. 为网络、文件、设备读取和条件变量等待确认业务级超时或唤醒机制。
4. 检查是否在 worker 内等待同一小线程池中的后续任务。
5. 用线程 dump 定位具体阻塞函数；状态快照只能指出活跃任务未结束，不能替代调用栈。

`shutdown(false)` 不是“杀线程”，也不能让不安全的悬空捕获变安全。若业务必须在进程重启后继续，先把未完成输入和阶段写到外部存储。

## 症状五：实时任务丢失或周期不稳

普通异步状态不能解释实时路径。查询对应名称的 `RealtimeExecutorStatus`：

| 字段 | 说明 | 建议动作 |
| --- | --- | --- |
| `is_running` | 专用实时线程是否运行 | 若为 false，检查注册/启动的 `_ex` 结果和生命周期。 |
| `queue_full_count` | 有界队列已满 | 限制生产速率、减少每条工作或重新评估容量。 |
| `pool_exhausted_count` | 预分配任务 wrapper 不足 | 检查在途任务峰值与消费预算。 |
| `rejected_not_running_count` | 启动前或停止后仍在推送 | 修正 producer 与实时线程的启停顺序。 |
| `cycle_timeout_count` | 周期 callback 超出周期预算 | 缩短 callback，移除分配、锁和不可控 I/O。 |
| `peak_queue_size / queue_capacity` | 峰值占用 | 接近 1 表示突发已经吃完余量，即使暂未 drop 也应预警。 |
| `priority_applied` 等调优字段 | 请求的系统调优是否实际生效 | 按平台权限核对；false 不等于线程没运行。 |

看累计值时必须计算时间窗口增量。`dropped_task_count` 是所有实时拒绝的总入口；继续用细分字段判断是未运行、空任务、队列满还是对象池耗尽。紧急停止信号必须有独立安全旁路，不能依赖队列最终被消费。

## 症状六：GPU 不可用或提交失败

按“构建能力 → 运行时 → 设备 → 注册 → 单次提交”的顺序检查：

1. 确认 CMake 已启用目标后端；没有编译 GPU 支持时，不要从驱动层开始排查。
2. 调用 `register_gpu_executor()`，记录 `ExecutorErrorCode` 和完整 `message`。
3. `BackendUnavailable` 通常表示后端未编译、未实现、运行时不可用或没有可用设备；`InvalidConfig` 应先修配置；`StartFailed` 再进入设备和驱动诊断。
4. 注册成功后，读取 `GpuExecutorStatus::is_running`、`queue_size`、`active_kernels`、`failed_kernels`、显存字段和 `last_error_message`。
5. 对每次 `submit_gpu()` 保留 future 并调用 `get()`；状态计数不能替代单次 kernel 异常。

注册失败后停止向同名 GPU executor 提交。若 GPU 不是业务正确性的必要条件，切换到显式 CPU 路径并记录降级；若 GPU 是硬性依赖，让健康检查失败并停止接流量，不要以空结果伪装成功。

## 故障恢复后的验收

“进程重新响应”不代表问题已经解决。至少验证：

- 新请求能得到确定结果，旧请求的副作用没有重复或丢失；
- `queue_size`、等待时间和实时队列占用回到稳定区间；
- 对应失败计数在恢复后不再持续增长；
- 关闭演练能在预算内完成，且停止后提交得到明确拒绝；
- GPU 降级路径返回正确业务结果，而不只是没有崩溃；
- 现场快照、根因、处置和防复发门禁进入故障记录。

## 继续深入

- [失败可观察性](/zh/reliability/failure-observability)：建立 future、callback、累计状态和最近事件的职责边界。
- [有界等待与状态快照](/zh/tutorial/waiting-and-status)：设计等待预算与超时后的业务决策。
- [并发架构反模式](/zh/guides/concurrency-antipatterns)：定位 worker 内等待、永久阻塞和关闭顺序错误。
- [启动专用实时控制循环](/zh/realtime-and-communication/realtime-control)：理解实时状态和平台调优降级。
- [诊断后端并安全降级](/zh/gpu/diagnostics)：验证无设备环境和 CPU 回退。
