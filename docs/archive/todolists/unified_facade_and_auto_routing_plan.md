# 统一 Facade 与自动路由开发计划

本文档将[统一 Facade 与自动路由设计](../design/unified_facade_and_auto_routing.md)拆分为可交付、可回归验证的开发任务。实现必须保留现有显式执行器 API 和各后端的真实完成/背压语义；自动路由只新增便利入口与统一控制面。

## 范围与实施原则

- `submit_auto()` 只服务于具有 `future` 完成语义的默认异步线程池和 CPU/GPU 双路径任务。
- 无锁和实时路径使用返回接收结果的 `dispatch_auto()`，不得伪造成 `future` 已完成语义。
- Blocking I/O 保持长期 worker 模型，通过生命周期 handle 统一管理，不进入单次任务路由。
- 路由判断只使用用户声明的意图、选项和后端能力快照；不分析 callable 内容，不根据 deadline 或 priority 推断实时安全性。
- capability snapshot 仅供预检和诊断；所有实际投递仍须处理并发 stop、队列满或后端拒绝。
- `RoutingDecision` 解释路由原因，`ExecutorFailureEvent` 报告提交/执行失败；两者独立记录。

## 阶段 0：基线与公共约定

### 任务

- [x] 盘点 `Executor`、`ExecutorManager`、GPU scheduler、无锁、实时和 Blocking I/O 的现有提交、状态和 shutdown 路径。
- [x] 确认 `0.3.x` 兼容边界：不改动 `submit()`、`submit_gpu()`、legacy 四参数 `submit_auto()`、实时和 Blocking I/O 的已有语义或返回类型。
- [x] 在 `include/executor/task_options.hpp` 定义并导出：
  - [x] `ExecutionIntent`、`FallbackPolicy`、`ExecutionBackend`、`RoutingReason`。
  - [x] `TaskOptions`，包括名称、优先级、意图、偏好执行器、回退策略和诊断 deadline。
  - [x] 按值返回的任务 builder，使普通 lambda 仍可直接提交。
- [x] 将新头文件聚合到 `include/executor/executor.hpp`，并检查只包含 facade 头文件的用户代码可编译。
- [x] 明确任务模型默认值：`TaskOptions` 使用 `Auto` 和 `NoFallback`；后续阶段的普通 `submit_auto(F&&, Args&&...)` 将采用该默认值并只选默认异步线程池。

### 验收

- [x] 新公共类型不改变既有公开 struct 的字段含义或 ABI 约定。
- [x] 无 GPU、无实时权限和未注册可选后端的构建/运行路径仍可用。
- [x] 公共枚举、返回结果和错误信息命名与既有 `ExecutorResult`、failure event 风格一致。

## 阶段 1：CPU/GPU 双路径任务与安全提交

### 任务

- [x] 实现 `CpuGpuTask<void>` 与 `cpu_gpu_task(cpu, gpu)`：CPU callable 和 GPU callable 可使用不同签名。
- [x] 增加 `.name()`、`.priority()`、`.data_size()`、`.compute_intensity()`、`.preferred_executor()`、`.fallback()` 等配置入口，并映射为 `TaskOptions` / `GpuTaskConfig`。
- [x] 在首版静态限制 CPU/GPU 自动任务为 `void` 返回；对带返回值需求给出明确编译期诊断或拒绝说明。
- [x] 新增 `Executor::submit_auto()` 重载：
  - [x] 普通 callable / `GeneralCpu` 委托默认异步线程池并返回 `future<T>`。
  - [x] CPU/GPU 任务只在 GPU 可提交时交由 `GpuScheduler` 决策。
  - [x] GPU 路径提交失败时，按 fallback 重新处理或让返回 future 立即带异常，绝不遗留未兑现 future。
- [x] 保留 legacy CPU/GPU `submit_auto(TaskCharacteristics, name, kernel, GpuTaskConfig)` 实现和测试，并在 API 注记迁移方向；`0.3.x` 不添加编译期弃用标记。
- [x] 为缺失、未运行或队列达到已知硬容量的 GPU 建立可提交性检查；实际提交竞争仍由后端提交路径处理。

