# 任务协作取消与定时句柄设计


> [!NOTE]
> 历史设计快照（0.5.0 定时句柄设计）。0.6.0 起 `submit_delayed`/`submit_periodic` 直接返回句柄（`_with_handle` 拼写已消失），字符串 ID 体系已移除。

## 1. 文档状态

- 状态：C0 设计提案，先行评审
- 输入：[客户端反馈缺口收敛计划](../archive/todolists/client_feedback_update_plan.md)、
  `include/executor/stop_token.hpp`、`Executor` facade 当前任务图和定时器实现
- 目标阶段：C1（任务级协作取消）和 T1（通用定时句柄）
- 条件阶段：T2/S2（外部序列化上下文绑定）

### 1.1 先行评审结论

**有条件通过 C0；在共享 cancellation state、future 终态、token overload 和句柄所有权
四项决策冻结前，不启动 C1/T1 的代码实现。**

2026-08-29 更新：第 11.2 节全部待决项已冻结（结论见该节），C0 完成，C1/T1 进入
实现阶段。

本设计不承诺抢占式中断。它只提供排队任务的安全取消，以及运行中任务可观察、可协作的
停止请求。阻塞在没有 wakeup 机制的第三方调用上，仍必须由调用方解除阻塞。

## 2. 范围与非目标

### 2.1 一期范围

C1/T1 只覆盖 `Executor` facade 的：

- 普通异步任务和 priority 任务；
- `submit_with_handle`、依赖图任务及其 future；
- facade 的 delayed/periodic 定时任务。

realtime、lockfree、GPU 和 Blocking I/O 保留各自已有的 stop、drop 或 worker 生命周期
协议，不因本设计自动获得统一的任务取消 API。Blocking I/O 继续使用已有的
`IBlockingIoWorker::run(executor::StopToken)` 和 `wakeup()` 契约。

### 2.2 非目标

- 不强制终止线程、函数或系统调用；
- 不把 `TaskOptions::deadline` 变成中断机制；
- 不在核心库引入 asio 或其他外部事件循环依赖；
- T1 不提供 asio strand 所有权或同 strand 销毁保证；
- 不修改现有 `submit()` / `submit_with_handle()` 的返回类型和无 token 调用形式；
- 不保存无限的句柄、任务历史或 callable/payload。

需要在指定 strand 上执行和销毁的 timer，继续由应用侧管理，直到 S2/T2 通过验收。

## 3. 现有实现约束

1. `Task` 当前包含 `std::atomic<bool> cancelled`，但 scheduler、本地队列和 steal 路径会
   复制 `Task` 字段。取消状态不能依赖该原子字段的按值传播。
2. 普通 `submit()` 只有 future，没有公共句柄；没有句柄就不能安全地定位单个任务并请求取消。
3. `submit_with_handle()` 已有 `TaskHandle`，但 handle 目前只有字符串 id，任务图有独立的
   retention 逻辑。
4. `submit_delayed()` 当前只返回 future；`submit_periodic()` 返回字符串 id，
   `cancel_task()` 只管理 periodic 表。
5. timer thread 停止时会移出 pending delayed task，并通过 `on_rejected` 满足 future；新的
   句柄协议必须把这条路径改成定义明确的取消终态，不能留下永久等待。
6. Android fallback `StopToken` 当前提供 `stop_requested()`，没有标准库完整的 callback
   注册接口。若公共契约承诺 stop callback，必须先补齐 fallback 实现和测试；否则一期跨平台
   契约只承诺 polling。

## 4. 核心状态模型

### 4.1 cancellation state

每个可取消或带 handle 的纳管任务创建一个独立的 `StopSource`。`StopSource` 产生的
stop state 由一个内部共享控制对象持有，建议形态如下：

