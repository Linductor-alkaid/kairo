# Facade 完整度与失败可观察性更新计划

本文档承接 README / API 文档中新的对外宣称：

> 自动调优可以安全回退；任务异常、提交拒绝、实时队列丢任务、超时等运行时状态必须通过 `future`、返回值、状态计数或监控统计保持可观察。用户可以选择不响应，但库不能让失败无迹可寻。

目标不是把所有失败都变成抛异常，也不是把 facade 做成静默万能层；目标是让普通用户优先面对 `Executor` 这一层完成常见工作流，并且在任务状态异常时有统一、低摩擦的观察入口。

---

## 当前基线

- [x] README / README_zh 已从“失败静默”改为“自动调优安全回退，任务失败可观察”。
- [x] docs/API.md 已更新 facade 哲学：默认即最优，失败可观察。
- [x] docs/MIGRATION.md 已补充失败可观察性约定。
- [x] 实时执行器 `push_task_ex()` 已返回入队结果，并通过 `dropped_task_count` 暴露丢任务。
- [x] 旧 `push_task()` 虽不返回结果，但失败仍累计到 `dropped_task_count`。
- [x] 普通异步任务异常尚未统一进入 facade 可见的失败通道。
  （过时基线：计划执行前状态，阶段 3 已统一，2026-09-22 回填）
- [x] `submit_periodic()` 丢弃 future，周期任务异常可能无可见出口。
  （阶段 3 交付：facade_submit_periodic 周期包装与诊断 + get_periodic_task_status +
  test_periodic_failure_observability，2026-09-22 回填）
- [x] `Executor` facade 尚无统一失败回调 / 最近错误 / 失败事件查询入口。
  （阶段 1 交付：set_failure_callback / get_failure_status / get_recent_failures，
  2026-09-22 回填）
- [x] 多个注册/初始化 API 仍只返回 `bool`，失败原因不可诊断。
  （阶段 5 交付：initialize_ex / register_gpu_executor_ex + test_executor_result_diagnostics，
  2026-09-22 回填）
- [x] 实时任务推送仍要求用户先拿底层 `IRealtimeExecutor*`，facade 完整度不足。
- [x] `wait_for_completion()` 超时返回不可区分，调用方无法知道是否真的完成。

---

## 设计原则

1. **Facade 优先**：新手路径应尽量只需要 `#include <executor/executor.hpp>` 和 `Executor` 方法；底层接口仍保留给高级用户。
2. **兼容优先**：不破坏现有 `submit()`、`push_task()`、`initialize()` 等签名；新增 `try_*`、`*_ex`、诊断结构或观察接口。
3. **失败不静默**：任务异常、提交拒绝、背压丢弃、软超时、等待超时必须至少落到一种可见渠道。
4. **自动调优不是任务失败**：CPU 探测、affinity、priority、mlock、timer slack 等平台调优失败可以安全回退，但应在诊断/调试层可解释，不应污染任务失败计数。
5. **状态可监控**：所有长期运行或后台行为都要能从状态结构、统计结构或事件回调中被观察。

---

## 阶段 1：统一失败事件模型

### 任务

- [x] 新增 `FailureEvent` / `ExecutorFailureEvent` 类型，建议字段：
  - `FailureKind kind`：`TaskException`、`SubmitRejected`、`TaskTimeout`、`RealtimeDrop`、`GpuFailure`、`WaitTimeout`、`TuningFallback`。
  - `std::string executor_name`
  - `std::string task_id`
  - `std::string message`
  - `std::exception_ptr exception`
  - `std::chrono::steady_clock::time_point timestamp`
- [x] 新增 `ExecutorFailureStatus` 或扩展现有状态查询，至少包含累计计数：
  - `task_exception_count`
  - `submit_rejected_count`
  - `timeout_count`
  - `realtime_drop_count`
  - `wait_timeout_count`
  - `tuning_fallback_count`