### 验收

- [x] `submit_auto([] { return value; })` 与 `submit()` 一样运行于默认异步线程池，future 正确兑现值或异常。
- [x] GPU 未注册或不可用时，`AllowCpu` 回退 CPU；`NoFallback` 明确拒绝；`RequireRequestedBackend` 仅接受已可提交的指定后端。
- [x] GPU 已注册但 stop、队列满或竞争拒绝时，调用者不会永久等待。（需 CUDA/OpenCL 后端集成测试）
- [x] legacy overload 在 GPU 未就绪时仍保持既有“无隐式回退”的行为。

## 阶段 2：能力注册表与可解释路由

### 任务

- [x] 在 `ExecutorManager` 建立内部 capability snapshot，包含后端类型、名称、注册/运行状态、支持的提交协议、GPU 能力、pending work 与 capacity hint。
- [x] 统一 Manager 所有当前已注册后端的名称唯一性检查，覆盖默认异步、GPU、实时和 Blocking I/O 注册表；无锁注册表留待阶段 3。
- [x] 新增内部 `TaskRouter`，由 `Executor` 持有；输入为不可变任务请求与 capability snapshot，输出 `RoutingDecision`。
- [x] 实现第一阶段规则：
  - [x] `Auto` 和 `GeneralCpu` 选择默认异步后端。
  - [x] `CpuOrGpu` 依次处理强制指定后端、偏好执行器、GPU scheduler 与 CPU fallback。
  - [x] `LowLatency`、`RealtimeQueue`、`BlockingWorker` 经泛型 `submit_auto()` 必须拒绝，并说明应使用 typed API。
- [x] 实现 facade 路由诊断：最近决策环形缓冲、`get_last_routing_decision()`、`get_recent_routing_decisions()` 和 `set_routing_callback()`。
- [x] 路由回调隔离异常；缓冲区默认容量为 128，允许容量为 0 时关闭保留但保留 callback。
- [x] 对回退与拒绝写入准确的 `RoutingReason`、`fell_back` 和详情；允许回退不计为用户任务失败。

### 验收

- [x] 每一次 `submit_auto()` 均产生可查询的决策，包括默认线程池选择。
- [x] 显式目标优先于启发式；不存在、未运行或任务形态不支持时严格遵循 fallback。
- [x] scheduler 不会把不可提交 GPU 当作候选；预检与真实提交间发生的竞争能留下拒绝/回退诊断。
- [x] callback 抛异常不影响投递、worker、future 或 failure buffer。

## 阶段 3：无锁执行器统一注册与有界 dispatch

### 任务

- [x] 将 `LockFreeTaskExecutor` 注册进 `ExecutorManager` 的能力与生命周期管理，但不要求其继承 `IAsyncExecutor`。
- [x] 定义 `DispatchResult`：至少包含 `accepted`、实际后端、执行器名称、`RoutingDecision` 和失败消息。
- [x] 新增 `Executor::dispatch_auto(TaskOptions, std::function<void()>)`：
  - [x] 仅用于 fire-and-forget、有界投递后端。
  - [x] `LowLatency` 仅在显式指定的运行中无锁执行器接受时投递。
  - [x] 空任务、停止、队列满、对象池耗尽均返回 `accepted = false`，并记录决策和 failure event。
- [x] 确保普通 `Auto` 不因性能理由自动选择无锁单消费者路径。
- [x] 规定 Manager shutdown 顺序：先从无锁注册表摘除并停止，再停止 Blocking I/O、实时、GPU 和默认异步后端。

### 验收

- [x] 无锁路径不提供伪造的完成 future；调用方能够区分“已接收”与“已完成”。
- [x] 后端未运行、队列满和空任务的拒绝原因、计数与 `DispatchResult` 保持一致。
- [x] dispatch 在 Manager 注册表的共享锁下完成；shutdown 先摘除后端，避免并发投递访问已释放对象。

## 阶段 4：实时与 Blocking I/O 的统一控制面

### 任务

