# 迁移指南

本文档说明不同 kairo 版本之间的迁移方式。若你从旧版本升级，请按对应版本节的说明操作。

---

## 从 0.5.2 升级到 0.5.3：评审修复与定时器事件驱动

0.5.3 是稳定性与性能维护版本，公开 API 签名与既有语义保持兼容，无需改
代码。主线是 2026-09-30 全量代码评审（`docs/CODE_REVIEW_2026-09-30.md`）
四阶段的修复（P0 内存安全/挂死/数据竞争 9 项、P1 功能正确性 24 项、构建/
打包 8 项）与热路径/定时器性能优化。需要关注的可观察行为变化：

- **定时器线程不再 1kHz 轮询**：定时器调度线程改为事件驱动条件等待，
  空闲（含零定时器）等待 CPU 从恒定 0.7-1.0% 单核降至约 0.024%。此前
  部署若依赖"定时器存在即有一个恒定忙轮询线程"这类非契约观察结果，
  请改用快照诊断接口观测。
- **periodic 定时器网格锚定**：周期任务的下次到期从 `本次唤醒时刻 + 周期`
  改为 `上一理论到期点 + 周期`（错过不追补）。唤醒过冲不再逐周期累积成
  漂移，长期运行时第 N 个周期的实际触发时刻不再滞后理论时刻；单次到期
  精度与之前持平。若下游曾依赖旧实现的累积漂移行为（不属公开契约），
  请自行在回调内维护绝对时刻。
- **边界缺陷的行为修复**：此前在特定时序下挂死/泄漏/数据竞争的场景
  （单例退出、初始化失败后提交、参数绑定异常、上下文先于派发销毁、
  KeepLatest 竞争丢最新值、worker park 丢唤醒等）现按文档契约工作。
  完整清单见 `CHANGELOG.md` 0.5.3 小节。

迁移动作：无需改代码。构建侧注意两点（源自评审 Phase 3）：打包脚本版本
默认值改从 `project(VERSION)` 提取（新增生成的 `kairo/version.hpp` 可
直接 include 获取版本）；`KAIRO_ENABLE_SANITIZERS` 不再控制 TSAN
（独立开关，且与 ASAN 显式同开在配置期报错）。

---

## 从 0.5.0 升级到 0.5.2：依赖驱动调度

`submit_after()` / `submit_after_with_handle()` / `when_all()` 的公开签名与
返回类型不变，但依赖等待的执行模型从"dependent 任务立即入队、wrapper 在
worker 上等待条件变量"演进为 dependency-driven scheduling（设计见
`docs/design/dependency_driven_scheduling.md`）。需要关注的可观察行为变化：
- **依赖等待不再占用 worker**：依赖未就绪的 dependent 驻留调度侧
  （生命周期 `DependencyBlocked`），依赖全部成功后按提交时的 priority
  入队（补记 `Queued`）。原"低线程数 + 宽依赖可饿死线程池"的窗口消除；
  依赖图的规模边界仍是 `max_in_flight_tasks`（parked 期间照常占额）与
  `task_graph_retention_capacity`。
- **queued soft timeout 计时包含依赖等待期**：`task_timeout_ms > 0` 时，
  超时预算自提交时刻起算（此前同样自入队起算，行为一致；区别是 parked
  期间现在也会触发超时），parked 超时按失败结算并级联下游。
  `task_timeout_ms == 0`（默认）行为不变——依赖等待无超时上限。
- **shutdown 对 parked 任务的结算**：`shutdown()` 返回时仍 parked（依赖
  永不就绪）的任务以 `std::runtime_error`（"Executor is shutting down;
  parked task was never executed"）结算，future 不悬空；admission/取消
  计数同步释放。此前这类任务的 future 可能永不就绪。
- **取消语义不变**：排队取消仍可赢（future 以 `TaskCancelled` 就绪），
  运行中取消仍是协作式。