- [x] 在 `Executor` facade 暴露：
  - `set_failure_callback(...)`
  - `get_failure_status()`
  - `get_recent_failures(size_t max_count)`
  - `clear_recent_failures()`
- [x] 为 failure ring buffer 设置固定容量，避免长期运行进程无限增长；容量可配置或给出合理默认。

### 验收

- [x] 用户只通过 `Executor` 就能订阅任务失败与提交拒绝。
- [x] 未设置 callback 时，失败仍会累计到状态/最近事件，而不是消失。
- [x] callback 自身抛异常不会杀死 worker 或后台线程。

---

## 阶段 2：修正普通异步任务失败可见性

### 问题

当前 `IAsyncExecutor::submit()` 在 wrapper 内捕获用户异常并写入 `promise`。底层 `ThreadPool::execute_task()` 看到 wrapper 正常返回，可能将任务统计为成功；如果用户不 `future.get()`，失败不可见。

### 任务

- [x] 调整 `submit()` / `submit_priority()` / `submit_batch()` 的包装方式，使用户任务异常同时：
  - 写入对应 `future`
  - 触发统一 failure event
  - 更新失败统计或专用 facade 失败计数
- [x] 明确 `AsyncExecutorStatus::failed_tasks` 的语义：
  - 方案 A：计入用户任务异常。
  - 方案 B：保持底层执行失败语义，新增 facade 级 `task_exception_count`。
  - 推荐方案 A，但需评估现有测试与 `wait_for_completion()` 不变量。
- [x] 对 `submit_batch_no_future()` 增加强制可见性：
  - 用户任务异常没有 future 可承载，必须进入 failure event / status counter。
- [x] 对提交被拒绝的场景计数：
  - shutdown 后提交
  - 空 batch
  - executor 未可用

### 验收

- [x] 单个 `submit([]{ throw ...; })` 即使不调用 `future.get()`，状态中也能观察到失败。
- [x] `submit_batch_no_future()` 内部任务抛异常时，failure callback 被调用且计数递增。
- [x] shutdown 后提交被拒绝时，调用方通过 future/返回值/事件至少一种方式可见。

---

## 阶段 3：修正延迟与周期任务状态

### 问题

`submit_periodic()` 当前周期回调只调用 `executor->submit(task)`，但丢弃返回的 `future`；周期任务异常可能只进入被丢弃的 future。延迟任务也需要确认提交失败和执行失败均可观察。

### 任务

- [x] 为 `submit_periodic()` 增加内部任务包装：
  - 用户任务异常触发 failure event。
  - 每次周期执行失败累计到周期任务状态。
  - 已记录连续失败计数、最后一次失败时间、最后错误消息。
- [x] 新增周期任务状态查询：
  - `get_periodic_task_status(task_id)`
  - `get_all_periodic_task_status()`
- [x] 定义周期任务失败后的默认行为：
  - 默认继续调度，但记录失败。
  - 失败后停止/退避/连续 N 次失败后停止保留为后续可选配置。
- [x] `submit_delayed()` 到期提交失败时，应设置 promise 异常并触发 failure event。
- [x] 定时器线程中 facade/manager 已 shutdown 时，延迟/周期任务不可悄悄丢失，应写入 failure event 或设置 future 异常。

### 验收

- [x] 周期任务抛异常但用户没有 future 时，仍能从 failure status 或 periodic status 观察到。
- [x] `cancel_task()` 对不存在 ID 的失败保持返回值可见，并可选记录诊断事件。
- [x] shutdown 期间未执行的 delayed/periodic 任务处理策略有文档说明和测试覆盖。

---

## 阶段 4：补全实时任务 facade

### 问题

实时背压可见性在底层已有基础，但普通用户仍需 `get_realtime_executor()` 后调用底层 `push_task_ex()`，这削弱 facade 完整度。

### 任务

- [x] 在 `Executor` facade 增加：
  - `bool push_realtime_task(const std::string& name, std::function<void()> task)`
  - `bool try_push_realtime_task(...)`
  - 可选 `submit_realtime_task(...)` 命名别名，需避免与普通 `submit` 混淆。
