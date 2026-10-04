# 统一 Facade 与自动路由设计


> [!NOTE]
> 历史设计快照（0.3.1 统一 facade 与意图路由）。§8.2 中"legacy CPU/GPU overload 移除"已由 0.6.0 兑现；0.6.0 的调度演进见 scheduling_runtime.md。

## 1. 背景

`Executor` 已经是项目的主要 facade，但其统一程度仍以“创建、注册和停止执行器”为主。用户在提交工作时仍需要理解多套入口：

- 默认异步线程池使用 `submit()`、`submit_priority()`、延迟和周期 API；
- 实时执行器使用注册、启动和 `push_realtime_task()`；
- Blocking I/O 使用专属 worker 的注册、启动和停止 API；
- `LockFreeTaskExecutor` 是独立对象，不受 `ExecutorManager` 管理；
- GPU 使用 `register_gpu_executor()` 与 `submit_gpu()`；
- 当前 `submit_auto()` 仅在 CPU 线程池和指定 GPU 之间选择。

当前 CPU/GPU 自动选择基于 `gpu::TaskCharacteristics` 和 `GpuScheduler` 的阈值/历史数据。它在选择 GPU 后直接调用 `submit_gpu()`，GPU 未注册或不可用时会提交失败；CPU 路径则调用 `kernel(nullptr)`。因此一个 callable 必须同时表达 CPU 和 GPU 两种执行环境，且选择结果不能反映后端是否可用、是否拥塞或是否发生降级。

本设计将 facade 演进为一个**按任务意图路由、行为可解释、保留执行器真实语义**的统一入口。它不试图把所有执行模型伪装成同一个线程池。

## 2. 目标与非目标

### 2.1 目标

1. 新用户可以将一般短任务提交到 `submit_auto()`，不需要首先理解内部执行器。
2. 用户以少量、显式的语义提示表达资源和时延意图；路由器不分析 lambda 内容来猜测阻塞、实时性或 GPU 能力。
3. 现有显式 API 保持可用且语义不变；自动路由是增量能力，不替代专家控制。
4. 每次自动决策、回退和拒绝均可查询或订阅，不能形成生产环境黑盒。
5. 将可安全统一的“单次任务”体验先覆盖 CPU/GPU，随后覆盖无锁单消费者路径；实时队列和 Blocking I/O 统一为同一管理与诊断模型，但保留其不同的提交/生命周期结果。
6. 保持无 GPU、无实时权限、或未配置可选后端时的安全降级和清晰错误。

### 2.2 非目标

1. 不自动推断 callable 是否包含阻塞 I/O、系统调用、锁、硬 deadline 或 GPU kernel。
2. 不将 Blocking I/O 的长期 worker 转换成一次性 `std::future<T>` 任务。
3. 不将实时队列的“有界、可拒绝、下一个周期执行”承诺伪装成线程池的“已接受任务最终兑现 future”。
4. 不在首个阶段实现跨执行器抢占、迁移正在运行的任务，或全局任务取消。
5. 不因自动路由而突破实时线程、GPU 内存或用户数据线程安全边界。

## 3. 现有执行模型与不可抹平的语义

| 模型 | 当前入口 | 工作单位 | 接收结果 | 完成语义 | 自动路由定位 |
| --- | --- | --- | --- | --- | --- |
| 异步线程池 | `submit()` | 一次性 callable | `future<T>` | 已接受工作成功、异常或软超时会兑现 future | 默认后端 |
| GPU | `submit_gpu()` | kernel/callback + `GpuTaskConfig` | `future<void>` | GPU 后端的完成/失败 | CPU/GPU 自动选择 |
| 无锁任务执行器 | `LockFreeTaskExecutor::push_task()` | MPSC 投递、单消费者处理 | `bool` | 仅表示进入有界队列；异常由统计/handler 可见 | 显式低延迟候选 |
| 实时线程 | `push_realtime_task()` | 在下一个周期消费的有界队列项 | `bool` | 可因未启动、队列满或对象池耗尽而丢弃 | 不隐式选择 |
| Blocking I/O | `register_blocking_io_worker()` | 专属、可唤醒的长期 worker | `ExecutorResult` / 状态 | 启动、停止、退出原因 | 不作为单次任务后端 |

