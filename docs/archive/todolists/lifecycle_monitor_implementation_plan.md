# Executor 完整生命周期 Monitor 实施计划

本文档基于[完整生命周期 Monitor 演进设计](../design/lifecycle-monitor-design.md)，列出从现有分散监控能力演进到统一生命周期快照的实施任务、验收标准和发布顺序。

## 目标

- [x] 新增只读的 `ExecutorSnapshot`，一次调用汇总 Executor 生命周期、各类后端状态、失败摘要和任务统计。
- [x] 第一阶段采用明确标注的 best-effort 语义，不改变提交、future、shutdown 和实时周期行为。
- [x] 为等待超时、shutdown 失败、启动失败和健康检查提供统一故障现场入口。
  （阶段 2.2 交付：wait/shutdown 超时保留完整 snapshot + sequence，见
  executor.cpp wait_for_completion_ex，2026-09-22 回填）
- [x] 后续按实际排障需求扩展有限在途任务诊断，而不是默认保存无限任务历史。
  （阶段 3 已实现有界 in-flight 表：types.hpp TaskLifecycleSnapshot 与
  in_flight_count/in_flight_tasks 字段，2026-09-22 回填）

## 当前基线

- [x] `TaskMonitor` 已支持按 `task_type` 聚合任务成功、失败、超时和执行时间。
- [x] `StatisticsCollector` 已提供任务统计，并通过 provider 获取 GPU 状态。
- [x] 已存在 `AsyncExecutorStatus`、`RealtimeExecutorStatus`、`BlockingIoExecutorStatus` 和 `GpuExecutorStatus`。
- [x] `ExecutorFailureStatus`、`get_recent_failures()` 和 failure callback 已提供失败累计与有限最近事件。
- [x] `ExecutorManager` 已使用 `shared_ptr` snapshot getter 保护后端查询期间的对象生命周期。
- [x] `CommStats` 已提供通信组件本地统计，但默认不并入 `ExecutorFailureStatus`。
- [x] 已具备统一的 `ExecutorLifecycleState` 生命周期枚举。
- [x] 已具备一次调用汇总所有后端和失败/统计状态的 `ExecutorSnapshot`。
- [x] 已具备统一 snapshot 序号、采集时间和 partial/inconsistency 标记。
- [x] 尚无在途任务的有界生命周期诊断表。
  （过时基线：阶段 3 已交付，见 types.hpp in-flight 字段，2026-09-22 回填）

## 设计约束

1. **Facade 优先**：新增能力通过 `Executor::get_snapshot()` 暴露，保留现有单项状态 API。
2. **不触发懒初始化**：snapshot 查询不能因为读取状态而创建默认异步执行器。
3. **局部同步、全局 best-effort**：各 provider 在自身同步域内返回值拷贝，统一快照不承诺事务级一致性。
4. **热路径无新增阻塞**：任务执行、实时周期和无锁队列路径不执行 map/vector 复制或 JSON 序列化。
5. **有界保留**：最近事件和在途诊断必须有容量上限；不保存任务 callable、payload 或异常对象的不可序列化内容。
6. **生命周期不混淆**：总体 lifecycle 是摘要，不能替代 backend-specific 的 running、stop reason、queue 和错误字段。

---

## 阶段 0：契约冻结与字段盘点

### 0.1 生命周期模型

- [x] 在 `include/executor/types.hpp` 定义 `ExecutorLifecycleState`：
  - [x] `Created`
  - [x] `Initializing`
  - [x] `Running`
  - [x] `Draining`
  - [x] `Stopped`
  - [x] `Failed`
- [x] 为每个状态补充中文/英文语义说明。
- [x] 明确状态推导规则：未初始化、任一后端运行、停止请求、全部停止、关键初始化失败分别如何映射。
- [x] 明确总体 lifecycle 不作为提交 reservation，也不改变现有 `is_running` 语义。

### 0.2 Snapshot 字段

- [x] 在 `include/executor/types.hpp` 定义 `ExecutorSnapshot`。
- [x] 固定 `schema_version`、`snapshot_sequence` 和 `captured_at` 的类型及语义。
- [x] 固定 `partial` 与 `consistency_note` 的设置条件。
- [x] 固定汇总字段：
  - [x] `running_backend_count`
  - [x] `stopping_backend_count`
  - [x] `active_task_count`
  - [x] `queued_task_count`
  - [x] `failed_task_count`
  - [x] `dropped_work_count`