```cpp
enum class TaskControlPhase {
    Pending,
    Queued,
    Running,
    Succeeded,
    Failed,
    TimedOut,
    Rejected,
    Cancelled
};

struct TaskCancellationState {
    StopSource stop_source;                // 每个任务独立创建
    std::atomic<TaskControlPhase> phase;
    std::atomic<bool> cancel_requested;
};
```

实际实现可以使用等价的不可变或锁保护结构，但必须满足：

- scheduler、本地队列、steal、执行包装和 handle registry 只复制
  `std::shared_ptr<TaskCancellationState>`，不能复制一份独立取消标志；
- registry 的 active entry 持有 state，任务副本和 callable wrapper 也可短暂持有 state；
- 终态后 registry 只保留有界的终态元数据，不保留 callable 或业务 payload；
- state 的生命周期不依赖 `TaskHandle` 对象本身，handle 可以按值复制。

state 还需要一个只负责满足 promise 的类型擦除 cancellation completion sink。取消方赢得
`Cancelled` 终态后立即、且只调用一次该 sink，使 future 不依赖 worker 何时再次取到已取消
的队列节点。sink 可以持有 promise state，但不得持有用户 callable 或业务 payload；sink
异常必须隔离，不能破坏取消状态机。

`StopSource` 是每任务一个；“共享”只描述其 cancellation state 在各任务副本、句柄和
registry 之间的传播。

### 4.2 控制阶段与线性化点

任务控制阶段不是 `TaskLifecycleState` 的替代品，而是取消实现的内部并发状态：

| 控制阶段 | 含义 |
| --- | --- |
| `Pending` | 已分配 handle/state，尚未进入线程池队列或依赖仍未满足 |
| `Queued` | 已被线程池接受，callable 尚未开始 |
| `Running` | worker 已取得任务并声明开始执行 |
| `Succeeded`/`Failed`/`TimedOut`/`Rejected`/`Cancelled` | 具体终态，不能再次执行 |

取消和执行开始之间必须有单一线性化点：

- `Pending`/`Queued` 到 `Running` 的 CAS 成功表示 worker 赢得开始执行；
- 取消在该 CAS 前成功，则任务转为 `Cancelled`，worker 不得调用 callable；
- 取消在该 CAS 后到达，则只置位 `StopSource`，任务进入运行中协作取消路径；
- 终态转换只能成功一次，cancel、soft timeout、shutdown 和 worker 完成必须通过同一
  控制状态仲裁，不得出现 future 被满足两次或任务执行两次。

取消请求应先调用 `request_stop()`，再根据控制阶段返回结果。重复请求必须是幂等的，不能
重新执行任务或重复增加“首次请求”计数。

### 4.3 与诊断生命周期的关系

`TaskLifecycleState::Cancelled` 只表示任务没有执行 callable，或 callable 明确以
`TaskCancelled` 终止。运行中任务收到停止请求后仍正常返回时，其任务终态可以是
`Succeeded`/`Failed`；停止请求通过独立取消计数观察，不强行改写业务结果。

### 4.4 线程池内部提交边界

现有 `IAsyncExecutor::try_submit_task(std::function<void()>)` 无法把 task id 和共享 state 传到
worker。C1 必须新增内部受控提交 envelope（具体命名可调整）：

```cpp
struct ControlledTaskSubmission {
    std::string task_id;
    std::function<void()> function;
    std::function<void(std::exception_ptr)> on_timeout;
    std::shared_ptr<TaskCancellationState> cancellation;
};
```

`IAsyncExecutor`/`ThreadPoolExecutor` 增加仅供 facade 使用的受控提交入口，最终将同一个 state
写入内部 `Task`。不能只在 facade closure 外包一层取消检查，因为 worker 的 soft timeout、
本地队列和 steal 路径必须参与同一终态 CAS 与完成计数。

直接使用 `IAsyncExecutor` 的既有公开提交方法保持不变；它们没有 `TaskHandle`，不进入本期
按句柄取消范围。受控入口必须使用 facade 的 task id，禁止线程池再次生成无法关联的第二套 id。