迁移动作：无需改代码。若下游曾依赖"dependent 任务会出现在执行器队列中"
这类实现细节（不属公开契约），请改用生命周期观测与 future 等待。

---

## 从 0.4.0 升级到 0.5.0：任务生命周期语义与确定性边界

0.5.0 保持既有公开提交 API 的签名与返回类型不变，但引入四个迁移主题：任务级
协作取消与定时句柄、串行执行上下文与总量有界 admission、`ExecutorSnapshot`
schema 2 → 3，以及若干可观察行为变化。Android CPU-only 交叉编译为新增平台
能力，不影响既有桌面集成。

### 任务协作取消与定时句柄迁移

本版本在 facade 上新增任务级协作取消（C1）与定时句柄（T1），并调整了两处可观察
行为。既有 `submit()` / `submit_priority()` / `submit_with_handle()` /
`submit_delayed()` / `submit_periodic()` / `cancel_task()` 的签名与返回类型不变。

#### 迁移到 TimerHandle：哪些自建定时可以迁移

满足以下全部条件的自建延迟/周期工作可以迁移到
`submit_delayed_with_handle()` / `submit_periodic_with_handle()`（或带
`cancellable` 的 token 变体）：

- 回调不要求在某个外部序列化上下文（asio strand 等）上执行与销毁；
- 状态所有权可以在回调与提交方之间用 `shared_ptr` 显式移交；
- 需要"析构即取消"时用 `ScopedTimerHandle` 包装句柄。

典型可迁移项：应用侧 `sleep_until` + 标志位手写的延迟重试、健康检查刷新、
超时降级等后台循环。

**T2 验收前不得迁移**的定时器：`asio::steady_timer`（或任何外部事件循环的
定时器）中"到期回调与 timer 对象销毁必须在同一 strand 上发生"的场景——facade
定时器把到期工作派发到默认异步线程池，不提供 strand 所有权或同上下文销毁保证
（见 [外部事件循环互操作指南](external_event_loop_interop.md) §5）。

#### 从私有 deadline 轮询迁移到 StopToken 协作取消

原来在任务体内手写"检查 deadline / 检查取消标志 / 提前 return"的模式，可以迁移到：

```cpp
auto submission = kairo.submit_cancellable(
    [](kairo::StopToken token) {
        while (!token.stop_requested() /* && 还有工作 */) {
            // 每步工作之间轮询 token
        }
        return result;
    });
// 需要停止时：kairo.request_task_cancel(submission.handle);
```

适用边界：任务自身能在工作步之间主动检查停止状态。阻塞在无 wakeup 机制调用上
（如不可中断的同步 I/O）的任务不会被取消打断，这类工作应使用 Blocking I/O
worker 的 `run(StopToken)` + `wakeup()` 契约，或让阻塞调用本身可超时。

#### 可观察行为变化

1. **shutdown 清理未到期 delayed 任务**：future 异常由
   `std::runtime_error("Timer stopped before delayed task execution")` +
   `SubmitRejected` failure 事件，改为 `TaskCancelled(Shutdown)` 且**不记
   failure 事件**；可观察性转移到 `get_timer_status_summary().cancelled_count`。
   依赖旧 SubmitRejected 诊断的监控需改盯新计数或异常类型。
2. **`ExecutorSnapshot::schema_version` 2 → 3**：新增 `cancellation`
   （`CancellationStatus`）与 `timers`（`TimerStatusSummary`）字段，快照文本新增
   `cancellation.*` 与 `timers.*` 行；解析 schema 版本的下游工具需按 3 更新。
3. 取消本身（成功取消、协作取消退出）不进入 `ExecutorFailureStatus`，改由
   `get_cancellation_status()` 独立计数。旧 `cancel_task()` 对无效周期任务 id 记
   `SubmitRejected` 的行为**保持不变**。

---

### 串行派发安全与总量有界 admission

本版本重构 `submit_on`/`submit_on_with_handle` 的派发包装为非阻塞共享状态
（消除多 worker 饥饿与栈条件变量竞争），并新增 `max_in_flight_tasks` 总量
有界 admission。所有既有提交 API 的签名与返回类型不变。

