# Executor 完整生命周期 Monitor 演进设计


> [!NOTE]
> 历史设计快照：生命周期监控设计。项目现名 kairo（`kairo::monitor`）。

## 1. 文档状态

- 状态：设计提案
- 范围：`Executor` facade、`ExecutorManager` 及其管理的 CPU、实时、GPU、Blocking I/O 执行器
- 非目标：本设计不把业务 payload、无限任务历史或通信数据内容纳入 Executor 内部存储

## 2. 背景与问题

当前项目已经具备多种局部观测能力：

- `TaskMonitor` 按 `task_type` 聚合成功、失败、超时和执行时间；
- `AsyncExecutorStatus`、`RealtimeExecutorStatus`、`BlockingIoExecutorStatus` 和 `GpuExecutorStatus` 描述各后端状态；
- `ExecutorFailureStatus` 与 `get_recent_failures()` 描述失败累计和有限最近事件；
- `CommStats` 描述通信组件自身的背压、覆盖、陈旧读取和延迟。

这些接口分别有价值，但调用方需要自行拼装结果，且无法直接回答以下问题：

1. Executor 整体处于创建、运行、排空还是已停止阶段？
2. 当前异常来自未初始化、队列积压、后端停止、提交拒绝还是任务执行失败？
3. 关闭或等待超时时，各执行后端和失败路径的完整现场是什么？
4. 一个健康检查或故障支持包如何以稳定格式保存整个执行器状态？

因此项目需要从“多个状态查询接口”演进为“统一的生命周期状态 Monitor”。

## 3. 目标与设计原则

### 3.1 目标

- 提供一次调用即可获得 Executor 的只读状态快照；
- 覆盖 Executor 生命周期、所有已注册后端、任务/队列汇总、失败摘要和任务统计；
- 支持健康检查、故障现场、超时诊断、日志和未来 JSON/Prometheus 适配；
- 不改变现有提交、future、停止和错误语义；
- 不在高频任务、实时周期和无锁队列路径引入阻塞采集；
- 对未初始化、正在停止、后端不可用等情况给出明确状态，而不是用全零值隐含表达。

### 3.2 原则

1. **只读值对象**：调用者获得独立数据，不持有内部执行器指针。
2. **Best-effort 明示**：第一阶段不承诺跨所有锁的事务级一致性，快照必须标明采集时间和一致性状态。
3. **有界内存**：最近事件和在途任务诊断均有容量上限。
4. **分层采集**：生命周期与后端状态由 Monitor 汇总；通信细节仍由 `executor::comm` 管理并可通过适配器接入。
5. **兼容优先**：保留现有状态和统计 API，统一 Monitor 是新增能力，不替换已有接口。

## 4. 术语与生命周期模型

建议新增统一生命周期枚举：

```cpp
enum class ExecutorLifecycleState {
    Created,
    Initializing,
    Running,
    Draining,
    Stopped,
    Failed
};
```

状态语义如下：

| 状态 | 语义 |
|---|---|
| `Created` | 对象已创建，尚未初始化默认异步后端 |
| `Initializing` | 正在创建或启动一个或多个后端 |
| `Running` | 至少一个可用后端运行，允许正常提交 |
| `Draining` | 已请求停止，不再接受新工作，等待已接受工作排空 |
| `Stopped` | 所有由 Manager 拥有的后端已停止 |
| `Failed` | 初始化或关键生命周期操作失败，需结合错误和最近事件诊断 |

生命周期状态是 Monitor 维护的摘要，不取代具体后端的 `is_running`、`stop_requested` 或 `stop_reason`。
状态按以下优先级推导：关键初始化或生命周期操作失败时为 `Failed`；已请求停止且仍有后端运行或工作待排空时为 `Draining`；至少一个后端运行时为 `Running`；全部已停止且曾初始化或完成过停止流程时为 `Stopped`；尚未初始化默认异步后端时为 `Created`。后端创建/启动过程由实现显式标记为 `Initializing`。该摘要不参与提交 reservation，既有 `is_running` 语义不变。

## 5. 统一快照数据模型

建议在 `include/executor/types.hpp` 中增加公共值类型，在 `include/executor/executor.hpp` 中增加：

```cpp
ExecutorSnapshot get_snapshot() const;
```