统一 facade 的对象不是“把结果都改成 future”，而是让每个模型共享：命名、配置、能力发现、路由诊断、故障事件和生命周期管理。

## 4. 核心模型

### 4.1 任务意图

新增公开的任务描述类型，建议定义在 `include/executor/task_options.hpp`，并由 `executor.hpp` 聚合包含。

```cpp
enum class ExecutionIntent : uint8_t {
    Auto,
    GeneralCpu,
    CpuOrGpu,
    LowLatency,
    RealtimeQueue,
    BlockingWorker
};

enum class FallbackPolicy : uint8_t {
    NoFallback,
    AllowCpu,
    RequireRequestedBackend
};

struct TaskOptions {
    std::string name;
    TaskPriority priority = TaskPriority::NORMAL;
    ExecutionIntent intent = ExecutionIntent::Auto;
    std::optional<std::string> preferred_executor;
    FallbackPolicy fallback = FallbackPolicy::NoFallback;
    std::optional<std::chrono::steady_clock::time_point> deadline;
};
```

`deadline` 在第一阶段只作为路由和诊断提示，不承诺中断正在运行的任务。线程池已存在的 `task_timeout_ms` 仍是“开始执行前的软超时”，二者不得混淆。

为降低入门门槛，提供不可变或按值返回的 builder；普通 lambda 不要求包装：

```cpp
auto future = executor.submit_auto([] { return parse(message); });

auto future = executor.submit_auto(
    executor::task([] { return transform(frame); })
        .name("frame-transform")
        .priority(TaskPriority::HIGH));
```

无选项的 `submit_auto(F&&, Args&&...)` 等价于 `intent = Auto`，而 `Auto` 在首个版本只能选择默认异步线程池，除非任务对象明确提供 CPU/GPU 双路径。

### 4.2 CPU/GPU 双路径任务

废除“CPU 分支用 `nullptr` stream 调 GPU callable”的设计方向。新增明确的双路径描述：

```cpp
template<class CpuFunction, class GpuFunction>
class CpuGpuTask;

template<class CpuFunction, class GpuFunction>
auto cpu_gpu_task(CpuFunction&& cpu, GpuFunction&& gpu);
```

示例：

```cpp
auto work = executor::cpu_gpu_task(
    [input] { run_cpu(input); },
    [input](void* stream) { run_gpu(input, stream); })
    .name("segmentation")
    .data_size(input.bytes())
    .compute_intensity(3.5F)
    .preferred_executor("cuda0")
    .fallback(FallbackPolicy::AllowCpu);

auto result = executor.submit_auto(std::move(work));
```

两个 callable 可以拥有不同签名；为保持统一，第一阶段将 CPU/GPU 自动任务限制为 `void` 返回值。带返回值的 CPU/GPU 自动任务在后续阶段通过显式 `ResultAdapter<T>` 支持，避免 GPU callback 的返回值和设备同步语义被草率定义。

`NoFallback` 允许 scheduler 在可用的 CPU/GPU 候选间选择，但被选中的 GPU 后端不可提交时直接拒绝；`AllowCpu` 允许此情况改走 CPU；`RequireRequestedBackend` 则要求 `preferred_executor` 可提交并跳过 CPU/GPU 启发式选择。

保留现有四参数 `submit_auto(TaskCharacteristics, name, kernel, GpuTaskConfig)`，但在文档中标记为 legacy CPU/GPU overload：

- `0.3.x` 不改变其现有无隐式回退行为；
- 新 API 成熟后标记 `[[deprecated]]`，迁移到双路径任务；
- 只在下一个允许破坏性 API 的主版本移除。

### 4.3 路由请求、结果与解释

路由前先生成值类型请求，路由结果必须独立于实际提交结果保存：