- [x] facade push 失败时统一记录 `RealtimeDrop` 或 `SubmitRejected` 事件：
  - 实时 executor 不存在
  - 未启动
  - 空任务
  - 队列满
  - 对象池耗尽
- [x] 保留 `get_realtime_executor()` 作为高级逃生口，但 README/API 示例优先使用 facade push。
- [x] `RealtimeExecutorStatus` 可选新增：
  - `rejected_not_running_count`
  - `rejected_empty_task_count`
  - `pool_exhausted_count`
  - `queue_full_count`
  以便把 `dropped_task_count` 拆出原因。

### 验收

- [x] 不接触 `IRealtimeExecutor*`，用户即可注册、启动、推送、观察实时任务。
- [x] facade push 失败同时通过返回值和状态计数可见。
- [x] 旧 `push_task()` 仍兼容，但文档标注为兼容入口，推荐新代码使用 facade/`push_task_ex()`。

---

## 阶段 5：可诊断的初始化与注册 API

### 问题

`initialize()`、`register_realtime_task()`、`register_gpu_executor()` 等 API 返回 `bool`，失败原因丢失。

### 任务

- [x] 新增轻量 Result 类型，例如：
  - `ExecutorResult`
  - `ExecutorErrorCode`
  - `std::string message`
- [x] 新增非破坏性 API：
  - `initialize_ex(config) -> ExecutorResult`
  - `register_realtime_task_ex(name, config) -> ExecutorResult`
  - `start_realtime_task_ex(name) -> ExecutorResult`
  - `register_gpu_executor_ex(name, config) -> ExecutorResult`
- [x] 旧 `bool` API 保留，并委托到 `_ex` 后只返回 `ok`。
- [x] 常见错误码：
  - `AlreadyInitialized`
  - `AlreadyShutdown`
  - `InvalidConfig`
  - `DuplicateName`
  - `NotFound`
  - `BackendUnavailable`
  - `StartFailed`
  - `PermissionDenied`
- [x] `_ex` 失败同时写入 failure/diagnostic event，但不把配置错误混为任务失败。

### 验收

- [x] 用户看到 `false` 时，可以通过 `_ex` 或最近诊断知道原因。
- [x] GPU 未编译/运行时不可用能清晰区分。
- [x] RT 配置无效能返回具体字段原因。

---

## 阶段 6：等待与生命周期可观察

### 问题

`wait_for_completion()` 内部最多等待 300 秒，但返回 `void`，超时不可区分。

### 任务

- [x] 新增：
  - `bool wait_for_completion_for(std::chrono::duration<...>)`
  - `WaitResult wait_for_completion_ex(timeout)`
  - `bool is_idle()` 或 `get_completion_status()`
- [x] 旧 `wait_for_completion()` 保持阻塞语义，文档说明其行为。
- [x] 等待超时记录 `WaitTimeout` 事件并累计计数。
- [x] shutdown 路径中如果等待超时，应有可观察诊断，而不是继续假装全部完成。

### 验收

- [x] 测试能明确区分完成返回和超时返回。
- [x] wait timeout 后状态能说明仍有 active / queued / pending 任务。

---

## 阶段 7：通信/并发辅助 facade

### 背景

`tests/harness/test_comm_facade_usage.cpp` 已替换为可编译用例，说明常见通信模式已通过 facade 覆盖。

更完整的设计与执行拆分见：

- [通信与并发辅助 Facade 设计](../design/comm_facade.md)
- [通信与并发辅助 Facade 更新计划](comm_facade_update_plan.md)

### 任务

- [x] 设计并落地 `executor::comm` facade API：
  - `MpscChannel<T>`
  - `SpscChannel<T>`
  - `LatestMailbox<T>`
  - `PhaseGate`
  - `Sequencer`
  - `DoubleBuffer<T>`
  - `RealtimeChannel<T>`