建议的第一版模型如下（字段可按 ABI 策略采用新头文件版本或尾部追加）：

```cpp
struct ExecutorSnapshot {
    uint32_t schema_version = 2;
    uint64_t snapshot_sequence = 0;
    uint64_t state_epoch = 0;
    std::chrono::steady_clock::time_point captured_at{};
    ExecutorLifecycleState lifecycle = ExecutorLifecycleState::Created;
    bool partial = false;
    std::string consistency_note;

    CompletionStatus completion;
    AsyncExecutorStatus async;
    std::map<std::string, RealtimeExecutorStatus> realtime;
    std::map<std::string, BlockingIoExecutorStatus> blocking_io;
    std::map<std::string, gpu::GpuExecutorStatus> gpu;

    ExecutorFailureStatus failures;
    std::vector<ExecutorFailureEvent> recent_failures;
    std::map<std::string, TaskStatistics> task_statistics;

    size_t running_backend_count = 0;
    size_t stopping_backend_count = 0;
    size_t active_task_count = 0;
    size_t queued_task_count = 0;
    size_t failed_task_count = 0;
    size_t dropped_work_count = 0;
};
```

其中：

- `schema_version` 用于未来兼容；
- `snapshot_sequence` 在同一个 Monitor 实例内单调递增，便于判断是否读到新快照；
- `captured_at` 固定记录采集开始时刻；
- `partial=true` 表示某个后端在采集期间被注销、停止或 provider 不可用；
- 汇总字段是诊断便利字段，不能替代各后端的详细状态；
- `recent_failures` 应复用现有容量限制，不复制无限历史。

### 5.1 在途任务扩展（第二阶段）

第一版只提供聚合统计，避免修改所有任务路径。需要定位卡住任务时，再增加有界诊断表：

```cpp
enum class TaskLifecycleState {
    Pending,
    Queued,
    Running,
    Succeeded,
    Failed,
    TimedOut,
    Rejected,
    Cancelled,
    DependencyBlocked
};

struct TaskLifecycleSnapshot {
    std::string task_id;
    std::string task_type;
    std::string executor_name;
    TaskLifecycleState state = TaskLifecycleState::Pending;
    std::chrono::steady_clock::time_point submitted_at{};
    std::chrono::steady_clock::time_point state_changed_at{};
};
```

当前实现覆盖默认异步线程池和 facade 任务图：任务被线程池接受后记录为 `Queued`，worker
开始执行时转为 `Running`，成功、失败或软超时时立即移除。`TaskHandle` 创建时记录为
`Pending`，依赖未满足时转为 `DependencyBlocked`，依赖失败或完成时立即移除。状态表默认容量为 128，
通过 `set_in_flight_task_capacity()` 配置；容量为 0 时关闭该表但不关闭既有聚合统计。
`set_in_flight_task_sampling_rate()` 与聚合统计采样独立。表满时不驱逐仍在途的条目，
只增加 `in_flight_dropped_count` 并将 snapshot 标为 `partial` / `in_flight_diagnostics_incomplete`。

`ExecutorSnapshot` 同时提供 `in_flight_count`、最老在途任务年龄、按 state 的计数和
有限 `in_flight_tasks`，而不是默认保存全部任务。实时、GPU 和 Blocking I/O 仍由各自
backend 状态描述：realtime 使用 `RealtimeExecutorStatus` 的运行、容量和 drop/rejection
计数，GPU 使用 `GpuExecutorStatus` 的 active/queued/completed/failed kernel 计数，Blocking
I/O 使用 `BlockingIoExecutorStatus` 的 running/ready/stop reason/error。它们不会写入普通
任务表；特别是 realtime cycle thread 不允许为诊断获取 `TaskMonitor` 的互斥锁。

## 6. 组件架构

建议将当前 `StatisticsCollector` 演进为监控基础设施，但不要让它直接拥有 Manager 的生命周期责任：

```text
Executor facade
        |
ExecutorMonitor / SnapshotCollector
        |
ExecutorManager ---- backend status providers
        |             |-- async/thread pool
        |             |-- realtime
        |             |-- blocking I/O
        |             `-- GPU
        |
        |-- TaskMonitor / StatisticsCollector
        |-- FailureState (status + recent events)
        `-- optional communication adapters
```