## 5. 取消语义

### 5.1 排队中取消

排队中包括 `Pending`、`Queued` 和依赖未满足的任务：

1. 取消赢得线性化点，任务不调用 callable；
2. future 以 `TaskCancelled` 异常就绪，`future.get()` 不会永久等待；
3. `TaskMonitor` 记录 `TaskLifecycleState::Cancelled`；
4. 不产生 `ExecutorFailureEvent`，不增加 `ExecutorFailureStatus`；
5. 被取消的依赖会使依赖方不执行，并以 `TaskCancelled`（reason =
   `DependencyCancelled`）满足其 future；
6. `when_all`/依赖图聚合在任一必要依赖取消时进入取消终态，不把取消伪装为任务异常。

取消依赖阻塞任务时必须通知 `task_graph_cv_`；依赖等待谓词同时检查自身 cancellation state，
保证取消不会等到其他依赖完成后才生效，也不会永久占用 worker。

### 5.2 运行中取消

运行中取消是请求而不是中断：

- 对接收 `StopToken` 的 callable，executor 置位 token，callable 负责轮询
  `stop_requested()` 或使用已经明确支持的 stop callback；
- callable 正常返回时，future 保留其正常返回值，任务可记为 `Succeeded`；
- callable 抛出 `TaskCancelled` 时，future 重新抛出该异常，任务记为 `Cancelled`，不触发
  failure callback；
- 只有该任务的 stop state 已被请求时，`TaskCancelled` 才按取消归类；用户在无取消请求时主动
  抛出该异常仍按任务异常处理，避免用异常类型绕过 failure 统计；
- 不接收 token 的 callable 仍可被排队取消，但运行后只能记录“已请求”，不能承诺停止；
- executor 不等待 callable 因取消而提前返回。关闭时是否等待仍由既有 `shutdown(bool)`
  语义决定。

### 5.3 与 soft timeout、deadline 的关系

- `task_timeout_ms` 是 worker 开始执行前的 queued soft timeout；它不打断运行中的任务。
- worker 在开始执行前先按控制状态仲裁取消和 timeout：谁先成功完成终态转换谁生效，另一方
  只能观察到已终止状态；两者不能同时计数为终态。
- `TaskOptions::deadline` 继续是路由/诊断提示，不自动调用 `request_task_cancel()`。
- 显式取消、排队 soft timeout 和 advisory deadline 必须在文档、状态字段和测试中区分。

## 6. Future、结果与公共 API 草案

### 6.1 Future 终态异常

新增一个不属于 failure 体系的异常类型：

```cpp
enum class TaskCancellationReason {
    Explicit,
    Shutdown,
    DependencyCancelled
};

class TaskCancelled : public std::runtime_error {
public:
    TaskCancelled(TaskCancellationReason reason, std::string message);
    TaskCancellationReason reason() const noexcept;
};
```

排队取消、shutdown 清理和取消传播都使用 `TaskCancelled`，通过 `reason()` 区分来源。
已有提交被拒、任务异常和 queued soft timeout 继续使用现有异常和 failure 语义。

不采用 broken promise：它无法表达取消来源，且容易把生命周期决策误诊为 promise 所有者异常
退出。也不采用返回空值：它不能覆盖 `void` 和任意返回类型，且会改变既有 future 类型。

### 6.2 取消请求结果

建议新增值类型，不复用 `ExecutorResult`，因为重复或过期取消是可预期的并发结果，而不是
初始化类错误：

```cpp
enum class TaskCancellationResult {
    RequestedBeforeStart,
    RequestedRunning,
    AlreadyRequested,
    AlreadyCompleted,
    NotFound,
    ShuttingDown
};

struct TaskCancellationResponse {
    TaskCancellationResult result = TaskCancellationResult::NotFound;

    bool accepted() const noexcept {
        return result == TaskCancellationResult::RequestedBeforeStart ||
               result == TaskCancellationResult::RequestedRunning ||
               result == TaskCancellationResult::AlreadyRequested;
    }
};
```