- [x] 固定后端容器名称：`realtime`、`blocking_io`、`gpu`，默认异步后端使用 `async`。

### 阶段验收

- [x] 设计文档、公共类型注释和实施计划中的字段名称完全一致。
- [x] 明确第一版不包含在途任务明细和通信 payload。
- [x] 明确 snapshot 是低频诊断接口，不建议放在实时周期中调用。

---

## 阶段 1：统一 best-effort Snapshot（MVP）

### 1.1 Manager 批量状态 Provider

**Files:**

- Modify: `include/executor/executor_manager.hpp`
- Modify: `src/executor/executor_manager.cpp`

- [x] 增加全部实时执行器状态查询：
  - [x] `std::map<std::string, RealtimeExecutorStatus> get_all_realtime_executor_statuses() const`
  - [x] 使用注册表读锁和 `shared_ptr` 快照，避免返回裸指针。
- [x] 增加全部 Blocking I/O 执行器状态查询：
  - [x] `std::map<std::string, BlockingIoExecutorStatus> get_all_blocking_io_executor_statuses() const`
- [x] 保持已有 `get_all_gpu_executor_statuses()`，检查未启用 GPU 时返回空 map 的语义。
- [x] 增加只读 lifecycle 查询或内部状态 provider，不能触发默认异步执行器懒初始化。
- [x] 对 provider 不可用、注册表正在关闭或对象已移除的情况保留可识别结果。

### 1.2 Monitor 汇总类

**Files:**

- Create: `include/executor/monitor/executor_monitor.hpp`
- Create: `src/executor/monitor/executor_monitor.cpp`
- Modify: `src/CMakeLists.txt`

- [x] 创建 `monitor::ExecutorMonitor` 或等价 `SnapshotCollector` 类。
- [x] 注入 `ExecutorManager`、`StatisticsCollector` 和 failure state 的只读 provider。
- [x] 实现 `ExecutorSnapshot collect() const`。
- [x] 按固定顺序采集：lifecycle → backend shared snapshots → failure → recent failures → task statistics → aggregate counters。
- [x] 在采集期间不调用会懒初始化的 `get_default_async_executor()`；使用 `has_default_async_executor()` 和 `get_default_async_executor_snapshot()`。
- [x] 对 backend 查询异常进行隔离：标记 `partial=true`，在 `consistency_note` 中写明 provider 名称，不让诊断异常传播到业务线程。
- [x] 维护同一 Monitor 实例内单调递增的 `snapshot_sequence`。
- [x] 明确 `captured_at` 是采集开始时间还是完成时间，并在注释和测试中固定。

### 1.3 Executor Facade API

**Files:**

- Modify: `include/executor/executor.hpp`
- Modify: `src/executor/executor.cpp`

- [x] 增加 `ExecutorSnapshot get_snapshot() const`。
- [x] 在 `Executor` 构造函数中初始化 Monitor，并确保单例模式与实例化模式各自隔离序号和状态。
- [x] 确认 snapshot 查询不会创建默认异步执行器。
- [x] 保持已有 `get_*_status()`、`get_failure_status()` 和任务统计 API 不变。
- [x] 对对象析构、shutdown 后和部分 backend 注册场景定义返回值。

### 1.4 MVP 测试

**Files:**

- Create: `tests/test_executor_snapshot.cpp`
- Modify: `tests/CMakeLists.txt`

- [x] 未初始化时 snapshot 为 `Created`，且查询前后默认异步执行器仍未初始化。
- [x] 初始化成功后 lifecycle 为 `Running`，async 状态与 `get_async_executor_status()` 对账。
- [x] 提交 active/queued 任务时，汇总 active、queued 和 pending 与现有 completion 状态一致。
- [x] 注册实时、Blocking I/O 和 GPU（可用时）后，全部 map 包含对应名称和状态。
- [x] shutdown 期间 snapshot 能观察到 `Draining` 或 stopping backend；完成后为 `Stopped`。
- [x] 初始化失败或关键 backend 启动失败可观察为 `Failed`，并能关联 failure status/recent event。
- [x] 并发注册、停止、查询和析构压力测试无数据竞争、死锁或 use-after-free。
- [x] 连续调用 snapshot 的 `snapshot_sequence` 严格递增。

### 阶段验收