#### 移除串行派发兼容层（Mira EXE-20260830-002/003 临时方案）

如果你的集成在 facade 之外自建了"reserve ticket + 非阻塞 post_reserved +
业务 promise"的兼容层（例如 Mira `RuntimeBaseline` 的非阻塞 tracked dispatch），
可以迁移回直接 `submit_on_with_handle()`：

- 派发包装不再阻塞 worker 等待串行回调，多 worker 池下突发提交按 ticket
  FIFO 有界时间内结算（两 worker × 10,000 突发在回归测试中约 1s 内完成）；
- 同步状态由共享对象拥有，TSAN 下重复 10,000+ 次串行提交无
  condition-variable lifetime race；
- 迁移前置检查：依赖版本包含 `docs/design/serial_execution_context.md`
  "派发与结算结构"一节；回归运行 `test_serial_execution_context`、
  `test_serial_context_stress`。

#### 把应用侧在途计数迁移到 max_in_flight_tasks（Mira EXE-20260830-001 临时方案）

如果你的集成在提交边界自建原子在途计数以获得拒绝语义（例如 Mira
`RuntimeBaseline` 的有界 admission boundary），可改为原生配置：

```cpp
kairo::ExecutorConfig config;
config.max_in_flight_tasks = /* 原 max_in_flight */;
```

- 拒绝语义：future 立即以 `CapacityExhaustedException` 就绪 +
  `FailureKind::CapacityExhausted` 事件（区别于 stopping 与 invalid input）；
- 迁移后删除应用侧计数与拒绝转换逻辑；回归运行 `test_bounded_admission`；
- 注意语义差异：原生容量按 Executor 内部结算点计算（覆盖 scheduler、本地
  队列与执行中），不再按应用命令生命周期；`submit_delayed*`/`submit_periodic*`
  不在覆盖范围（见 API.md §3.10）。

#### queue_capacity 语义澄清

`ExecutorConfig::queue_capacity` 只构造每 worker 本地有界队列并驱动扩缩容
阈值，**不是总量背压边界**；本地队列满时任务回退到 scheduler 全局队列。
如果你的代码把 `queue_capacity` 当作拒绝边界使用，请改用
`max_in_flight_tasks`。

---

### ExecutorSnapshot schema 2 → 3

- `ExecutorSnapshot::schema_version` 由 2 升至 3，新增 `cancellation`
  （`CancellationStatus`）与 `timers`（`TimerStatusSummary`）独立字段与快照
  文本行；字段为纯新增，未删除或改名既有字段。
- 解析快照文本的下游工具需按新 schema 更新：按行前缀解析的实现通常只需
  增加两行识别；按列数或字段总数断言的实现需要放宽。
- 查询入口 `Executor::get_cancellation_status()` /
  `get_timer_status_summary()` 与快照字段同源。

### 行为变化清单

- **进程内存锁租约（P-004）**：`util::ProcessMemoryLockLease` 引用计数管理
  `mlockall`/`munlockall`——最后一个持有租约的实时执行器停止时才解除进程锁。
  此前单独停止某个实时执行器即解锁；与进程内其他 `mlockall` 使用方共存更安全，
  但依赖"停止即解锁"旧语义的部署需要复查。
- **shutdown 清理未到期 delayed 任务**：future 异常由
  `std::runtime_error("Timer stopped...")` + `SubmitRejected` failure 事件改为
  `TaskCancelled(Shutdown)`，不再记录 failure 事件；按异常类型或 failure 计数
  对该路径告警的调用方需要把检查迁移到 `TaskCancelled` 分类与定时计数。
- **性能取舍提示**：无锁对象池在 2-4 生产者的 mpsc 吞吐较 0.4.0 下降约
  23%-24%（Treiber 单链在低生产者数下的缓存行往返；1P 与 8P 以上显著提升，
  见 CHANGELOG 0.5.0 性能小节）。低生产者数且对吞吐敏感的集成可评估线程池
  执行器路径。