建议 facade API：

```cpp
TaskCancellationResponse request_task_cancel(const TaskHandle& handle) noexcept;
CancellationStatus get_cancellation_status() const;
```

`submit()` 保持不变且不新增隐式可取消入口。已有 `submit_with_handle()` 和
`submit_after_with_handle()` 建立共享 state，因此可实现排队取消；新增显式 token API 用于
运行中协作取消：

```cpp
template <typename F, typename... Args>
auto submit_cancellable(F&& f, Args&&... args)
    -> TaskSubmission<std::invoke_result_t<F, StopToken, Args...>>;

template <typename F, typename... Args>
auto submit_cancellable_priority(int priority, F&& f, Args&&... args)
    -> TaskSubmission<std::invoke_result_t<F, StopToken, Args...>>;
```

token 固定作为 callable 的第一个参数，由 executor 注入；调用方传给该 API 的 `Args...`
不包含 token。首期不采用对既有 `submit()` 的自动 token 检测，避免泛型 lambda、重载 callable
和 `std::invoke_result` 推导发生静默行为变化。依赖图提供等价的显式
`submit_cancellable_after*` overload，或在 C1 实现前证明一个不增加歧义的统一包装。

### 6.3 cancellation status

取消不进入 `FailureKind`。建议新增独立快照类型：

```cpp
struct CancellationStatus {
    uint64_t request_count = 0;                 // 每个任务首次被接受的请求
    uint64_t queued_cancelled_count = 0;        // 未调用 callable 的任务
    uint64_t running_request_count = 0;         // 运行中收到请求的任务
    uint64_t completed_after_request_count = 0; // 请求后仍正常完成的任务
};
```

`request_count`、`queued_cancelled_count` 和 `running_request_count` 只在同一任务的首次
相应事件发生时递增。`CancellationStatus` 应通过 `get_cancellation_status()` 提供，并
作为 `ExecutorSnapshot` 的独立字段；不并入 `ExecutorFailureStatus::total_count`。

## 7. TimerHandle 设计

### 7.1 T1 的能力边界

T1 的 timer 由 facade timer thread 管理，到期后提交到已选择的普通 async executor。它不
绑定 asio strand，也不改变外部 context 的线程或销毁语义。T1 可迁移不依赖外部 strand
所有权的 delayed/periodic 工作；node/relay 中需要在同一 strand 访问对象并销毁 timer 的
工作，必须等待 T2/S2。

### 7.2 句柄所有权

普通 `TimerHandle` 是可复制的控制句柄：

- 复制只增加控制对象引用，不复制 timer；
- 析构不取消；
- `cancel()`/`reschedule_after()` 是非阻塞请求；
- handle 不持有 callable 或业务对象的裸指针。
- handle 只持有 id 和可安全失效的控制锚点；Executor/registry 析构后调用返回 `NotFound` 或
  `ShuttingDown`，不得通过保存的裸 `Executor*` 访问已销毁对象。

需要析构即取消时使用单独的 move-only `ScopedTimerHandle`：

- 只能移动，不能复制；
- 析构调用一次非阻塞 `cancel()`；
- 析构不等待正在运行的 callback，也不保证 callback 不再访问业务对象；
- 临时创建的普通 `TimerHandle` 不会因表达式结束而意外取消。

### 7.3 建议 API