- [x] 为 Blocking I/O 设计并实现 `BlockingWorkerSpec`、`WorkerHandle` 与 `start_worker()` facade：封装注册、启动、状态查询和停止。
- [x] `WorkerHandle` 保留 `IBlockingIoWorker::wakeup()`、`stop_token`、启动超时和退出原因的既有契约。
- [x] 评估并按需添加 `RealtimeHandle`；现有实时 API 已覆盖生命周期和状态，避免引入伪造 worker/future 完成模型的 handle。
- [x] 扩展统一状态/能力查询，使用户能从一个 facade API 枚举所有已注册后端及其运行状态。
- [x] 为未来 `dispatch_auto(... RealtimeQueue ...)` 预留 typed 路由入口：只选择用户指定且已启动的实时后端，绝不按 deadline、priority 或压力自动推断。
- [x] 明确 `wait_for_completion()` 仅表示 future 型异步工作完成，不表示实时周期或长期 worker 已退出。

### 验收

- [x] Blocking worker 的 wakeup、停止、启动失败和超时语义与当前实现一致。
- [x] 实时队列仍明确暴露有界背压/drop，未启动时不静默转投线程池。
- [x] 用户可发现全部后端状态，同时不会误解统一 facade 为同一种完成模型。

## 阶段 5：测试、文档与发布

### 测试任务

- [x] 为公共类型、builder、双路径 callable 和不兼容 intent 添加编译/单元测试。
- [ ] ⏸ 门控（CUDA 主机）：覆盖 CPU/GPU 路由矩阵：无配置、GPU 可用、未注册、已停止、后端错误、硬容量满、真实提交竞争，以及三种 fallback 策略。
  （无 GPU 侧已由 test_executor_auto_routing_stage1.cpp 覆盖 AllowCpu 回退、
  NoFallback/RequireRequestedBackend 拒绝；GPU 在场场景待 CUDA 主机，2026-09-22 注记）
- [x] 覆盖 routing buffer 的容量、顺序、清理、callback 异常隔离和与 failure event 的分离。
- [x] 覆盖无锁 dispatch 的接受、停止、队列满、对象池耗尽、空任务及 shutdown 并发。
- [x] 覆盖 Blocking worker 启动超时/停止和实时未启动/背压，验证不会经自动路由改变语义。
- [x] 复跑现有 GPU scheduler、facade failure observability、realtime、blocking I/O、lockfree 和 manager 测试，防止回归。（GPU 启用集成测试待 CUDA 主机复跑）

### 文档与迁移任务

- [x] 更新 `docs/API.md`，按 `submit_auto`、`dispatch_auto`、`start_worker` 与显式 API 说明返回语义和适用范围。
- [x] 更新 `docs/MIGRATION.md`：新 API 默认 `NoFallback`，legacy overload 在 `0.3.x` 不变，后续主版本才进入弃用/移除窗口。
- [x] 为 README 和教程加入场景式决策表：普通短任务、CPU/GPU 双路径、无锁低延迟、实时队列、Blocking I/O worker。
- [x] 说明自动路由不能验证 callable 的实时安全、线程安全、GPU 内存所有权或 I/O 可中断性。
- [x] 记录发布说明和版本兼容策略；带返回值的 CPU/GPU 自动任务和 `ExecutionReport<T>` 仅列为后续主版本评估项。

### 最终验收

- [x] 新用户可只使用 `Executor` 和 `submit_auto(lambda)` 完成默认异步任务，并获得可解释的默认路由决策。
- [x] 专家用户仍可调用全部既有显式 API，且行为与回归测试基线一致。
- [x] 所有后端不可用、拒绝、回退、任务异常和背压状态至少通过 future/返回值、路由决策、failure event 或状态计数之一可观察。
- [ ] ⏸ 门控（CUDA 主机）：全量测试、禁用 GPU 构建和启用 GPU 构建均通过；新增测试不依赖实际 GPU 才能验证不可用/回退路径。
  （CI 已有三路 EXECUTOR_ENABLE_GPU=OFF job + gpu-headers-build stub 编译任务；
  真实 GPU 运行验证待 CUDA 主机，2026-09-22 注记）