## 从 0.3.1 升级到 0.4.0：通信同步核心无锁化

0.4.0 保持既有通信类型与主要调用方式兼容，同时将通信同步核心改为构造期固定存储和原子状态，并新增 Topic、LET 阶段契约、实时分配诊断、通信延迟分位数、任务图句柄保留上限和真实 worker 扩缩容。升级后应复查以下同步与实时使用边界：

- `MpscChannel<T>` / `RealtimeChannel<T>` 改为构造期预分配的有界 MPSC 节点池，数据路径不再使用
  mutex；仍要求一个逻辑消费者。
- `LatestMailbox<T>` / 未绑定的 `DoubleBuffer<T>` 改为四个固定 reader-pin 快照槽，复制非平凡
  `T` 时不依赖存在 data race 的 seqlock。`try_load()` 最多检查四个槽；`try_publish()` 是系统级
  lock-free，但竞争 CAS 可重试，不能声明为单次调用有界或 wait-free。
- `PhaseGate` / `Sequencer` 使用原子状态核心。带 timeout 的 wait API 与保证成功的兼容 API 仍会
  spin/yield，只适合普通控制线程。
- 新增 `is_synchronization_lock_free()`；兼容的 `is_lock_free()` 返回相同结果。所需原子不是
  lock-free 时，组件在构造时拒绝运行，而不是静默退化到库内部锁。
- channel 的 `close()` 只关闭新的 producer 准入；关闭前已经准入的 producer 仍可完成发布。
  需要判断所有已接受消息均已排空时使用 `is_drained()`，不要只把某次 `empty()` 当作终态。
- `Topic<T>` 是明确例外：subscription registry 和 `publish()` fan-out 快照仍使用 mutex 与动态
  分配，整体不是实时或 lock-free 路径。

上述“同步无锁”和“内部固定存储”不覆盖 `T` 的复制/移动/析构、时钟、诊断 callback、缺页、
调用方分配或 OS 调度。迁移实时路径时应使用非等待 API、关闭高频 callback，并在目标硬件上验证
完整调用链的最坏耗时与页面驻留情况。`Topic<T>` 用于普通控制面 fan-out；需要实时或无锁保证时，继续使用经验证的专用数据面。

---

## 从 0.3.0 升级到 0.3.1：统一 Facade 与自动路由

0.3.1 除实时进程内存锁配置项外是向后兼容扩展。`submit()`、`submit_gpu()`、四参数 legacy `submit_auto(TaskCharacteristics, name, kernel, config)`、实时和 Blocking I/O 的既有入口及返回类型均保持不变。

### 破坏性变更：实时进程内存锁配置

`RealtimeThreadConfig::enable_memory_lock` 已更名为 `enable_process_memory_lock`，并改为默认关闭，以纠正 Linux `mlockall` 的进程级语义。若需要进程级内存锁，改用 `enable_process_memory_lock = true`，并检查 `RealtimeExecutorStatus::process_memory_lock_applied` 与 `process_memory_lock_errno`；仅在完成整个进程的 memlock 内存预算评估后显式启用。

### 推荐迁移路径