```cpp
enum class ExecutionBackend : uint8_t {
    DefaultAsync,
    Gpu,
    LockFree,
    Realtime,
    BlockingIo
};

enum class RoutingReason : uint8_t {
    DefaultPolicy,
    ExplicitIntent,
    PreferredExecutor,
    GpuHeuristic,
    AdaptiveHistory,
    BackendUnavailable,
    BackendNotRunning,
    CapacityPressure,
    FallbackPolicy,
    Rejected
};

struct RoutingDecision {
    std::string task_name;
    ExecutionIntent requested_intent;
    ExecutionBackend selected_backend;
    std::string selected_executor_name;
    RoutingReason reason;
    bool fell_back = false;
    std::string detail;
    std::chrono::steady_clock::time_point timestamp;
};
```

提供：

```cpp
std::optional<RoutingDecision> get_last_routing_decision() const;
std::vector<RoutingDecision> get_recent_routing_decisions(size_t max_count = 0) const;
void set_routing_callback(std::function<void(const RoutingDecision&)> callback);
```

回调必须与现有 failure callback 一样隔离异常。提交拒绝、任务异常和超时仍进入现有 `ExecutorFailureEvent`；`RoutingDecision` 说明“为什么走此后端”，不是替代失败诊断。

## 5. 路由策略

### 5.1 第一阶段规则

1. 普通 callable 或 `GeneralCpu`：提交默认异步线程池。
2. `CpuOrGpu`：候选为请求的 GPU 执行器和默认异步线程池。
3. `LowLatency`、`RealtimeQueue`、`BlockingWorker`：若通过泛型 `submit_auto` 提交，明确拒绝，并说明应使用对应 typed API；不静默转到线程池。
4. 显式目标名称优先于启发式，但目标不存在、未运行或不支持任务形态时按 `FallbackPolicy` 决定回退或拒绝。
5. `GpuScheduler` 只在 GPU 处于可提交状态时参与选择。未注册、未运行或后端错误的 GPU 不能被 heuristic 选中。

CPU/GPU 决策顺序：

```text
显式 RequireRequestedBackend
        │
        ├─ 后端可用 → 使用指定后端
        └─ 不可用 → 拒绝

显式 preferred executor / CPU-GPU task
        │
        ├─ GPU 可用 → GpuScheduler（偏好、历史、阈值）
        │                  │
        │                  ├─ GPU → 提交 GPU
        │                  └─ CPU → 提交默认异步池
        └─ GPU 不可用 → AllowCpu 则 CPU；否则拒绝
```

GPU “可用”至少表示：已注册、`is_running`、没有阻止新提交的后端错误，并且队列未达到配置的硬上限。`queue_size` 只是快照，不能作为容量保证；实际提交仍必须处理拒绝。

### 5.2 第二阶段：无锁后端

将 `LockFreeTaskExecutor` 注册到 `ExecutorManager`，但不强行继承 `IAsyncExecutor`。它需要独立的能力接口，原因是它没有 `future` 完成语义，也不是多消费者线程池。

```cpp
struct DispatchResult {
    bool accepted = false;
    ExecutionBackend backend = ExecutionBackend::LockFree;
    std::string executor_name;
    RoutingDecision decision;
    std::string message;
};

DispatchResult dispatch_auto(TaskOptions options, std::function<void()> task);
```

`dispatch_auto()` 是 fire-and-forget/有界队列 API；它适合 `LowLatency` 和将来的 `RealtimeQueue`，而 `submit_auto()` 继续只承诺 future 型后端。这样调用方在类型层面看得到“已接收”与“已完成”的区别。

无锁路由必须是 opt-in：`LowLatency` 明确指定的执行器运行中且接受任务时才投递。队列满、对象池耗尽或停止都返回 `accepted = false`，并同时记录路由和 failure 事件。不能把它作为普通 `Auto` 的优化，因为单消费者/有界背压是业务语义而非纯性能参数。

### 5.3 实时与 Blocking I/O

实时路径将来可支持 `dispatch_auto(... RealtimeQueue ...)`，但仅选择用户指定、已经启动的实时执行器；不得由 deadline、priority 或 queue pressure 自动推断。