- [x] `ctest` 中新增 snapshot 测试全部通过。
- [x] 现有监控、失败可观察性、实时生命周期和 shutdown 测试不回归。
- [x] `get_snapshot()` 不触发懒初始化、不阻止 shutdown、不改变任务结果。

---

## 阶段 2：故障现场与导出

### 2.1 可读导出

**Files:**

- Create: `include/executor/monitor/executor_snapshot_formatter.hpp`
- Create: `src/executor/monitor/executor_snapshot_formatter.cpp`
- Modify: `include/executor/executor.hpp`
- Modify: `src/executor/executor.cpp`

- [x] 提供稳定的文本格式，适用于日志和故障支持包。
- [x] 增加 `std::string get_snapshot_text() const`；JSON 作为可选后续 API。
- [x] 所有枚举使用稳定字符串，不直接输出整数值。
- [x] 时间、计数和容量字段使用固定单位。
- [x] 输出中包含 `schema_version`、`snapshot_sequence`、`captured_at`、`lifecycle` 和 `partial`。
- [x] 不序列化 `std::exception_ptr`、任务 callable、用户 payload 和通信数据内容。

### 2.2 关键路径集成

- [x] `wait_for_completion_ex()` 超时时允许通过显式 callback 或诊断 hook 获取 snapshot。
- [x] shutdown 等待超时路径保留完整 snapshot，而不是只输出 active/queued 数量。
- [x] 初始化、注册和启动失败路径可附加 snapshot sequence 和 backend 状态。
- [x] 导出动作在外部线程或调用线程执行，不在 realtime cycle thread 内执行。

### 2.3 测试与验收

- [x] 文本输出字段顺序稳定，便于 golden test 或日志解析。
- [x] 空 backend、GPU 未编译和 partial provider 场景输出可读且不崩溃。
- [x] 导出失败不会改变 Executor 生命周期和任务结果。
- [x] 建立单次采集/格式化耗时和动态分配次数的性能基线：`benchmark_lifecycle_snapshot` 输出 idle initialized 场景的 wall/reported 平均耗时、格式化器本地分配次数和输出字节数。

---

## 阶段 3：有限在途任务生命周期诊断

### 3.1 任务状态模型

**Files:**

- Modify: `include/executor/types.hpp`
- Modify: `include/executor/monitor/task_monitor.hpp`
- Modify: `src/executor/monitor/task_monitor.cpp`

- [x] 定义 `TaskLifecycleState`：`Pending`、`Queued`、`Running`、`Succeeded`、`Failed`、`TimedOut`、`Rejected`、`Cancelled`、`DependencyBlocked`。
- [x] 定义 `TaskLifecycleSnapshot`，只保存 task id/type、executor name、时间戳和状态。
- [x] 为 TaskMonitor 增加有限容量 in-flight 表及配置接口。
- [x] 增加采样策略：聚合统计采样与在途诊断采样可独立配置。
- [x] 容量溢出时保留计数并在统一 snapshot 中标记诊断不完整。

### 3.2 生命周期埋点

- [x] 在默认异步线程池的 accepted/queued/running/complete/fail/timeout 路径接入状态更新；提交拒绝仍由既有 failure status/recent failure 观察，尚不保留为 in-flight 条目。
- [x] 在任务图 pending、dependency blocked、dependency failed 路径接入状态更新。
- [x] 在实时 push accepted/drop 路径复用 `RealtimeExecutorStatus` 的运行、队列容量和 drop/rejection 计数；不在 realtime cycle thread 写入有锁的普通任务表。
- [x] 在 GPU kernel accepted/running/completed/failed 路径复用 `GpuExecutorStatus` 的 active/queued/completed/failed kernel 计数。
- [x] 在 Blocking I/O worker start/ready/stop/exception 路径复用 `BlockingIoExecutorStatus` 的 running/ready/stop reason/error 状态。
- [x] 普通线程池和任务图诊断更新均隔离监控异常，不能影响任务提交、完成和 worker 退出。

### 3.3 查询与测试

- [x] 在 `ExecutorSnapshot` 中增加 `in_flight_count`、最老任务年龄和按状态计数。
- [x] 增加有限数量的 `in_flight_tasks`，默认不暴露 payload。
- [x] 测试长任务、队列积压、容量溢出、依赖阻塞和软超时；提交拒绝仍通过既有 failure status/recent failure 观察，不进入在途表。
- [x] 对账：普通任务和任务图终态会从在途表移除，既有 `TaskStatistics` 与 failure counters 仍由原路径维护。