- 普通短任务可从 `submit()` 逐步迁移到 `submit_auto(lambda)`；它默认只选择异步线程池，并通过 `get_last_routing_decision()` 提供可解释的默认决策。
- 需要两条独立实现时使用 `cpu_gpu_task(cpu, gpu)`。默认 `FallbackPolicy::NoFallback`：GPU 不可提交会使 future 进入异常；只有显式 `AllowCpu` 才会回退 CPU。带返回值的 CPU/GPU 自动任务尚未提供。
- 已验证的 MPSC 单消费者路径使用 `dispatch_auto()` + `LowLatency`，周期实时工作使用 `dispatch_auto()` + `RealtimeQueue`。两者必须指定已启动后端，返回的 `DispatchResult` 仅表示接收，不表示完成。
- 长期可中断 I/O 推荐改用 `start_worker(BlockingWorkerSpec{...})`。`WorkerHandle` 统一启动结果、状态查询和停止，但不改变 `wakeup()`、stop token、启动超时或退出原因契约。
- 通过 `get_executor_capabilities()` 枚举所有后端状态；它是预检快照，不能替代处理实际投递竞争和背压。
- 低频健康检查、等待/关闭超时现场和故障支持包可新增 `get_snapshot()` 或 `get_snapshot_text()`；它们是只读 best-effort 诊断，不触发默认异步执行器懒初始化，也不替代提交 reservation、任务结果或后端专属状态 API。可运行的最小示例见 `examples/lifecycle_snapshot.cpp`。

### 兼容与后续版本

- legacy CPU/GPU `submit_auto` 在整个 `0.3.x`（包括 0.3.1）保持现有“GPU 未就绪即失败、无隐式 CPU 回退”的行为，暂不添加编译期弃用标记。
- 后续允许破坏性变更的主版本才会进入 legacy overload 的弃用/移除窗口；`CpuGpuTask<T>` 的返回值支持和 `ExecutionReport<T>` 也仅在该窗口评估。
- 自动路由不能证明 callable 的实时安全、线程安全、GPU 内存所有权或 I/O 可中断性；这些仍由应用设计、部署和测试。

---

## 0.3.0：Blocking I/O worker

`BlockingIoExecutor` 是向后兼容的库级扩展，用于替代由调用方手写、长期阻塞且需要有序停止的 `std::thread` / `std::jthread`。它不提供协议、设备或业务流程迁移：调用方保留自己的 worker 实现、消息数据面和安全策略。

### 推荐迁移路径

1. 将长期循环封装为 `IBlockingIoWorker`：把主体放入 `run(kairo::StopToken)`，实现不抛异常且可重复调用的 `wakeup()`。
2. 保证停止可达：`wakeup()` 要直接解除等待；不能直接唤醒时使用有限 timeout，并在每次返回后检查 `kairo::StopToken`。不要依赖 stop token 自动中断外部库调用。桌面平台上该类型等价于 `std::stop_token`，现有 override 保持源码与 ABI 兼容。
3. 用 `register_blocking_io_worker_ex()` 注册，再用 `start_blocking_io_worker_ex()` 启动；将 `ExecutorResult` 的拒绝原因写入调用方日志或诊断。
4. 用 `stop_blocking_io_worker()` 或 `Executor::shutdown()` 收敛生命周期。不要 detach worker，也不要在 `shutdown(false)` 时假定 I/O worker 会继续运行。

### 不适用的迁移

- 有限、可排队的工作仍应使用线程池；不要为短任务创建 I/O worker。
- 固定周期控制回调仍应使用 `RealtimeThreadExecutor`；不要在 `cycle_callback` 内等待长期 I/O。
- 协议解析、设备重连、数据新鲜度、命令语义和安全动作不属于 `kairo`，由调用方独立设计和验证。

---

## 从 0.2.3 升级到 0.3.0

0.3.0 重点新增通信与并发辅助 facade，把常见跨线程通信、实时周期消费、快照读取和任务时序控制提升到 `Executor` / `kairo::comm` 公开层。已有手写同步代码可以继续工作；新代码建议优先迁移到下列组件，以获得统一生命周期、背压和诊断统计。

### 推荐迁移到通信与并发辅助 facade

阶段 7 新增 `kairo::comm`，用于替代常见的手写共享变量、mutex、condition_variable、底层无锁队列和 promise/future 链。综合示例见 [examples/comm_robot_pipeline.cpp](../examples/comm_robot_pipeline.cpp)，它模拟传感器采集、规划、实时控制和状态监控流水线。

迁移建议：