Blocking I/O 不进入 `submit_auto()` 或 `dispatch_auto()`。为统一用户体验，可引入：

```cpp
struct WorkerHandle {
    std::string name;
    ExecutorResult start_result;
    void request_stop() noexcept;
    void stop();
    BlockingIoExecutorStatus status() const;
};

WorkerHandle start_worker(BlockingWorkerSpec spec);
```

它包装当前注册/启动/停止流程，保留 `IBlockingIoWorker::wakeup()`、`stop_token` 和启动超时的现有契约。实时 thread 也可在后续提供类似的 `RealtimeHandle`，但两者不能共享一个假想的“任务完成 handle”。

## 6. 内部架构

### 6.1 能力注册表

`ExecutorManager` 增加内部、非多态提交接口的能力快照：

```cpp
struct ExecutorCapability {
    ExecutionBackend backend;
    std::string name;
    bool registered;
    bool running;
    bool supports_future_submission;
    bool supports_bounded_dispatch;
    bool supports_gpu_kernel;
    size_t pending_work;
    size_t capacity_hint;
};
```

该结构只用于路由预检和诊断，不能取代真实提交路径的原子性。Manager 负责所有已注册后端的名称唯一性，当前跨 realtime/Blocking I/O/GPU 的重复检查也应扩展到无锁注册表。

### 6.2 路由器

新增内部 `TaskRouter`，由 `Executor` 拥有。它只接收不可变请求和 capability snapshot，输出 `RoutingDecision`；实际投递仍由 facade 调用具体执行器，以避免将五种提交协议塞入一个脆弱的虚接口。

职责：

- 验证 intent 与任务形态是否兼容；
- 获取候选后端的快照；
- 应用显式目标、fallback、GPU scheduler 和容量策略；
- 生成可解释的决定；
- 不执行用户任务，不拥有 GPU/worker，不阻塞等待。

`GpuScheduler` 保持为 CPU/GPU 子策略，不扩展为“万能调度器”。其 `PerformanceRecord` 在后续可加入 `executor_name`、端到端耗时和样本有效性；第一阶段不自动记录时间，避免把排队、传输、同步和业务准备时间混为 kernel 性能。

### 6.3 结果类型边界

| API | 适用后端 | 返回类型 | 表示什么 |
| --- | --- | --- | --- |
| `submit_auto` | 默认异步、CPU/GPU | `future<T>` / `future<void>` | 工作已被接受后的完成或异常 |
| `dispatch_auto` | 无锁、未来实时 | `DispatchResult` | 一次有界投递是否被接受 |
| `start_worker` | Blocking I/O | `WorkerHandle` | 专属 worker 的生命周期控制 |
| 现有显式 API | 全部 | 保持不变 | 专家控制与兼容入口 |

## 7. 可观测性与失败策略

1. 每次 `submit_auto` / `dispatch_auto` 必须产生一个 `RoutingDecision`，包括默认线程池选择。
2. 因后端不可用而发生的允许回退使用 `fell_back = true` 和 `RoutingReason::FallbackPolicy`；可选记录 `FailureKind::TuningFallback`，但不计为用户任务失败。
3. `FallbackPolicy::NoFallback` 或 `RequireRequestedBackend` 的失败是明确拒绝，记录 `FailureKind::SubmitRejected`；GPU 后端错误额外记录 `GpuFailure`。
4. 后端接受后发生的任务异常、线程池软超时、实时 drop 和 worker 异常继续通过现有状态和 failure event 报告。
5. 不将“队列长度低”解释为“低延迟”；所有 queue/active 字段均是监控快照。
6. 路由决策缓冲默认容量应独立于 failure buffer，建议为 128，可设为 0 来关闭保留但不关闭 callback。

## 8. 兼容与发布策略

### 8.1 `0.3.x` 增量版本

- 添加任务描述、双路径 CPU/GPU task、路由决策 API 和 `TaskRouter`；
- 保留 `submit()`、`submit_gpu()`、现有 `submit_auto()`、实时和 Blocking I/O API；
- 现有 `submit_auto` 保持“GPU 未就绪即失败”的既有语义；
- 新 API 默认 `FallbackPolicy::NoFallback`，防止升级后悄悄改变业务后端；
- 所有新类型只添加字段/新 API，不修改现有公开 struct 的含义。