```cpp
enum class TimerState {
    Scheduled,
    Completed,
    Cancelled,
    ShutdownCancelled,
    Failed
};

enum class TimerOperationResult {
    CancelledBeforeDispatch,
    CancellationRequestedAfterDispatch,
    Rescheduled,
    AlreadyCancelled,
    AlreadyCompleted,
    NotFound,
    ShuttingDown,
    InvalidDuration
};

struct TimerStatus {
    std::string timer_id;
    TimerState state = TimerState::Scheduled;
    bool periodic = false;
    uint64_t execution_count = 0;
    uint64_t active_callback_count = 0;
    uint64_t cancellation_count = 0;
    std::chrono::steady_clock::time_point next_execute_time{};
};

class TimerHandle {
public:
    bool valid() const noexcept;
    TimerOperationResult cancel() noexcept;
    TimerOperationResult reschedule_after(int64_t delay_ms) noexcept;
    std::optional<TimerStatus> status() const;
};

template <typename T>
struct TimerSubmission {
    TimerHandle handle;
    std::future<T> future;
};

template <typename F, typename... Args>
auto submit_delayed_with_handle(int64_t delay_ms, F&& f, Args&&... args)
    -> TimerSubmission<std::invoke_result_t<F, Args...>>;

template <typename F, typename... Args>
auto submit_delayed_cancellable_with_handle(int64_t delay_ms, F&& f, Args&&... args)
    -> TimerSubmission<std::invoke_result_t<F, StopToken, Args...>>;

TimerHandle submit_periodic_with_handle(
    int64_t period_ms, std::function<void()> task);

TimerHandle submit_periodic_cancellable_with_handle(
    int64_t period_ms, std::function<void(StopToken)> task);
```

实际公开命名可在 C1/T1 实现前调整，但必须保留上述语义。`submit_delayed()` 和
`submit_periodic()` 的旧签名继续存在。

### 7.4 内部索引与重排

现有 delayed timer 使用 `std::priority_queue`，不能高效删除或就地重排任意节点。T1 采用
registry + generation heap：

```cpp
struct TimerRecord {
    std::string timer_id;
    uint64_t generation = 0;
    TimerState state = TimerState::Scheduled;
    // callable、下一次到期、周期和共享 task state 由 registry 拥有。
};

struct TimerHeapEntry {
    std::chrono::steady_clock::time_point deadline;
    std::string timer_id;
    uint64_t generation = 0;
};
```

- `reschedule_after()` 在 registry 锁内递增 generation、更新到期时间并压入新 heap entry；
- timer thread 弹出 entry 后核对 registry 中的 id、generation 和 state，旧 generation
  视为 stale 并丢弃；
- cancel 从 registry 移除 callable/业务捕获，heap 中的 stale entry 不再拥有 callable；
- 重复重排产生的 stale entry 必须有压缩阈值或容量上限，不能长期重排导致 heap 无界增长；
- timer thread 的等待采用 steady 时钟 1ms 分片休眠：每次醒来重新加锁检查 heap 顶，
  新增更早到期、重排或 shutdown 的可见性上界为 1ms，远优于旧实现固定的 10ms 轮询；
  到期精度不受影响——最后一个分片精确睡到 heap 顶 deadline。
  实现上刻意不使用 `std::condition_variable` 做定时等待：libstdc++ 把
  `wait_until(steady_clock)` 映射到 `pthread_cond_clockwait`，gcc-11 时代的 libtsan
  未拦截该原语，会在醒来重锁时误报 "double lock of a mutex"（gcc PR101978 /
  google/sanitizers#1259）；分片休眠对 TSAN 与 MSVC 全环境行为可预测。

句柄的控制锚点与包含 callable 的 `TimerRecord` 分离。句柄可在 Executor 销毁后安全失效，
但不得延长 callable 或业务对象的生命周期。

### 7.5 Timer 竞争和 future 语义

- 对一次性 timer，从 `Scheduled` 取得当前 generation 并生成待提交 callback 是到期线性化点；
  在此之前 `cancel()` 会阻止提交，delayed future 以 `TaskCancelled(Explicit)` 就绪。
- 到期线性化点之后，`cancel()` 通过共享 task state 继续请求排队取消或运行中 StopToken，
  返回 `CancellationRequestedAfterDispatch`；不把“已派发”误报成“已执行”，也不承诺运行中
  callback 会停止。