- 采集线程到规划线程的有界数据流：从“共享 vector + mutex”或直接使用底层队列，迁移到 `MpscChannel<T>` / `SpscChannel<T>`。满队列、关闭、超时通过返回值和 `CommStats` 可见。
- 配置线程到实时控制线程的“只要最新值”：从共享配置对象和原子 flag，迁移到 `LatestMailbox<T>`。实时线程用 sequence 避免重复消费旧配置。
- 实时周期内处理有限条命令：从实时线程里阻塞等待队列，迁移到 `RealtimeChannel<T>::drain_for_cycle()`，并设置每周期预算。该 facade 当前提供有界、非等待的调用语义，但其内部仍使用 mutex；硬实时或无锁要求需使用经验证的专用实现。
- 监控线程读取系统状态：从共享 mutable state，迁移到 `DoubleBuffer<T>` / `Snapshot<T>`，读者只看到完整发布后的快照。
- 启动、初始化、阶段顺序：从手写 condition variable predicate，迁移到 `PhaseGate` / `Sequencer`。
- 任务级依赖：从手写 promise/future 链或轮询 `TaskDependencyManager`，迁移到 `TaskHandle`、`submit_with_handle()`、`submit_after()` 和 `when_all()`。
- 诊断：每个通信组件都有 `stats()`；低频事件可通过 `set_event_callback()` 接入日志或监控。通信事件默认不计入 `ExecutorFailureStatus`，需要统一上报时由业务在 callback 中桥接。

### 选择指南

| 旧写法/需求 | 推荐 facade |
|-------------|-------------|
| producer/consumer 传递每条数据 | `MpscChannel<T>` / `SpscChannel<T>` |
| 控制配置只关心最新值 | `LatestMailbox<T>` |
| 实时周期内 drain 有限命令 | `RealtimeChannel<T>` |
| 多读者读取完整系统状态 | `DoubleBuffer<T>` / `Snapshot<T>` |
| 启动顺序、阶段推进 | `PhaseGate` |
| 精确 ticket 顺序 | `Sequencer` |
| 任务完成后再执行后续任务 | `TaskHandle` + `submit_after()` / `when_all()` |

### 破坏性变更

**无。** 0.3.0 保持 0.2.3 公开 API 兼容；通信 facade、任务图 facade、统计和场景示例均为向后兼容扩展。旧的共享变量、手写锁、底层队列和 promise/future 链仍可继续使用，但新代码推荐逐步迁移到 `kairo::comm` 和 `Executor` facade。

---

## 从 0.2.2 升级到 0.2.3

0.2.3 是向后兼容版本，重点补齐 `Executor` facade 的失败可观察性、可诊断结果和等待生命周期状态。已有代码可以继续使用旧 `bool` API；新代码建议迁移到下列可诊断入口。

### 推荐迁移到可观察 facade

- 初始化、实时注册/启动、GPU 注册建议从旧 `bool` API 迁移到 `initialize_ex()`、`register_realtime_task_ex()`、`start_realtime_task_ex()`、`register_gpu_executor_ex()`，失败时读取 `ExecutorResult::error_code` 和 `message`。
- 普通任务仍通过 `future.get()` 获取返回值和重新抛出的任务异常；同时可通过 `Executor::set_failure_callback()`、`get_failure_status()`、`get_recent_failures()` 监控未被调用方立即消费的失败趋势。
- 实时任务推送建议从 `auto* rt = get_realtime_executor(...); rt->push_task(...)` 迁移到 `Executor::push_realtime_task()` / `try_push_realtime_task()`，以便不存在、未启动、队列满、对象池耗尽等失败同时通过返回值、failure event 和 `RealtimeExecutorStatus` 计数可见。
- 等待任务完成时，新代码优先使用 `wait_for_completion_for(timeout)` 或 `wait_for_completion_ex(timeout)`；后者在超时时返回 `WaitResult::status.pending_tasks`、`active_tasks`、`queued_tasks`，并累计 `wait_timeout_count`。
- 旧 API 均保持兼容；迁移的目的不是改变执行模型，而是让已有失败路径带上可诊断结果和统一监控入口。