### 阶段验收

- [x] 在默认异步线程池中，在途任务诊断可定位“运行中慢任务”和“排队未执行任务”。
- [x] 关闭监控或在途采样率为 0 时，任务结果和 Executor 生命周期语义不变。
- [x] 有界容量和高并发压力下无无限内存增长。

---

## 阶段 4：一致性增强（按需）

### 4.1 Epoch 校验

- [x] 为 Manager 增加轻量 `state_epoch`，覆盖注册表和 Manager 生命周期边界变化。
- [x] snapshot 采集前后读取 epoch；变化时最多有限重试两次。
- [x] 重试仍不稳定时设置 `partial=true`，并写入 `consistency_note=epoch_changed`。
- [x] 不使用一把全局锁包住所有 backend 状态读取。

### 4.2 有界事件流（可选）

> 2026-09-22 定性：条件触发的可选项——仅当 best-effort snapshot 无法定位真实故障时
> 才启动；阶段验收已确认"事件流保持可选、未默认开启"，非欠账。

- [ ] 仅当 best-effort snapshot 无法定位真实故障时，增加生命周期事件 ring buffer。
- [ ] 事件至少包含 sequence、时间、backend、task id、前后状态和简短原因。
- [ ] 事件容量、采样率和敏感字段策略可配置。
- [ ] snapshot 保存聚合状态，事件流保存有限变化历史；不实现无限 event sourcing。

### 阶段验收

- [x] 并发状态变更下能区分稳定快照与 partial 快照。
- [x] epoch 校验开销已通过生命周期快照基线验证后默认开启；事件流仍保持可选、未默认开启。

---

## 发布与文档更新

### API/用户文档

- [x] 更新 `docs/API.md`，增加统一 snapshot 使用示例、生命周期语义和 best-effort 限制。
- [x] 更新 `README_zh.md` 和 `README.md` 的监控能力说明。
- [x] 更新 `website/zh/reliability/monitoring.md`，说明统一 snapshot 与任务统计、失败状态、后端状态的边界。
- [x] 更新 `website/zh/quick-start/lifecycle.md` 和 `website/zh/tutorial/waiting-and-status.md`，加入超时/关闭现场采集示例。
- [x] 增加 `examples/lifecycle_snapshot.cpp`，展示初始化、提交积压、查询、失败和 shutdown snapshot。
- [x] 更新 `examples/CMakeLists.txt` 与构建说明。

### 版本与兼容

- [x] 将 `ExecutorSnapshot`、生命周期枚举和新 API 纳入版本变更说明。
- [x] 通过 `schema_version` 保证文本/JSON 导出可演进。
- [x] 保留所有既有单项状态和统计 API，不要求用户迁移已有监控代码。

---

## 总体验收标准

- [x] 用户只使用 `Executor::get_snapshot()` 即可获得完整的 Executor 生命周期和所有已注册 backend 状态摘要。
- [x] snapshot 查询不触发懒初始化，不改变提交、future、shutdown 和 realtime 语义。
- [x] snapshot 与注册、停止、析构并发运行无数据竞争、死锁和 use-after-free。
- [x] active、queued、failed、dropped 等汇总字段能与现有 backend status、failure status 和 task statistics 对账。
- [x] 未初始化、运行中、排空、停止、启动失败和 partial provider 场景均有测试覆盖。
- [x] 监控关闭、采样率为 0 和诊断容量耗尽时，核心执行路径仍保持正确性和有界资源使用。
- [x] 文档明确 best-effort 快照不是事务级一致读，也不代表提交 reservation 或任务可恢复状态。

## 参考

- 设计文档：[docs/design/lifecycle-monitor-design.md](../design/lifecycle-monitor-design.md)
- 现有监控实现：`src/executor/monitor/task_monitor.*`、`src/executor/monitor/statistics_collector.*`
- 生命周期管理：`include/executor/executor.hpp`、`include/executor/executor_manager.hpp`
- 现有状态类型：`include/executor/types.hpp`、`include/executor/config.hpp`
- 失败可观察性计划：[facade_observability_update_plan.md](facade_observability_update_plan.md)
- 监控用户文档：`website/zh/reliability/monitoring.md`