推荐职责：

- `ExecutorManager`：提供线程安全的后端名称、生命周期和状态 provider；
- `StatisticsCollector`：继续负责任务统计和 GPU provider，必要时增加统一采集辅助方法；
- `ExecutorMonitor`：汇总 provider、计算跨后端摘要、生成 `ExecutorSnapshot`；
- `Executor`：暴露稳定的 `get_snapshot()` facade，并在等待/关闭超时时可选调用导出器；
- `CommStats`：保持组件本地语义，通过显式 adapter 注册到 Monitor，不隐式改变 `ExecutorFailureStatus`。

Monitor 不应在采集时调用会触发懒初始化的接口；应使用现有 snapshot getter，例如 `get_default_async_executor_snapshot()`，避免“查询状态导致创建线程池”。

## 7. 一致性与并发语义

### 7.1 第一阶段：best-effort 快照

各 provider 在自己的同步域内返回值拷贝，Monitor 按固定顺序采集：

1. 记录采集起始时间；
2. 读取 Manager 生命周期和注册表名称；
3. 获取各后端 `shared_ptr` 快照并读取其状态；
4. 读取 failure 状态、最近事件和任务统计；
5. 计算汇总字段并记录 `partial`/`consistency_note`；
6. 递增 `snapshot_sequence` 并返回。

这保证每个子状态自身安全，但不保证所有字段来自同一纳秒。文档和 API 名称必须明确这是诊断快照而非事务读。

### 7.2 第二阶段：epoch 校验

Manager 维护轻量 `state_epoch`，覆盖注册表和 Manager 生命周期边界变化；任务
计数等高频状态变化不递增 epoch。Monitor 采集前后读取 epoch：

```text
读取 epoch N -> 读取全部 provider -> 再读 epoch
若 epoch 变化，重试有限次数；仍变化则 partial=true
```

最多重试两次；若仍发生变化，返回的 `state_epoch` 为最后一次观测值并设置
`partial=true`、`consistency_note` 包含 `epoch_changed`。不通过一把全局锁包住所有
后端状态读取，因为停止、GPU 查询或外部 worker 状态读取可能阻塞，并会放大实时系统抖动。

当前实现将 `state_epoch` 暴露在 `ExecutorSnapshot` 中。它覆盖 Manager 管理的注册表
和生命周期边界，不覆盖高频任务计数；因此 epoch 变化表示快照的结构性观察窗口被打断，
而不是表示每个任务计数都必须重新采集。

### 7.3 生命周期竞态

- provider 被注销后，已取得的 `shared_ptr` 保证对象在读取期间存活；
- stop 可以与 snapshot 并发，状态应反映“正在停止”而不是假设仍可提交；
- snapshot 不阻止 shutdown，也不作为提交 reservation；
- 裸指针 API 的既有并发限制保持不变。

## 8. 采集开销与数据保留

- `get_snapshot()` 是低频诊断 API，不应在每个任务上调用；
- 热路径只维护现有原子计数，复杂 map/vector 仅在采集时复制；
- `recent_failures` 复用 ring buffer 容量；
- 在途任务表必须配置上限，超限时保留计数并设置 `partial` 或 `diagnostic_overflow`；
- 不在默认 snapshot 中包含异常对象序列化、任务 callable、用户输入或通信 payload；
- JSON 序列化应在调用线程或外部导出线程执行，不能在实时周期线程内执行。

## 9. 对外 API 与兼容策略

建议新增：

```cpp
ExecutorSnapshot Executor::get_snapshot() const;
std::string Executor::get_snapshot_text() const;
void Executor::set_snapshot_diagnostic_callback(ExecutorSnapshotCallback callback);
```

`get_snapshot_text()` 输出稳定的行式诊断文本；时间字段带明确单位，枚举使用稳定字符串，且不包含 `exception_ptr`、callable、payload 或通信内容。JSON 仍作为后续可选 API，避免过早把 JSON 库或格式约束引入核心类型。`set_snapshot_diagnostic_callback()` 在等待超时及 facade 初始化、注册、启动失败时，于调用线程交付独立快照；回调异常必须隔离，不能从实时周期或任务热路径调用。已有接口继续保留：