### 破坏性变更

**无。** 0.2.3 保持 0.2.2 公开 API 兼容；新增 result、failure callback、facade push 和 wait result API 均为向后兼容扩展。

---

## 从 0.2.1 升级到 0.2.2

0.2.2 是向后兼容版本，**没有破坏性变更**。已有 0.2.1 代码可以直接重新编译使用；需要注意的是，部分 facade 默认值改为"默认即最优"，零配置用户会自动获得更积极的线程池与实时线程配置。

### 默认值变化：默认即最优 Facade

- `RealtimeThreadConfig.enable_memory_lock` 默认 `true`：Linux 下尽力尝试 `mlockall`，降低分页导致的实时抖动；平台不支持或权限不足时安全回退，不改变任务状态。
- `RealtimeThreadConfig.timer_slack_ns` 默认 `1`：Linux 下将 timer slack 调到 1 ns；设置为 `0` 表示显式 opt-out。
- `ThreadPoolConfig.min_threads` / `max_threads` 默认 `0`：作为 sentinel，初始化时自动探测 `hardware_concurrency()`；探测失败退到安全默认。
- `ThreadPoolConfig.enable_work_stealing` 默认 `true`：`max_threads == 1` 时自动关闭。
- `cpu_affinity` 为空时自动分配：线程池使用 [0..hw-1]；实时线程空 affinity 时通过 `g_next_rt_cpu_hint` 在当前允许 CPU 集合内 round-robin 自动选择，可用 CPU 数量 <= 1 时不设置亲和性；显式配置始终保留。

### 新增 API

- `IRealtimeExecutor::push_task_ex(std::function<void()>) -> bool`：背压可见版本的实时任务推送 API。返回 `true` 表示成功入队，返回 `false` 表示任务因空任务、队列满或对象池耗尽被丢弃；`push_task()` 的 `void` 签名保留以保证兼容。
- `RealtimeExecutorStatus` 新增背压字段：`dropped_task_count`、`failed_pushes`、`peak_queue_size`、`queue_capacity`，用于观察实时任务队列是否出现丢任务。
- `task_timeout_ms` 软超时：当任务开始执行前发现排队时间 `elapsed >= timeout` 时跳过任务并增加 `timeout_count`。执行中的任务不会被强制中断。

### 失败可观察性约定

Facade 的默认调优可以安全回退，但运行时任务状态不能静默丢失。任务异常、提交拒绝、实时队列丢任务和超时应通过 `future`、返回值、状态计数或监控统计暴露；调用方可以选择不响应这些信号，但库不应让失败无迹可寻。

### 破坏性变更

**无。** 0.2.2 保持 0.2.1 公开 API 兼容；新增字段、默认值和 API 均为向后兼容扩展。

### 升级检查清单

- [ ] 如果业务不希望库自动锁内存或调整 timer slack，显式设置 `enable_memory_lock = false` 或 `timer_slack_ns = 0`。
- [ ] 如果线程池线程数或 CPU 亲和性必须固定，显式设置 `min_threads`、`max_threads` 与 `cpu_affinity`，不要依赖默认 sentinel。
- [ ] 实时任务推送路径建议从 `push_task()` 迁移到 `push_task_ex()`，并监控 `dropped_task_count`。
- [ ] 使用 `task_timeout_ms` 时确认它是软超时：长任务需要在任务内部自行检查取消条件。
- [ ] 打包或安装 GPU 版本时确认 CUDA/OpenCL 为可选运行时依赖；无 GPU 或无 CUDA 驱动时会运行时降级。

---

## 从无到有（首次使用）

**0.1.0** 为首个发布版本，无需迁移。直接参考 [README.md](../README.md)、[docs/API.md](API.md) 与 [docs/BUILD.md](BUILD.md) 集成即可。

---

变更摘要见 [CHANGELOG.md](../CHANGELOG.md)。