- `reschedule_after()` 只允许对仍为 `Scheduled` 的一次性 timer，或对周期 timer 的下一次
  到期时间操作；已经 `Completed`/`Cancelled` 的 timer 不得重新排入。
- cancel 与到期竞争只能产生一次执行或一次取消终态；测试必须覆盖 timer thread、cancel
  调用方和 async worker 三方并发。
- periodic timer 在注册期间保持 `Scheduled`；每次 tick 以 generation 标识，
  `active_callback_count` 可大于 1，以保留当前允许 callback 重叠的兼容行为；
- periodic `cancel()` 原子地阻止生成后续 generation。已经提交到 async executor 的 tick
  保存在有界 active-state 集合中，cancel 对每个 active tick 请求排队或协作取消，但不撤回
  已取得执行权的 callback，也不等待它们完成；tick 终态后立即移出该集合。
- periodic `reschedule_after()` 只修改下一次到期时间，不改变 `period_ms`；若要修改周期，
  后续另设显式 API，不能让一个参数同时表达 next expiry 和 period。

### 7.6 shutdown

shutdown 停止 timer thread 后，所有仍为 `Scheduled` 的 delayed timer 进入
`ShutdownCancelled`，future 以 `TaskCancelled(Shutdown)` 就绪；pending periodic timer
停止后续 tick，不产生新的 failure event。已经完成到期线性化或提交到 async executor 的
callback 遵循既有 `shutdown(wait_for_tasks)` 等待语义。

旧 `cancel_task(task_id)` 保持兼容包装：成功取消不产生 failure event；对不存在的旧
periodic id 是否继续记录 `SubmitRejected` 必须在 C1 迁移测试中锁定，首选保留旧行为以避免
改变已有诊断契约。新 `TimerHandle::cancel()` 对过期句柄只返回明确结果，不写入 failure。

## 8. Android StopToken 兼容要求

- 桌面 `executor::StopToken` 继续是 `std::stop_token` 别名，`StopSource` 继续是
  `std::stop_source` 别名。
- Android fallback 必须保证可复制 token、每任务独立 state、`stop_requested()` 的
  acquire 读取和 `request_stop()` 的 release/acq_rel 发布与桌面语义一致。
- 若 C1 文档公开 stop callback，fallback 必须新增等价 callback 注册/注销和并发触发语义，
  并通过 forced-fallback 单测；在此之前文档只承诺 polling。
- fallback 的 `JThread` 不改变现有“析构 request_stop + join”语义；任务取消不复用
  `JThread` 的线程级 stop source。

## 9. Registry、监控和性能门槛

### 9.1 有界 registry

active registry 以 task id 定位共享 state。它必须满足：

- active entry 在任务终态后立即移出，不因调用方长期保存 handle 而继续持有 callable；
- 如需区分 `AlreadyCompleted` 与 `NotFound`，只保留 task id、终态和时间戳等有界 tombstone；
- tombstone 可复用 `task_graph_retention_capacity` 的策略，或新增独立容量，但不能无界增长；
- active registry 容量耗尽时，新的可取消提交必须明确拒绝，写入现有 submit rejection
  诊断，不能退化成“提交成功但无法取消”；
- `TaskHandle` id 与线程池内部 `Task::task_id` 应统一，避免一个提交产生两套无法关联的 id。

### 9.2 合入门槛

C1/T1 合入前必须提供：

1. cancellation state/active registry 的单任务内存增量和最大保留容量；
2. cancel 与开始执行的并发压力测试，至少覆盖 TSAN；
3. timer 句柄数量、cancel/reschedule 吞吐、到期抖动和 shutdown 收敛数据；
4. `TaskLifecycleState::Cancelled`、`CancellationStatus` 与现有 failure 计数互斥的断言；
5. Android forced fallback 的编译期和运行时语义测试。