- [x] 暴露任务时序 API：
  - `TaskHandle`
  - `submit_after(...)`
  - `when_all(...)`
- [x] 每个通信组件必须定义失败可见性：
  - push 失败返回值
  - drop/overwrite 计数
  - close/shutdown 状态
  - 高水位或容量状态
  - stale / missed phase / producer-consumer lag
- [x] 启用并补全 `test_comm_facade_usage.cpp` 中 disabled 用例。
- [x] 在 README/API 加入场景式示例，避免用户直接拼底层队列、锁和生命周期。
- [x] 新增综合用户场景示例 `examples/comm_robot_pipeline.cpp`，覆盖采集、规划、实时控制、状态监控、启动顺序、任务依赖和通信诊断。

### 验收

- [x] 典型 producer/consumer、latest-state、phase handoff、realtime drain 场景无需用户直接操作底层无锁队列。
- [x] 所有丢弃、覆盖、关闭后的提交均可观察。
- [x] 用户能通过统计回答 drop、latency、stale、missed phase 和 producer/consumer lag。

---

## 阶段 8：测试矩阵

- [x] `test_executor_failure_observability.cpp`
  - [x] `submit` 用户异常可观察。
  - [x] `submit_priority` 用户异常可观察。
  - [x] `submit_batch` 部分任务异常可观察。
  - [x] `submit_batch_no_future` 用户异常可观察。
  - [x] shutdown 后提交拒绝可观察。
- [x] `test_periodic_failure_observability.cpp`
  - [x] 周期任务抛异常后无 future 也可观察。
  - [x] 连续失败计数和最后错误可查询。
  - [x] cancel 不存在任务返回 false 且可诊断。
- [x] `test_realtime_facade_push.cpp`
  - [x] facade 推送成功。
  - [x] 不存在 RT executor 推送失败可见。
  - [x] 未启动 / 已停止推送失败可见。
  - [x] 队列满 / 池耗尽推送失败可见。
- [x] `test_executor_result_diagnostics.cpp`
  - [x] 初始化重复。
  - [x] shutdown 后初始化。
  - [x] RT 无效配置。
  - [x] GPU 后端不可用。
- [x] `test_wait_completion_result.cpp`
  - [x] wait 完成返回成功。
  - [x] wait 超时返回 timeout，并保留 pending 状态。

---

## 阶段 9：文档与示例同步

- [x] README / README_zh
  - [x] 示例优先使用 `Executor` facade 完成普通任务、实时任务、状态观察。
  - [x] 明确 `future.get()` 是获取返回值/异常的方式，但状态计数也能监控失败趋势。
- [x] docs/API.md
  - [x] 新增 failure event / result / wait result API 文档。（failure event / result 已完成；wait result 留阶段 6）
  - [x] 标注旧 bool API 与新 `_ex` API 的关系。
  - [x] 修正 `AsyncExecutorStatus::failed_tasks` 语义。
- [x] docs/MIGRATION.md
  - [x] 增加从旧接口迁移到 facade push、`*_ex` result、failure callback 的建议。
- [x] examples
  - [x] 新增 `examples/failure_observability.cpp`。
  - [x] 新增或更新 `examples/realtime_can.cpp`，优先展示 facade push 和 drop 监控。
  - [x] 新增 `examples/periodic_monitoring.cpp`。

---

## 推荐实施顺序

1. 阶段 1：统一失败事件模型。
2. 阶段 2：普通异步任务失败可见。
3. 阶段 3：延迟/周期任务失败可见。
4. 阶段 4：实时任务 facade push。
5. 阶段 5：`*_ex` 诊断 API。
6. 阶段 6：等待与生命周期可观察。
7. 阶段 8：补齐回归测试矩阵。
8. 阶段 9：同步文档与示例。
9. 阶段 7：通信/并发辅助 facade，可作为后续较大版本推进。

这个顺序先保证新宣称不落空，再补用户体验层面的完整 facade。