- `get_async_executor_status()`；
- `get_realtime_executor_status()`；
- `get_all_gpu_executor_status()`；
- `get_blocking_io_worker_status()`；
- `get_failure_status()` / `get_recent_failures()`；
- `get_task_statistics()` / `get_all_task_statistics()`。

已有接口适合热路径或单一问题查询，统一 snapshot 适合健康检查、故障现场和外部遥测。

## 10. 分阶段实施计划

### Phase 0：契约与字段盘点

- 固定生命周期枚举、字段命名、时间单位和 `partial` 语义；
- 列出各 backend 的可用/不可用状态；
- 明确 snapshot 不覆盖通信 payload 和业务状态。

### Phase 1：统一 best-effort snapshot（MVP）

- 新增 `ExecutorSnapshot` 和 `Executor::get_snapshot()`；
- 增加 Manager 的全部 realtime、blocking I/O、GPU 状态批量 provider；
- 汇总 completion、failure、task statistics 和 backend counters；
- 添加未初始化、启动失败、并发停止测试。

### Phase 2：故障现场与导出

- 增加 JSON/文本导出；
- 在 `wait_for_completion_ex()` 超时、shutdown 超时和关键启动失败路径支持自动保存或 callback；
- 增加采集耗时、快照序号和字段版本。

### Phase 3：有限在途任务诊断

- 在 submit/queue/start/complete/fail/timeout/reject/cancel 路径接入生命周期记录；
- 增加容量、采样和敏感字段策略；
- 验证监控开关、采样率和异常回调不会改变任务结果。

### Phase 4：按需增加 epoch 一致性和事件流

- 只有在真实故障无法由 best-effort snapshot 定位时实施；
- 事件流采用有界 ring buffer，snapshot 保存最新聚合状态；
- 通过压力、TSAN 和实时预算测试评估额外成本。

## 11. 测试与验收标准

### 功能

- 未初始化 Executor 返回 `Created`，且不会触发懒初始化；
- 初始化成功后返回 `Running`，后端状态与现有单项 API 对账；
- stop 期间返回 `Draining` 或明确的 stopping 子状态；
- shutdown 完成后返回 `Stopped`；
- 后端启动失败进入 `Failed`，失败计数和最近事件可关联；
- async、realtime、GPU、blocking I/O 的数量汇总与逐项状态一致；
- snapshot 序号单调递增，最近事件和统计遵守容量/采样策略。

### 并发与安全

- snapshot 与注册、注销、stop、submit 并发运行无数据竞争、死锁或 use-after-free；
- snapshot 不延长后端停止完成时间；
- 调用 snapshot 不创建默认异步执行器；
- failure callback、通信 callback 抛异常不会破坏 snapshot。

### 性能

- 默认任务热路径无新增互斥锁；
- 在无后端、单后端和多后端场景测量采集耗时与分配次数；
- 实时周期测试确认不在周期线程执行 map 复制或 JSON 序列化；
- 高并发任务下 snapshot 采集不会导致完成等待长期饥饿。

## 12. 风险与未决问题

1. **一致性预期过高**：必须在 API 文档中明确 best-effort；需要事务语义时再引入 epoch。
2. **字段膨胀**：优先稳定摘要，详细诊断通过可选扩展或独立接口提供。
3. **后端语义不统一**：GPU kernel、实时周期和普通任务不能强行映射为同一种 completion 语义，应保留 backend-specific 状态。
4. **敏感信息泄露**：默认不保存任务参数、异常文本以外的 payload 和设备内部数据。
5. **监控反噬性能**：采样、容量上限和外部异步导出必须是默认策略。
6. **ABI 兼容**：公共结构体新增字段需要遵循项目版本策略，必要时通过 `schema_version` 和新 API 过渡。

## 13. 结论

项目已经具备建设统一生命周期 Monitor 的基础：后端状态接口、失败状态、任务统计、Manager 注册表和生命周期安全的 `shared_ptr` snapshot getter 都已存在。最合理的演进路径是先增加一个只读、best-effort 的 `ExecutorSnapshot` 汇总层，把分散能力统一成稳定诊断契约；随后根据真实排障需求增加有限在途任务信息，最后再评估 epoch 一致性和事件流。

这样可以快速获得完整生命周期可见性，同时避免把所有执行路径过早改造成高成本的全量事件系统。