取消状态和 timer 状态属于诊断/生命周期数据，不保存 callable、future 值、异常对象或
业务 payload。句柄状态查询为 best-effort 快照，不得作为 cancel 与执行竞争的同步手段。

## 10. 验收与测试矩阵

### 10.1 C1

- 句柄任务的排队取消、运行中 token 取消、无 token 任务取消、重复/过期句柄；
- cancel、worker 开始、soft timeout、shutdown 四方竞争；
- 依赖阻塞、依赖取消、`when_all` 取消传播；
- future 对 `Explicit`、`Shutdown`、`DependencyCancelled` 的异常原因断言；
- 成功取消不产生 failure event，旧无效 periodic id 行为回归；
- Linux、Windows 以及 `EXECUTOR_STOP_TOKEN_FORCE_FALLBACK`。

### 10.2 T1

- delayed/periodic handle cancel、reschedule、复制/移动和 scoped 析构；
- cancel 与到期竞争无双执行、无 use-after-free；
- timer thread 创建失败、shutdown pending future 收敛；
- periodic 已提交 tick 与后续 cancel 的边界；
- `benchmark_timer_precision` 回归及大量句柄下的资源预算。

### 10.3 不在本阶段验收

- asio strand 上的对象所有权和 timer 销毁顺序；
- 外部 context 的 post 纳入 admission/统计/失败体系；
- realtime/GPU/Blocking I/O 的统一任务取消。

上述项目分别属于 S2/T2 或对应后端自己的设计。

## 11. 先行评审发现与处置

### 11.1 已处置

| 严重度 | 发现 | 设计处置 |
| --- | --- | --- |
| 高 | `Task::cancelled` 按值复制，不能实现 handle 驱动的全路径取消 | 引入每任务独立、各副本共享的 `TaskCancellationState`，并规定统一线性化点 |
| 高 | broken promise 或空值无法稳定表达取消 future | 定义公共 `TaskCancelled` 和 reason，所有取消 future 必须就绪 |
| 高 | 自动 token 注入会改变泛型 lambda/重载 callable 行为 | 一期采用显式 `submit_cancellable*`，token 固定为首参数 |
| 高 | 普通 handle 析构取消会导致临时对象意外取消 | `TimerHandle` 析构不取消，另设 move-only `ScopedTimerHandle` |
| 高 | T1 无 strand 绑定却可能被用于替换 asio timer | 明确 T1/T2 边界，T2/S2 前禁止迁移依赖 strand 所有权的 timer |
| 高 | facade 的 `std::function<void()>` 提交边界不能传递共享 state | 增加内部 controlled submission envelope，统一 task id、soft timeout 和 worker 终态仲裁 |
| 中 | shutdown 当前把 pending timer 当 rejected | delayed 改为 `TaskCancelled(Shutdown)`；periodic pending 停止不新增 failure |
| 中 | 取消历史和 registry 可能无界增长 | active entry 终态即清理，tombstone 有界保留 |
| 中 | Android fallback 没有 stop callback | 一期只承诺 polling；公开 callback 前必须补 fallback |
| 中 | periodic tick 可重叠，不能用一次性 timer 状态机表达 | 注册期间保持 `Scheduled`，按 generation 派发并单独统计 active callback |
| 中 | priority queue 无法原地删除/重排，重复 reschedule 可能积累 | 使用 registry + generation heap，并要求 stale entry 压缩/容量门槛 |
| 中 | handle 可能晚于 Executor 析构 | 句柄只持有可失效控制锚点，不持有裸 Executor 指针或延长 callable 生命周期 |
| 中 | timer 到期后已派发不等于 callback 已执行 | 返回 `CancellationRequestedAfterDispatch`，继续请求排队或运行中取消 |

### 11.2 冻结决策（2026-08-29）

以下决策已冻结，C1/T1 按此实现：