### 8.2 下一个主版本

- 在足够迁移期后 deprecate/移除 legacy CPU/GPU overload；
- 若实践证明类型系统足够稳定，再考虑泛型 `CpuGpuTask<T>`；
- 评估以 `ExecutionReport<T>` 取代裸 `future<T>` 的新 API，但不得改变旧 `submit()` 返回类型。

## 9. 实施里程碑与验收

### M1：安全的 CPU/GPU 自动提交

实现：`TaskOptions`、`CpuGpuTask<void>`、`FallbackPolicy`、capability snapshot、`TaskRouter`、routing buffer/callback。

验收：

- 普通 `submit_auto(lambda)` 与 `submit(lambda)` 一样进入默认线程池并返回 future；
- GPU 未注册时，`AllowCpu` 明确回退并可查询原因；`NoFallback` 明确拒绝；
- GPU 已注册但未运行、队列满、提交拒绝时不产生未兑现 future；
- legacy overload 行为和现有测试保持不变；
- callback 抛异常不影响 worker、路由或 future。

### M2：统一能力发现与无锁 dispatch

实现：Manager 的无锁执行器注册、`DispatchResult`、`dispatch_auto()`、名称冲突检查、无锁状态适配。

验收：

- `LowLatency` 只有显式指定/配置候选时才选择无锁后端；
- 队列满、停止和空任务均返回未接受并保留原因；
- 不为无锁执行器伪造“future 完成”承诺；
- shutdown 以确定顺序停止所有 Manager 所有的已注册后端。

### M3：实时/worker 统一控制面

实现：`WorkerHandle`、可选 `RealtimeHandle`、统一列表/状态快照和文档决策树。

验收：

- 长期 worker 的 wakeup、stop token、启动超时语义不变；
- 实时投递仍通过有界队列，并明确显示 drop/backpressure；
- 用户能在一个 facade 状态 API 中发现所有注册后端，但不会误以为 `wait_for_completion()` 等待全部后台活动。

## 10. 测试矩阵

| 场景 | 关键断言 |
| --- | --- |
| 无配置新用户 | `submit_auto(lambda)` 懒初始化默认线程池，future 正确兑现 |
| GPU 可用且命中阈值 | 选择 GPU，决策原因是 heuristic 或 history |
| GPU 不可用 + AllowCpu | 选择 CPU，`fell_back` 为真，不记录任务失败 |
| GPU 不可用 + NoFallback | 拒绝可观察，future 就绪异常或 `_ex` 失败 |
| 指定 GPU 已停止 | 不由 scheduler 假设可用，按 fallback 处理 |
| GPU 提交竞争/队列满 | 实际拒绝不会让调用方永久等待 |
| 无锁已停止/队列满 | `DispatchResult.accepted == false`，计数一致 |
| 实时未启动 | 不隐式改投线程池，返回明确拒绝 |
| Blocking worker 启动超时 | `WorkerHandle` 保留 `StartFailed` 与错误详情 |
| 路由 callback 抛异常 | 提交、执行器和 failure buffer 正常工作 |
| shutdown 并发提交 | 不出现悬空执行器指针、双重兑现或丢失诊断 |

## 11. 文档与引导

新手文档应以“默认任务使用 `submit_auto`，需要时再说明意图”为主线，而不是首先列出五种后端。高级页面提供选择表：

- 计算量普通且短：默认自动/线程池；
- 同一业务有独立 CPU 与 GPU 实现：CPU/GPU task；
- 已验证的 MPSC 单消费者低延迟路径：无锁 dispatch；
- 已有周期控制和明确背压策略：实时队列；
- 可中断的长期 read/poll/handle 循环：Blocking I/O worker。

所有页面必须强调：自动路由依据用户声明的意图和可观测状态作决定，不能证明 callable 的实时安全性、线程安全性、GPU 内存所有权或 I/O 可中断性。