1. **`TaskCancellationState` 具体形态**：按第 4.1 节形态落在公共头
   `include/executor/task_cancellation.hpp`（因 facade 模板与 wrapper 需要完整类型），
   由 `std::atomic<TaskControlPhase>` 单原子提供线性化点，不额外加锁；completion
   sink 在 state 发布前一次性设置，发布后只读。registry 由 facade 持有，active
   entry 默认容量 65536（`set_cancellation_registry_capacity()` 可调），终态
   tombstone 与 active 同容量、FIFO 淘汰；容量耗尽时新的可取消提交按提交拒绝路径
   处理（future 立即异常 + `SubmitRejected` 诊断），不静默降级。
2. **提交边界实现形态**：第 4.4 节的 controlled submission envelope 目标（统一
   task id、soft timeout 与 worker 终态仲裁）由 "facade wrapper + 终态 phase CAS"
   达成：wrapper 与 `on_timeout` 处理器共享同一 `TaskCancellationState`，
   cancel/timeout/开始执行都通过同一 CAS 仲裁，任务在 scheduler/本地队列/steal
   的副本经由闭包内的 `shared_ptr` 共享同一 state。不再新增
   `IAsyncExecutor` 受控提交虚方法，`Task` 公共结构不变。
3. **`TaskCancelled` 公共异常**：是公共异常（`executor::TaskCancelled :
   std::runtime_error`），携带 `TaskCancellationReason{Explicit, Shutdown,
   DependencyCancelled}`；所有取消 future 以它就绪。
4. **explicit cancellable overload 命名**：`submit_cancellable` /
   `submit_cancellable_priority` / `submit_cancellable_after`（含单句柄与句柄向量
   两个 overload），token 一律作为 callable 首参数注入，全部返回
   `TaskSubmission<T>`。不对既有 `submit()` 做自动 token 检测。
5. **Android fallback stop callback**：一期跨平台契约只承诺
   `stop_requested()` polling；fallback 实现（含 forced-fallback 编译实例化测试）
   不新增 callback 注册接口。公开 stop callback 前必须先补 fallback 与测试。
6. **旧 `cancel_task` 兼容**：保留旧行为——只作用于周期任务；对不存在的 id 依旧
   记录 `SubmitRejected` failure 事件并返回 false。新取消入口
   （`request_task_cancel` / `TimerHandle::cancel`）对无效/过期句柄只返回结果
   枚举，不写 failure。
7. **TimerHandle 最终形态**：状态枚举、操作结果枚举与 `TimerStatus` 字段按
   第 7.3 节；`reschedule_after()` 仅对 `Scheduled` 状态生效（一次性 timer 重排
   下一次到期；周期 timer 只改下一次到期不改 period），`delay_ms <= 0` 返回
   `InvalidDuration`；RAII 类型命名为 `ScopedTimerHandle`（move-only，析构请求
   一次非阻塞 cancel）。
8. **`CancellationStatus` 与 schema**：`CancellationStatus` 为独立快照结构（字段
   按第 6.3 节），经 `Executor::get_cancellation_status()` 暴露，并作为
   `ExecutorSnapshot` 的独立成员 `cancellation`；同批新增 `TimerStatusSummary`
   （pending/executed/cancelled 定时任务计数）成员。`ExecutorSnapshot::
   schema_version` 由 2 升至 3（纯新增字段，快照文本格式同步扩展）。

## 12. 实施门禁

C0 评审者应逐项确认第 11.2 节，并把结论回写本文。第 11.2 节已于 2026-08-29
全部冻结；C1/T1 分别以独立、可回滚提交实现；公开 API、编译示例和测试通过后，
才能更新 API/MIGRATION 文档及网站。

S1 可以与 C0 并行。S2 必须由 S1 使用反馈门控，T2 又必须由稳定的 S2 context adapter
门控；不能因为 T1 已提供句柄就扩大对外部 strand timer 的能力声明。
