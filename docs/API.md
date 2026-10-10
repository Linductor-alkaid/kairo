# Kairo API 使用说明（v0.6.1）

本文档说明 `kairo` 库的主要 API、配置与类型，便于集成与扩展。完整接口定义见头文件 `include/kairo/`。

---

## 1. 包含头文件

使用 Executor facade 时包含：

```cpp
#include <kairo/executor.hpp>
```

该头文件已包含 `config.hpp`、`types.hpp`、`interfaces.hpp`、`executor_manager.hpp`。使用 GPU API 时需启用 `KAIRO_ENABLE_GPU` 并包含 GPU 相关头文件（见 [BUILD.md](BUILD.md) 构建选项）。

通信与并发辅助 facade 使用独立聚合头：

```cpp
#include <kairo/comm.hpp>
```

当前阶段提供 `kairo::comm` 命名空间、通用结果/错误码/统计/事件类型、`MpscChannel` / `SpscChannel`、`Topic` / `TopicSubscription`、`LatestMailbox`、`RealtimeChannel`、`PhaseGate`、`Sequencer` 和 `DoubleBuffer`。任务依赖 facade 通过 `Executor` 的 `TaskHandle`、`submit_after()` 和 `when_all()` 暴露。

---

## 2. 核心类：Executor

### 2.1 获取实例

| 方式 | 说明 |
|------|------|
| `Executor::instance()` | 单例，使用全局 `ExecutorManager`，进程内共享 |
| `Executor()` | 实例化模式，创建独立 `ExecutorManager`，资源隔离 |

### 2.2 初始化与关闭

```cpp
ExecutorResult initialize(const ExecutorConfig& config); // 初始化默认异步执行器（线程池），返回可诊断结果
ShutdownResult shutdown(bool wait_for_tasks = true); // 关闭所有执行器
WaitResult wait_for_completion(std::chrono::milliseconds timeout);
bool try_wait_for_completion(std::chrono::milliseconds timeout);
template<class Rep, class Period>
bool wait_for_completion_for(std::chrono::duration<Rep, Period> timeout);
bool is_idle() const;
CompletionStatus get_completion_status() const;
```

- **懒初始化**：若不调用 `initialize(config)`，首次提交任务时会使用默认配置自动初始化（不抛异常）。需要自定义线程数、队列容量等时，请在首次提交前显式调用 `initialize(config)`。
- **退出时自动关闭（单例）**：使用单例时，若未显式调用 `shutdown()`，进程退出时会自动关闭所有执行器。若需在退出前等待未完成任务完成，请在业务逻辑中显式调用 `shutdown(true)`。
- `shutdown(true)` 会先通过 facade 等待队列中任务完成后再退出；如果等待超过 `kDefaultWaitForCompletionTimeout`，会记录 `WaitTimeout` 诊断并走非等待关闭路径，避免假装全部完成。
- 从 ThreadPool worker 任务内部调用 `shutdown(true)` 或 `shutdown(false)` 时，返回 `ShutdownResult::RequestedFromWorker`：只请求关闭，**不等待**当前任务完成，也**不从 worker 内 join**。随后由外部线程调用 `shutdown(true)`，其返回 `ShutdownResult::Completed` 并完成 wait/join。
- `wait_for_completion(timeout)` 返回 `WaitResult`，其中包含 `completed`、`timed_out`、`timeout`、`message`、`CompletionStatus` 快照；超时时 `diagnostic_snapshot` 保存同一次路径采集的完整生命周期现场。等待上限由调用方传入；facade 内部（如 shutdown）使用的公开常量为 `kairo::kDefaultWaitForCompletionTimeout`（当前 300 秒）。
- `try_wait_for_completion(timeout)` 返回 `true` 表示所有已提交异步任务在 `timeout` 内完成；返回 `false` 表示等待超时且仍有任务未完成。超时不是 panic，也不抛异常；调用方可继续通过 `get_failure_status().wait_timeout_count` 或 `get_recent_failures()` 观察。
- `wait_for_completion_for(timeout)` 是支持任意 `std::chrono::duration` 的 bool 便捷入口。
- `WaitResult::diagnostic_snapshot` 仅在等待超时时有值；它与该次超时诊断回调收到的快照使用同一个 `snapshot_sequence`。它仍是 best-effort 现场，不表示任务已取消或可恢复。
- `get_completion_status()` 返回默认异步执行器的完成状态快照，包括 `is_initialized`、`is_running`、`is_idle`、`active_tasks`、`queued_tasks`、`pending_tasks`、`completed_tasks` 和 `failed_tasks`；`is_idle()` 是其中 `is_idle` 的便捷入口。状态查询不会触发默认执行器懒初始化。
- 所有上述等待 API 只覆盖默认异步执行器的 future 型任务；不会等待 GPU、无锁、实时队列或长期 Blocking I/O worker。后者分别使用其返回结果、状态和显式停止接口观察。
- `initialize(config)` 返回 `ExecutorResult`，可区分 `AlreadyInitialized`、`AlreadyShutdown`、`InvalidConfig`、`StartFailed` 等原因。

**注意事项**：懒初始化后不可再通过 `initialize()` 更换配置（已初始化则返回 false）。atexit 使用 `shutdown(false)`，不等待未完成任务。避免在静态析构中使用 Executor。

---

## 3. 任务提交 API（线程池）

### 3.0 串行上下文派发（S2）

```cpp
#include <kairo/executor.hpp>

kairo::SerialExecutionContext context;
auto future = ex.submit_on(context, [] { return 42; });
auto tracked = ex.submit_on_with_handle(context, [] { /* FIFO */ });
```

`SerialExecutionContext` 使用单线程 FIFO 执行回调；任务仍先进入 facade admission，
因此提交拒绝、异常和取消可在 kairo 状态中观察。`submit_on_with_handle` 返回的
句柄可用于 `request_task_cancel()`。上下文关闭后新提交会以 `ExecutorStopping` 完成。

派发与结算分离：池 worker 只执行有界非阻塞的 ticket 发布，不等待串行回调完成，
业务 future 由串行线程直接结算。因此小型多 worker 池不会被等待中的派发包装占满，
任意 worker 数下突发提交都按 ticket FIFO 有界时间内结算。排队取消、执行前超时与
提交拒绝都会释放 ticket，不阻塞后续 ticket 的顺序执行；被池丢弃的派发任务由
内部兜底以 `ExecutorStopping` 结算。该类型不依赖或适配 asio，也不保证与外部
strand 绑定；strand 所有权定时器迁移仍等待 T2。

### 3.1 基本提交

```cpp
template<typename F, typename... Args>
auto submit(F&& f, Args&&... args)
    -> std::future<typename std::invoke_result<F, Args...>::type>;
```

- 提交到默认线程池，返回 `std::future`。
- 支持任意可调用对象及参数，`future.get()` 获取返回值或异常。

### 3.2 优先级提交

```cpp
template<typename F, typename... Args>
auto submit_priority(int priority, F&& f, Args&&... args)
    -> std::future<typename std::invoke_result<F, Args...>::type>;
```

- `priority`：`0`=LOW，`1`=NORMAL，`2`=HIGH，`3`=CRITICAL（对应 `TaskPriority`）。
- 高优先级任务优先被调度。

### 3.3 批量提交

```cpp
template<typename F>
std::vector<std::future<void>> submit_batch(const std::vector<F>& tasks);

template<typename F>
void submit_batch_no_future(const std::vector<F>& tasks);
```

- **`submit_batch`**：批量提交任务，返回 `std::future<void>` 列表，适合需要等待任务完成的场景。
- **`submit_batch_no_future`**：批量提交任务，不返回 future（fire-and-forget），省去逐个 future 的管理开销，实际性能以 benchmark 为准。

#### 性能特性

`submit_batch()` / `submit_batch_no_future()` 的目标是减少重复提交路径开销，但当前版本不承诺固定加速比。实际结果会随任务数量、任务体、线程数、硬件、系统负载和构建配置变化；某些轻量任务或小批量场景可能低于循环 `submit()`。需要性能结论时，请优先运行本地 benchmark。

| 场景 | 经验建议 | 推荐使用 |
|------|---------|---------|
| 单线程提交大量任务 | 先 benchmark；若无需逐个 future，可优先尝试批量提交 | `submit_batch_no_future` |
| 单线程提交少量任务 | 收益不稳定，通常无需专门批量化 | 循环 `submit()` 或按实测选择 |
| 多线程并发提交 | 批量准备成本和锁竞争收益会互相抵消 | 按本地实测选择，默认可用循环 `submit()` |

#### 适用场景

**✅ 推荐使用批量提交**：
- 单线程需要提交大量任务，并且本地 benchmark 显示批量路径更快
- 任务准备开销小（如简单的 lambda）
- 不需要立即获取每个任务的 future（使用 `submit_batch_no_future`）

**⚠️ 不推荐使用批量提交**：
- 多线程并发提交（每个线程准备任务列表的开销会抵消收益）
- 任务数量较少（< 100 个）
- 需要在提交过程中动态决定是否继续提交

#### 最佳实践

```cpp
// ✅ 推荐：单线程批量提交大量任务
Executor kairo;

std::vector<std::function<void()>> tasks;
tasks.reserve(1000);  // 预分配内存

for (int i = 0; i < 1000; ++i) {
    tasks.push_back([i]() {
        process_data(i);
    });
}

// 使用无 future 版本；实际收益请以本地 benchmark 为准
kairo.submit_batch_no_future(tasks);

// 或使用有 future 版本（如需等待完成）
auto futures = kairo.submit_batch(tasks);
for (auto& f : futures) {
    f.wait();  // 等待所有任务完成
}
```

```cpp
// ⚠️ 不推荐：多线程并发批量提交
std::vector<std::thread> threads;
for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&kairo]() {
        std::vector<std::function<void()>> tasks;
        for (int i = 0; i < 1000; ++i) {
            tasks.push_back([i]() { process(i); });
        }
        // 每个线程准备任务列表的开销较大
        kairo.submit_batch_no_future(tasks);
    });
}

// ✅ 推荐：多线程直接提交
std::vector<std::thread> threads;
for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&kairo]() {
        for (int i = 0; i < 1000; ++i) {
            kairo.submit([i]() { process(i); });
        }
    });
}
```

#### 性能测试数据

> 复现元数据：数据日期 2026-07-09，结果来源 [docs/performance/batch_submit_baseline_2026-07-09.json](performance/batch_submit_baseline_2026-07-09.json)。benchmark commands: `cmake --build build --target benchmark_batch_scales benchmark_batch_submit_real benchmark_batch_submit_concurrent -j2`，`./build/tests/benchmark_batch_scales`，`./build/tests/benchmark_batch_submit_real`，`./build/tests/benchmark_batch_submit_concurrent`。commit: `2ea0c37`。CPU: 13th Gen Intel(R) Core(TM) i9-13900KF，32 logical CPUs。OS: Linux 6.8.0-124-generic x86_64。compiler: GCC 11.4.0。build type: Release。
>
> 说明：以下数据来自当前版本的一次本地 benchmark 运行，不构成固定性能承诺。不同 benchmark 的计时范围不同，不能混合成同一个加速比口径。

单线程提交路径耗时（`benchmark_batch_scales`，使用 `submit_batch_no_future`；只计提交调用耗时，完成等待在计时后）：

| 任务数 | 循环 submit | submit_batch_no_future | 实测加速比 |
|--------|-------------|------------------------|------------|
| 500    | 5528 μs     | 1152 μs                | 4.80x      |
| 1000   | 7901 μs     | 770 μs                 | 10.26x     |
| 2000   | 17487 μs    | 1291 μs                | 13.55x     |
| 5000   | 45182 μs    | 3272 μs                | 13.81x     |

单线程真实负载端到端耗时（`benchmark_batch_submit_real`，使用 `submit_batch`；计提交并等待所有 future）：

| 任务数 | 循环 submit | submit_batch | 实测加速比 |
|--------|-------------|--------------|------------|
| 1000   | 9 ms        | 3 ms         | 3.00x      |
| 5000   | 41 ms       | 16 ms        | 2.56x      |
| 10000  | 38 ms       | 33 ms        | 1.15x      |
| 50000  | 381 ms      | 290 ms       | 1.31x      |

多线程并发端到端耗时（`benchmark_batch_submit_concurrent`，使用 `submit_batch`；计并发提交并等待所有 future）：

| 线程数 | 每线程任务数 | 总任务数 | 循环 submit | submit_batch | 实测加速比 | 建议 |
|--------|-------------|----------|-------------|--------------|------------|------|
| 2      | 5000        | 10000    | 55 ms       | 39 ms        | 1.41x      | 按本地实测选择 |
| 4      | 2500        | 10000    | 43 ms       | 38 ms        | 1.13x      | 按本地实测选择 |
| 8      | 1250        | 10000    | 36 ms       | 39 ms        | 0.92x      | 默认可用循环 `submit()` |
| 16     | 625         | 10000    | 33 ms       | 34 ms        | 0.97x      | 默认可用循环 `submit()` |
| 16     | 5000        | 80000    | 423 ms      | 493 ms       | 0.86x      | 默认可用循环 `submit()` |
| 32     | 312         | 9984     | 31 ms       | 36 ms        | 0.86x      | 默认可用循环 `submit()` |

**结论**：批量提交是可选的提交路径优化，不是固定倍率性能承诺。单线程大批量、无需 future 的场景可优先 benchmark `submit_batch_no_future()`；多线程并发提交和轻量任务场景应以实测选择循环 `submit()` 或批量提交。

#### 空任务拒绝

底层 `ThreadPool::try_submit(std::function<void()>)`、`ThreadPool::try_submit_priority(...)` 和 `ThreadPool::try_submit_batch(...)` 会拒绝空的 `std::function<void()>`。单任务路径返回 `false`，带回调的 overload 会向回调传入 `std::invalid_argument("empty task")`；批量路径只要发现任意空任务就返回 `false`，并且不会部分提交同一批次中的其他任务。

`ThreadPoolExecutor` / `IAsyncExecutor` / `Executor` facade 的 future API 不会同步抛出该拒绝；`submit(empty_function)` 和包含空任务的 `submit_batch(...)` 会返回已经 ready 的 future，`future.get()` 抛 `std::invalid_argument("empty task")`。`Executor` facade 同时将该情况记录为 `SubmitRejected`。

### 3.4 任务依赖提交

`Executor` facade 提供轻量任务图 API，用 `TaskHandle` 表达同一个 `Executor` 实例内的完成依赖。需要继续链式依赖时使用 `submit_with_handle()` 或 `submit_after_with_handle()`；只关心结果时使用 `submit_after()` 返回的 `std::future`。

```cpp
kairo::Executor kairo;
kairo.initialize(config);

auto load = kairo.submit_with_handle([] {
    return load_sensor_frame();
});

auto plan = kairo.submit_after(load.handle, [&] {
    return run_planner(load.future.get());
});

auto first = kairo.submit_with_handle([] { return preprocess_a(); });
auto second = kairo.submit_with_handle([] { return preprocess_b(); });
auto both = kairo.when_all({first.handle, second.handle});

auto fused = kairo.submit_after(both, [] {
    return fuse_results();
});
```

主要 API：

- `TaskHandle`：任务图中的不透明 handle，提供 `id()`、`valid()` 和 `operator bool()`。
- `TaskSubmission<T>`：包含 `TaskHandle handle` 和 `std::future<T> future`。
- `submit_with_handle(f, args...)`：像 `submit()` 一样提交任务，同时返回可作为依赖的 handle。
- `submit_after(dependency, f, args...)` / `submit_after(dependencies, f, args...)`：等待依赖成功后执行任务，返回 dependent task 的 future。
- `submit_after_with_handle(...)`：同时返回 dependent task 的 handle 和 future，适合继续构造任务链。
- `when_all(dependencies)`：返回逻辑 handle；所有依赖成功后该 handle 成功，任一依赖失败后该 handle 失败，可继续传给 `submit_after()`。

依赖失败时，dependent task 默认不执行；dependent future 进入异常状态，`future.get()` 会重新抛出依赖异常或依赖图错误。无效 handle、跨 `Executor` 实例 handle 或 cycle 会记录 `SubmitRejected`，并返回 ready exceptional future 或失败的逻辑 handle。已完成 handle 按 `ExecutorConfig::task_graph_retention_capacity` 保留，默认保留最近 1024 个终态 handle；容量为 0 时终态 handle 立即过期。仍被活动任务依赖的终态 handle 不会提前回收。过期 handle 再用于 `submit_after()` / `when_all()` 会被拒绝并返回可诊断异常。也可通过 `set_task_graph_retention_capacity()` 在运行时调整容量。

依赖调度为 dependency-driven：依赖未就绪的 dependent 任务**不入队、不占用 worker**，驻留调度侧；依赖全部成功时由调度器按提交时的 priority 入队（生命周期 DependencyBlocked → Queued → Running），任一依赖失败/被取消时立即以依赖异常结算，任务不接触 worker。`ExecutorConfig::task_timeout_ms > 0` 时，queued soft timeout 自**提交时刻**起算、覆盖依赖等待期，parked 超时按失败结算并级联下游。`shutdown()` 返回时仍 parked（依赖永不就绪）的任务以异常结算（"Executor is shutting down; parked task was never executed"），future 不悬空。

### 3.5 软超时

`task_timeout_ms` 是线程池任务的**执行前软超时**。worker 准备执行任务时会检查 `now - submit_time`；若 elapsed >= timeout，则跳过该任务并将线程池内部 timeout 计数与 `TaskStatistics::timeout_count` 加 1。通过 `ThreadPool::submit()`、`Executor::submit()`、priority submit 或 batch submit 暴露的 `std::future` 会被显式置为异常状态，`future.get()` 抛 `kairo::TimedOutException`（例如 `Task timed out after 100ms`），不会变成 `std::future_error(broken_promise)`。

```cpp
kairo::ExecutorConfig config;
config.task_timeout_ms = 100;  // 100 ms soft timeout

auto& ex = kairo::Executor::instance();
ex.initialize(config);
```

| 行为 | 结果 |
|------|------|
| 任务排队超时，执行前检测到 | 跳过执行，`timeout_count++`；若有 future，`future.get()` 抛 `TimedOutException` |
| 任务已经开始执行后超时 | 不强制中断，继续运行到任务自行返回 |
| `task_timeout_ms = 0` | 不检查超时（默认行为） |

C++ 没有安全的通用线程强杀机制，因此 soft timeout 不会终止执行中的任务。排队超时是独立观测事件：它增加 timeout 计数，但不增加 `fail_count` / `failed_tasks`。长耗时任务应在任务内部自行检查取消条件或 deadline。

### 3.6 任务背压

实时执行器的 `IRealtimeExecutor::push_task()` 返回 `ExecutorResult`，直接反映该次 push 的成败。新代码优先使用 `Executor` facade 的 `push_realtime_task()` / `try_push_realtime_task()`；需要底层逃生口时再直接使用 `IRealtimeExecutor::push_task()`。

```cpp
if (!ex.try_push_realtime_task("can_rx", []() {
    read_can_frame();
})) {
    // 实时执行器不存在、未运行、空任务、队列满或对象池耗尽导致失败
}

auto status = ex.get_realtime_executor_status("can_rx");
const auto backpressure_drops =
    status.pool_exhausted_count + status.queue_full_count;
if (backpressure_drops > 0) {
    // 对象池或队列容量不足：可告警、扩容或降级
}
// dropped_task_count 还包括空任务和未运行/已停止时的拒绝；
// 分别查看 rejected_empty_task_count 与 rejected_not_running_count。
```

| API / 字段 | 说明 |
|------------|------|
| `Executor::push_realtime_task(name, task)` / `try_push_realtime_task(name, task)` | 推荐 facade 入口；失败同时通过返回值、failure event 和状态计数可见 |
| `push_task(std::function<void()>) -> ExecutorResult` | 底层逃生口，`ok` 表示成功入队；失败携带类别并累计到状态计数 |
| `dropped_task_count` | 总拒绝/丢弃量，覆盖未运行/已停止、空任务、对象池耗尽和队列满；不受 `enable_stats` 影响，不能单独作为背压指标 |
| `rejected_not_running_count` / `rejected_empty_task_count` | 分别分析生命周期状态拒绝和调用方传入空任务的输入错误 |
| `pool_exhausted_count` / `queue_full_count` | 背压子集：分别表示对象池耗尽和队列满；用于背压告警、容量规划与降级决策 |
| `failed_pushes` | 所有底层队列失败入队尝试数；静止快照中等于 `queue_full_rejections + contention_rejection + reservation_cancelled_rejections`。仅 `enable_stats=true` 时统计 |
| `peak_queue_size` / `queue_capacity` | 用于分析实时任务队列水位与背压比例 |

### 3.7 统一自动路由

自动路由只依据 `TaskOptions`、用户显式意图和后端能力快照；不会检查 lambda 是否阻塞、线程安全、实时安全，或是否正确管理 GPU 内存。默认 `TaskOptions` 为 `ExecutionIntent::Auto` + `FallbackPolicy::NoFallback`。

| 需求 | API | 返回值语义 |
|------|-----|------------|
| 普通短 CPU 任务 | `submit_auto([] { return value; })` | `std::future<T>`：任务完成或异常 |
| 独立 CPU/GPU 实现 | `submit_auto(cpu_gpu_task(cpu, gpu)...)` | `std::future<void>`：已接受路径的完成或异常 |
| 指定无锁低延迟队列 | `dispatch_auto(TaskOptions{LowLatency, ...}, task)` | `DispatchResult::accepted`：仅表示有界队列已接收 |
| 指定实时队列 | `dispatch_auto(TaskOptions{RealtimeQueue, ...}, task)` | `DispatchResult::accepted`：仅表示后续周期已接收 |
| 长期可中断 I/O | `start_worker(BlockingWorkerSpec{...})` | `WorkerHandle`：启动结果与生命周期控制 |

```cpp
auto decoded = ex.submit_auto(
    kairo::task([] { return decode(); }).name("decode"));

kairo::TaskOptions rt;
rt.intent = kairo::ExecutionIntent::RealtimeQueue;
rt.preferred_executor = "control";
auto admission = ex.dispatch_auto(rt, [] { apply_control(); });
if (!admission.accepted) {
    // 检查 admission.decision、admission.message 和 failure/status counters
}
```

- `LowLatency` 与 `RealtimeQueue` 必须显式指定 `preferred_executor` 且后端已运行；不会因 deadline、priority 或队列水位自动选择。`LowLatency` 后端的注册/启动/停止见 §5.9（`register_lockfree_executor` / `start_lockfree_executor`）。
- `dispatch_auto()` 的接收结果不是完成通知。实时任务的 drop/backpressure 继续由 `RealtimeExecutorStatus` 计数和 failure event 观察。
- `get_last_routing_decision()`、`get_recent_routing_decisions()` 与 `set_routing_callback()` 提供独立的路由解释；`ExecutorFailureEvent` 仍用于实际拒绝和执行失败。
- `get_executor_capabilities()` 返回所有已注册后端的建议性状态快照，只用于显示/预检；实际投递仍可能因并发 stop 或满队列被拒绝。

### 3.8 Scheduling Runtime（deadline / QoS / affinity / resource）

0.6.0 起调度决策由可注入的 `IScheduler`（`include/kairo/scheduler.hpp`）
产出，默认实现 `DefaultScheduler` 在意图路由之上叠加调度模型约束。
0.6.1 起路由结果以结构化形式表达：`RoutingDecision::status`
（`Accepted` / `AcceptedDegraded` / `Rejected`）是接受/拒绝的权威判据，
`reason`（含 `DeadlineExpired` / `AffinityMismatch`）与 `diagnostics`
位掩码（`AffinityMismatch` / `ResourceInfeasible`）解释原因，`detail`
仅供人阅读。设计见
[docs/design/scheduling_runtime.md](design/scheduling_runtime.md)。

```cpp
auto decision_done = std::chrono::steady_clock::now() + std::chrono::seconds(1);
auto task = kairo::task([] { return compute(); })
    .name("inference")
    .qos(kairo::QosClass::Interactive)          // 未显式 priority 时映射排队优先级
    .deadline(decision_done)                     // 同优先级内 EDF；错过记 DeadlineMissed
    .affinity(kairo::AffinityHint{{0, 1}})       // advisory；不匹配进结构化诊断
    .resources(kairo::ResourceRequirements{.memory_bytes = 1u << 28,
                                           .gpu_device = 0});
auto result = ex.submit_auto(task);
```

| 模型 | 语义 | 违约行为 |
|------|------|----------|
| `deadline` | 同优先级内 EDF 排序（层次 `priority → EDF → FIFO`，非全局 EDF）；开始执行时已错过记录 `FailureKind::DeadlineMissed`（`deadline_missed_count`），任务仍执行 | 提交时已过期（严格已过，恰好相等不拒） → 拒绝（`RoutingReason::DeadlineExpired`） |
| `qos` | `BestEffort/Standard/Interactive/Critical` 映射默认排队优先级（LOW/NORMAL/HIGH/CRITICAL）；显式 `priority()` 优先。QoS 是排队优先级 preset，不提供抢占/延迟界/带宽保证 | 严格优先级无 aging（CR-024）：BestEffort 可被饿死 |
| `affinity` | per-task advisory；与后端 `bound_cpus` 不相交时任务仍被接受，决策标记 `status = AcceptedDegraded` + `reason = AffinityMismatch` + `diagnostics` 位 | 不重新绑定 OS 线程；不提供独占核保留 |
| `resources` | GPU `device`/`memory_bytes` 声明与能力快照核对（feasibility hint，非资源保证；存在 TOCTOU 窗口；capability 未知时跳过检查——permissive fallback） | device 不符 → `BackendUnavailable`；内存不足 → `CapacityPressure`；两者均携带 `ResourceInfeasible` 诊断位 |

自定义调度器：`ex.set_scheduler(std::make_unique<MyScheduler>())`
（须在首次提交前调用；`nullptr` 恢复默认）。调度器只产出决策，
投递仍由 Executor 按既有后端协议执行。0.6.0 风格的拒绝（只设
`reason = Rejected` 等拒绝类 reason、未设 `status`）仍被
`route_task` 归一化为 `status = Rejected`，不会因升级而漂移。

**调度指标（0.6.1）**：`ex.get_scheduling_metrics()` 返回
`SchedulingMetrics` 单调计数快照——按 status 分类的
`accepted_count` / `accepted_degraded_count` / `rejected_count`，按
reason 细分的 `deadline_rejected_count` /
`backend_unavailable_rejected_count` / `capacity_rejected_count`，
诊断位驱动的 `resource_rejected_count` / `affinity_mismatch_count`，
以及 `deadline_missed_count` / `feedback_reported_count`。计数对象
是路由决策（一次提交可能产生多条决策，如 GPU 提交异常后回退），
无需配置观测开关即可读取。

**执行期反馈（0.6.1）**：`IScheduler::wants_feedback()` 返回 true 的
调度器会通过 `on_task_completed(SchedulingFeedback)` 收到实际执行
任务的终态测量：最终 backend/executor、`queue_wait_ns`、
`execution_duration_ns`、`had_deadline`/`deadline_missed`、
`success` 与 `failure_kind`。反馈在 worker 线程同步交付；覆盖范围是
默认异步池路径（提交前被拒/admission 拒绝/排队期超时的提交不产生
反馈，由决策与指标体系观测）。DefaultScheduler 不消费反馈，
0.6.1 也不包含任何基于反馈的自适应调度行为。

**0.7.0 开发快照（自适应调度，未随稳定版本发布）**：以下为 `master`
上已落地的 additive 扩展，未注入对应调度器时不产生任何行为或开销
变化。

- **反馈聚合层（`<kairo/feedback_aggregator.hpp>`）**：
  `scheduling::FeedbackAggregator` 按 `(backend, executor_name, qos)`
  分键，在 worker 线程无锁、无分配、noexcept 地累加 EWMA
  （`queue_wait_ns` / `execution_duration_ns`）、分桶直方图与
  失败/deadline 计数；`FeedbackAggregatorConfig`（`max_keys=16`、
  `merge_interval=100ms`、直方图桶界等，构造期夹取）控制键空间上界
  与合并节奏。读方持有 `shared_ptr<const FeedbackSnapshot>`（RCU
  风格发布）；公开查询辅助 `scheduling::find_entry(snapshot, backend,
  qos, name)`（无样本键返回 `nullptr`）与
  `scheduling::histogram_quantile_ns(histogram, q)`（桶内线性插值；
  无样本返回 0）。`Executor::get_feedback_snapshot()` 返回诊断用
  快照，`get_snapshot_text()` 追加 `scheduling_feedback.*` 段。
- **`IScheduler::effective_priority_for(options, default_priority)`**：
  新虚方法（默认实现原样返回），每次 `submit_auto(TaskBuilder)` 在
  未显式设置 `priority` 时咨询一次；显式 `priority_set` 永不经过它。
  自定义调度器可覆写以参与 QoS→priority 提升；须可多提交线程并发
  调用。
- **`AdaptiveScheduler`（`<kairo/adaptive_scheduler.hpp>`，opt-in）**：
  复用 0.6.1 基线路由 + 内嵌 `FeedbackAggregator`，叠加三类可解释
  决策：①`CpuOrGpu` 意图的历史端到端 EWMA 选择（双侧
  `min_samples=8` 后采信，切换需 `hysteresis_margin=0.25` 边际；
  命中时 `reason=AdaptiveHistory` + 同名诊断位，`detail` 携带两侧
  数值；`RequireRequestedBackend` 不翻转）；②QoS 感知降载（类
  queue wait p99 按 `queue_wait_p99_target_ns={0, 100ms, 10ms, 2ms}`
  目标、连续 `shed_breach_windows=3` 窗超阈开闸，对严格更低 QoS 类
  合成 `reason=LoadShedding` 拒绝；连续 `shed_close_windows=5` 恢复
  窗关闸，`recovery_ratio=0.5`；窗口样本 `< min_window_samples=32`
  或目标 0 时跳过评估）；③QoS→priority 有界提升（连续
  `promotion_windows=6` 窗 +1 级、`promotion_close_windows=10` 窗
  取消，封顶 CRITICAL，显式 priority 不覆盖，生效经
  `effective_priority_for`）。评估节奏每 `merge_interval` 至多一次；
  诊断接口 `load_shed_active(QosClass)` / `priority_promoted(QosClass)`
  / `feedback_snapshot()` / `format_state_text()`。`load_shedding_enabled`
  / `priority_promotion_enabled` 可分别关闭。
- **新增结构化诊断通道**：`RoutingReason::LoadShedding`；
  `RoutingDiagnostics::AdaptiveHistory`（`1u<<2`）、
  `RoutingDiagnostics::LoadShedding`（`1u<<3`）；`SchedulingMetrics`
  计数 `adaptive_history_count` / `load_shedding_rejected_count` /
  `priority_promoted_count`。对 `RoutingReason` 穷举 switch 的代码
  需补新分支；以 `RoutingDecision::status` 为权威判据的代码不受影响。
  选型与风险边界见网站"何时使用 AdaptiveScheduler"页
  （`website/zh/guides/adaptive-scheduling.md`）。

### 3.9 延迟与周期任务

> ⚠️ **API 范围提示**：`submit_delayed`、`submit_periodic` **仅在 `Executor` Facade 类中提供**，**不属于** `IAsyncExecutor`、`IExecutor` 或 `ThreadPool` 的接口。用户直接对底层 `ThreadPool` 实例调用这些方法会编译失败。延迟与周期任务统一由 Facade 内部的 `ExecutorManager` 调度，底层 `ThreadPool` 不感知任务时间维度。

```cpp
template<typename F, typename... Args>
auto submit_delayed(int64_t delay_ms, F&& f, Args&&... args)
    -> TimerSubmission<typename std::invoke_result<F, Args...>::type>;

template<typename F, typename... Args>
auto submit_delayed_cancellable(int64_t delay_ms, F&& f, Args&&... args)
    -> TimerSubmission<typename std::invoke_result<F, StopToken, Args...>::type>;

TimerHandle submit_periodic(int64_t period_ms, std::function<void()> task);
TimerHandle submit_periodic_cancellable(int64_t period_ms,
                                        std::function<void(StopToken)> task);
```

- `submit_delayed`：延迟 `delay_ms` 毫秒后执行，返回 `TimerSubmission`（`handle` +
  `future`）。调度线程停止（`shutdown`）时，未到期任务以
  `TaskCancelled(Shutdown)` 异常就绪，不记 failure 事件。
- `submit_delayed_cancellable`：同上，并注入 StopToken 支持协作取消。
- `submit_periodic`：按 `period_ms` 周期重复执行，返回 `TimerHandle`；
  `handle.cancel()` 阻止后续 tick，`handle.id()` 可用于
  `get_periodic_task_status()` 查询。
- `submit_periodic_cancellable`：同上，每个 tick 独立注入 StopToken。

#### 定时句柄（TimerHandle / ScopedTimerHandle）

`TimerHandle` 语义（详见
[docs/design/task_cancellation_and_timers.md](design/task_cancellation_and_timers.md)）：

`TimerHandle` 语义：

- 可复制控制句柄，**析构不取消**；复制共享同一控制锚点。需要 RAII 取消时使用
  move-only 的 `ScopedTimerHandle`（析构请求一次非阻塞取消，不等待在途回调）。
- `cancel()`：到期前返回 `CancelledBeforeDispatch`（任务不执行，future 以
  `TaskCancelled(Explicit)` 就绪）；到期派发后返回
  `CancellationRequestedAfterDispatch`，继续向排队/运行中的任务传播取消；
  重复取消幂等（`AlreadyCancelled`）；句柄过期或 Executor 已销毁返回 `NotFound`。
  取消是生命周期事件，**不产生 failure 事件**。
- `reschedule_after(delay_ms)`：仅对 `Scheduled` 状态生效；一次性 timer 重排
  下一次到期，周期 timer 只改下一次到期时间、不改 `period_ms`。`delay_ms <= 0`
  返回 `InvalidDuration`。
- `status()`：返回 `TimerStatus`（`timer_id`、`state`、`periodic`、
  `execution_count`、`active_callback_count`、`cancellation_count`、
  `next_execute_time`）的 best-effort 快照，不作为同步原语。
- 句柄只持有可失效锚点，不持有裸 `Executor*`；Executor 析构后操作安全返回
  `NotFound`。句柄不延长 callable 或业务对象的生命周期。

**能力边界**：facade 定时器把到期工作派发到默认异步线程池，不绑定 asio strand
等外部序列化上下文，不承诺与外部 strand 同上下文执行或销毁。需要在同一 strand
上执行与销毁的 timer 在 T2 验收前继续由应用侧管理（见
[外部事件循环互操作指南](external_event_loop_interop.md)）。

#### 集成契约：普通周期任务不是实时线程

`submit_periodic()` 只负责在 Facade 的定时器到期后，将回调提交给**普通异步线程池**。它不创建专用线程，也不承诺实时调度策略、CPU 亲和性、内存锁定、确定性唤醒或不被其他异步任务延迟。回调运行时间超过周期，或线程池拥堵时，后续一次提交可以与前一次回调重叠；因此同一个有状态对象不能被周期回调并发访问，除非应用自行串行化。

`submit_periodic()` 提供允许抖动的遥测、健康检查和后台刷新能力。固定控制周期、单线程状态所有权和实时调度尝试由第 4 节的专用实时线程提供。

### 3.10 任务协作取消

取消是**请求不是中断**：排队中的任务会被安全跳过；运行中的任务通过协作停止
令牌（`kairo::StopToken`）自行决定何时退出。阻塞在无 wakeup 机制调用上的任务
不会被强制打断。`TaskOptions::deadline` 保持 advisory 语义，不会自动触发取消。

```cpp
// 显式可取消提交：token 由 kairo 注入为 callable 首参数。
template<typename F, typename... Args>
auto submit_cancellable(F&& f, Args&&... args)
    -> TaskSubmission<typename std::invoke_result<F, StopToken, Args...>::type>;
template<typename F, typename... Args>
auto submit_cancellable_priority(int priority, F&& f, Args&&... args)
    -> TaskSubmission<typename std::invoke_result<F, StopToken, Args...>::type>;
template<typename F, typename... Args>
auto submit_cancellable_after(const std::vector<TaskHandle>& dependencies,
                              F&& f, Args&&... args)
    -> TaskSubmission<typename std::invoke_result<F, StopToken, Args...>::type>;

// 取消入口与独立计数。
TaskCancellationResponse request_task_cancel(const TaskHandle& handle) noexcept;
CancellationStatus get_cancellation_status() const;
void set_cancellation_registry_capacity(size_t capacity);

// 诊断：超时闭包墓地当前规模（0.5.2 新增）。
size_t closure_graveyard_size() const;
```

语义：

- **排队取消**（任务未开始，含依赖未满足）：任务不执行，future 以
  `TaskCancelled(Explicit)` 就绪；依赖该任务的任务以
  `TaskCancelled(DependencyCancelled)` 终止，`when_all` 聚合同样传播。
- **运行中取消**：只置位任务的 `StopToken`（`RequestedRunning`）；任务轮询
  `stop_requested()` 后正常返回则保留业务结果，抛出 `TaskCancelled` 则任务记为
  已取消。无取消请求时任务主动抛出的 `TaskCancelled` 仍按任务异常计入 failure，
  防止绕过统计。
- **幂等与过期**：重复运行中取消返回 `AlreadyRequested`；终态句柄返回
  `AlreadyCompleted`；未知/失效句柄返回 `NotFound`。成功取消不产生
  `ExecutorFailureEvent`，也不计入 `ExecutorFailureStatus`。
- **既有 API 不变**：`submit()` / `submit_priority()` 无句柄、不可取消；
  `submit_with_handle()` / `submit_after_with_handle()` 建立共享取消状态，
  天然支持排队取消（callable 不接收 token 时运行中取消只记录"已请求"）。
- **registry 容量**：按句柄取消索引默认容量 65536（active 与终态 tombstone
  各自上限），`set_cancellation_registry_capacity()` 可调；容量耗尽时新的可取消
  提交被明确拒绝（future 立即异常 + `SubmitRejected` 诊断），不会静默失去
  取消能力。

`TaskCancelled` / `TaskCancellationReason` / `TaskCancellationResponse` /
`CancellationStatus` 定义在 `include/kairo/task_cancellation.hpp`；
`TimerHandle` 等定时句柄类型定义在 `include/kairo/timer.hpp`。取消跨平台契约
只承诺 `stop_requested()` 轮询（Android fallback 同语义）。

与外部事件循环（asio strand 等）的取消/定时边界见
[外部事件循环互操作指南](external_event_loop_interop.md)。

### 3.11 总量有界 admission（max_in_flight_tasks）

`ExecutorConfig::max_in_flight_tasks` 为 facade 默认异步提交提供总量上限：
`0`（默认）不启用、零热路径开销；正值 N 表示该 Executor 实例已接纳未结算的
提交（scheduler 全局队列 + worker 本地队列 + 执行中）至多 N 个。覆盖
`submit`、`submit_with_handle`、`submit_priority`、`submit_after*`、
`submit_cancellable*`、`submit_batch*`、`submit_on*`；`submit_delayed*` /
`submit_periodic*`（timer 派发）与 realtime / GPU / Blocking I/O / lockfree
不在覆盖范围。直接使用 `ThreadPool` / `PriorityScheduler` 的调用方不受保护。

```cpp
kairo::ExecutorConfig config;
config.max_in_flight_tasks = 256;          // 总在途上限
ex.initialize(config);
// 运行期可调（不驱逐已接纳任务）：
ex.set_max_in_flight_tasks(512);
size_t in_flight = ex.get_in_flight_submissions();
size_t current_cap = ex.get_max_in_flight_tasks();  // 当前生效上限（0 = 未启用）
```

- **拒绝语义**：达到上限时提交不抛出，对应 future 立即以
  `CapacityExhaustedException` 就绪，并同步记录 `FailureKind::CapacityExhausted`
  事件（`capacity_exhausted_count` 计数）。与 kairo 停止
  （`ExecutorStopping` / `SubmitRejected`）和非法输入（`std::invalid_argument`）
  经异常类型与 failure kind 双通道可区分。
- **释放语义**：每个接纳的提交恰好释放一次容量，终态集合 = 正常完成、任务
  异常、排队取消、执行前超时、提交拒绝、context shutdown 拒绝、池丢弃。
  future 就绪前容量计数已释放（观测不变式：future 就绪后查询 status 即为
  最终值）。
- **batch**：逐任务独立接纳，部分接纳合法；被拒任务的对应 future 以
  `CapacityExhaustedException` 就绪。
- **串行上下文**：`submit_on_with_handle` 被拒时 context ticket 同步释放，
  后续 FIFO 不阻塞。
- **与 `queue_capacity` 的区别**：`queue_capacity` 只构造每 worker 的本地
  有界队列并驱动扩缩容阈值，不是总量背压边界；本地队列满时任务回退到
  scheduler 全局队列。总量背压只由 `max_in_flight_tasks` 表达。
- **无旁路**：CRITICAL 优先级不越过容量；需要余量时由配置容量表达。

详细设计见 [总量有界 Admission 设计](design/bounded_admission.md)。

---

## 4. 实时任务 API（专用线程）

### 4.1 注册、启动、停止

```cpp
ExecutorResult register_realtime_task(const std::string& name,
                                      const RealtimeThreadConfig& config);
ExecutorResult start_realtime_task(const std::string& name);
void stop_realtime_task(const std::string& name);
bool push_realtime_task(const std::string& name, std::function<void()> task);
bool try_push_realtime_task(const std::string& name, std::function<void()> task);
```

- 每个 `name` 对应一个专用实时线程，按 `RealtimeThreadConfig` 周期执行 `cycle_callback`。
- 先 `register_realtime_task`，再 `start_realtime_task`；`stop_realtime_task` 停止该线程。
- `register_realtime_task` / `start_realtime_task` 返回可诊断结果：空名或非法配置为 `InvalidConfig`，重复注册为 `DuplicateName`，启动不存在的实时执行器为 `NotFound`，重复启动为 `AlreadyInitialized`。

### 4.2 获取执行器与列表

```cpp
IRealtimeExecutor* get_realtime_executor(const std::string& name);
std::vector<std::string> get_realtime_task_list() const;
```

- `push_realtime_task` / `try_push_realtime_task`：推荐任务推送入口；失败返回 `false`，并写入 failure event / 状态计数。
- `get_realtime_executor`：高级逃生口，用于直接访问 `push_task` 等底层操作；若不存在返回 `nullptr`。返回的是 manager 持有的**非持有裸指针**，不能跨 `shutdown()` 缓存或与并发 `shutdown()` 同时使用。
- `get_realtime_task_list`：当前已注册的实时任务名称列表。

直接使用 `ExecutorManager` 时，优先使用 `get_default_async_executor_snapshot()`、`get_realtime_executor_snapshot(name)`、`get_lockfree_executor_snapshot(name)`、`get_blocking_io_executor_snapshot(name)` 和 `get_gpu_executor_snapshot(name)`。它们返回 `std::shared_ptr`，即使并发 `shutdown()` 已从注册表移除该项，也会保持对象存活到本地快照释放；这只解决对象生命周期，**不会**阻止 shutdown 请求停止执行器，因此每次调用仍须处理停止或拒绝结果。`shutdown()` 开始后，manager 拒绝新的具名执行器注册。

### 4.3 集成契约：周期、队列与安全路径

- 专用实时线程采用“每周期执行 `cycle_callback`，再在该线程中消费已入队工作”的模型，不是可无限 `drain()` 的串行执行器。`max_tasks_per_cycle` 默认限制为 64，剩余工作会留到后续周期，以保护周期预算。`LatestMailbox` 和有界通信通道提供最新状态传递能力，不会在周期内清空无界历史积压。
- `push_realtime_task()` 是有界、可拒绝的队列入口；返回值表示本次是否成功入队。`dropped_task_count`、`queue_full_count` 与 `pool_exhausted_count` 提供拒绝和背压统计。入队成功只表示工作会在实时线程的后续周期处理，不表示已完成。
- `wait_for_completion()` 仅等待默认**异步执行器**已提交任务完成，不等待实时线程的周期回调或实时队列。视觉、控制等多消费者流水线的完成状态由应用自己的确认序号、future 或 `PhaseGate` 汇总。
- `push_realtime_task()` 会等待实时线程的后续周期消费，因此不构成紧急停止路径。紧急停止由应用提供的独立硬零旁路执行，例如直接写安全 I/O、硬件急停或经过安全控制器的同步命令；实时队列承载常规控制工作。
- kairo 提供并行调度，不改变业务算法的线程安全属性。PID、限幅器、轨迹跟踪器等有状态对象可由一个控制线程串行访问；其他线程可通过消息传递输入数据或读取已发布快照。

### 4.4 实时调优的降级与部署检查

Linux 上请求 `SCHED_FIFO`、CPU 亲和性和 `mlockall` 可能因 `CAP_SYS_NICE`、`CAP_IPC_LOCK`、容器 cpuset 或平台限制而失败；库会继续运行以保持可用性。这不表示所请求的调优已经生效。Android 同样按 best-effort 处理：普通 App 通常无法申请 `SCHED_FIFO`、绑核或 `mlockall`，周期短于 10 ms 时也不会自动提升优先级；显式设置的 `thread_priority` / `cpu_affinity` 仍会尝试并记录结果。`RealtimeExecutorStatus` 通过 `priority_applied`、`cpu_affinity_applied`、`process_memory_lock_applied` 和 `timer_slack_applied` 报告各项请求的实际结果；未请求、平台不支持或权限不足的项目均为 `false`。这些字段可与周期统计和丢弃计数共同构成应用的健康或降级状态。

---

### 4.5 阻塞 I/O worker API

`BlockingIoExecutor` 管理一个长期、可中断阻塞的 `IBlockingIoWorker`。它不提供 `push_task()`，不实现协议 adapter，也不定义 worker 的输入、输出或设备状态；调用方负责 `run()` 中的实际工作及其线程安全。

#### 4.5.1 公开类型

```cpp
#include <kairo/blocking_io.hpp>
#include <kairo/executor.hpp>

class IBlockingIoWorker {
public:
    virtual ~IBlockingIoWorker() = default;
    virtual void run(kairo::StopToken stop_token) = 0;
    virtual void wakeup() noexcept = 0;
};
```

`wakeup()` 必须解除 `run()` 当前的等待，且可重复调用、不抛异常。仅请求 `kairo::StopToken` 不会中断第三方库或操作系统的无限阻塞调用；若等待原语不能直接唤醒，worker 必须使用有限 timeout 并在返回后检查 stop token。桌面平台上 `kairo::StopToken` 是 `std::stop_token` 的别名；Android 上使用等价的库内实现。

#### 4.5.2 注册与生命周期

```cpp
ExecutorResult register_blocking_io_worker(
    const std::string& name,
    const BlockingIoConfig& config,
    std::unique_ptr<IBlockingIoWorker> worker);
ExecutorResult start_blocking_io_worker(const std::string& name);
void stop_blocking_io_worker(const std::string& name);
BlockingIoExecutorStatus get_blocking_io_worker_status(const std::string& name) const;
std::vector<std::string> get_blocking_io_worker_list() const;
WorkerHandle start_worker(BlockingWorkerSpec spec);
```

- `name` 必须在同一 `Executor` 中与 async、RT、GPU 和其他 I/O kairo 名称唯一。
- `BlockingIoConfig::thread_name` 不可为空；`startup_timeout` 不可为负，`0` 表示不等待 ready。
- `register_blocking_io_worker` / `start_blocking_io_worker` 以 `InvalidConfig`、`DuplicateName`、`NotFound`、`AlreadyInitialized` 或 `StartFailed` 说明拒绝原因。
- `stop_blocking_io_worker()` 执行 stop request、`wakeup()` 和 join；不 detach worker。重复停止安全。
- `Executor::shutdown()` 也会请求停止、唤醒并 join 所有已注册 I/O worker，即使传入 `shutdown(false)`。
- 新代码可用 `start_worker(BlockingWorkerSpec{name, config, std::move(worker)})` 原子完成注册和启动。返回的 `WorkerHandle` 暴露 `start_result()`、`started()`、`request_stop()`、`stop()` 和 `status()`；它表示长期 worker 的生命周期，绝不是单次任务完成 future。

#### 4.5.3 配置与状态

| 类型/字段 | 含义 |
| --- | --- |
| `BlockingIoConfig::thread_name` | 必填的线程名称 |
| `cpu_affinity` | 可选 affinity；空值保持 OS 调度 |
| `enable_memory_lock` | 默认 `false`；仅在显式请求时尝试进程级 `mlockall`，会影响当前和后续进程映射。锁定通过引用计数租约持有（`ProcessMemoryLockLease`）：worker 停止时释放本执行器的租约，进程内最后一个持有执行器（实时或阻塞 I/O）停止时才调用 `munlockall`，多个执行器并发启停不会互相解除锁定 |
| `startup_timeout` | 默认 1000 ms；`0` 不等待 ready，正值限制启动等待 |
| `BlockingIoExecutorStatus::ready` | kairo 线程已建立并完成线程属性设置；不表示协议、设备或业务数据已就绪 |
| `wakeup_count` | kairo 调用 worker `wakeup()` 的累计次数 |
| `stop_reason` | `Requested`、`WorkerReturned`、`WorkerException` 或 `StartFailed` 等生命周期结果 |

运行期协议错误、队列背压、数据年龄和设备安全动作不属于该状态；由使用方的数据面和应用逻辑定义。

---

## 5. 无锁任务执行器 API

### 5.1 概述

`LockFreeTaskExecutor` 是高性能的无锁任务执行器，支持 **MPSC（多生产者单消费者）** 模式。通过无锁队列和 CAS 操作避免互斥锁开销，提供极低延迟和高吞吐。

**适用场景**：
- 高频日志收集（多线程写入日志）
- 实时事件处理（多个事件源）
- 传感器数据采集（多传感器并发）
- 多线程环境下的任务聚合
- 性能敏感的异步任务分发

**技术特性**：
- 支持多个线程并发调用 `push_task()`
- 单个消费者线程处理任务
- 使用 CAS (Compare-And-Swap) 保证线程安全
- 提交路径除队列与对象池自身的原子操作外无任何互斥锁（对象池为 tagged
  索引 freelist，无锁 acquire/release）
- 消费者线程空闲时进入 futex 驻停：长期空闲的执行器 CPU 占用近零；
  间隔数百微秒的零星任务由退避缓冲带轮询捡起，任务提交后由生产者
  定向唤醒
- 完全向后兼容单生产者场景

**限制**：
- 固定队列容量，满时提交失败
- 仅支持 `std::function<void()>` 任务
- 单消费者（不支持多消费者）

### 5.2 包含头文件

```cpp
#include <kairo/lockfree_task_executor.hpp>
```

### 5.3 基本用法

#### 单生产者场景（SPSC）

```cpp
// 创建执行器（队列容量1024）
kairo::LockFreeTaskExecutor exec(1024);

// 启动消费者线程
exec.start();

// 提交任务
bool success = exec.push_task([]() {
    // 任务逻辑
});

if (!success) {
    // 队列满，处理背压
}

// 停止执行器（会处理剩余任务）
exec.stop();
```

#### 多生产者场景（MPSC）

```cpp
kairo::LockFreeTaskExecutor exec(4096);
exec.start();

// 多个线程可以安全地并发提交任务
std::vector<std::thread> producers;
for (int i = 0; i < 4; ++i) {
    producers.emplace_back([&exec, i]() {
        for (int j = 0; j < 1000; ++j) {
            exec.push_task([i, j]() {
                // 处理任务
                std::cout << "Thread " << i << " task " << j << "\n";
            });
        }
    });
}

for (auto& t : producers) {
    t.join();
}

exec.stop();
```

### 5.4 API 接口

```cpp
class LockFreeTaskExecutor {
public:
    explicit LockFreeTaskExecutor(size_t queue_capacity = 1024,
                                  size_t backoff_multiplier = 2,
                                  bool enable_stats = false);
    ~LockFreeTaskExecutor();

    bool start();                                    // 启动消费者线程；stop() 后不可再次启动
    void stop();                                     // 停止接收新任务、处理已接受任务并等待
    bool stop_and_join();                            // 外部线程等待；消费者线程内返回 false
    bool is_running() const;                         // 检查运行状态

    // 单任务提交（线程安全，支持多生产者并发）
    // stop() 开始后返回 false；未 start 前仍可用于预填充队列
    bool push_task(std::function<void()> task);

    // 批量提交（原子语义：全成或全败）
    // tasks: 任务数组指针；count: 数组长度；pushed: 实际入队任务数（输出）
    // 返回值：true = 全部入队（pushed == count）；
    //         false = 空输入、stop() 已开始、内部对象池耗尽或队列空间不足，
    //                 没有任务入队，pushed 保持 0
    bool push_tasks_batch(const std::function<void()>* tasks,
                          size_t count,
                          size_t& pushed);

    size_t pending_count() const;                    // 队列中待处理任务数（近似值）
    uint64_t processed_count() const;                // 已处理任务总数

    QueueStats get_queue_stats() const;              // 队列状态快照；字段是否依赖 enable_stats 见下表
    QueueStats get_status_snapshot() const;          // O(1)、可复制的非同步状态快照
    QueueStats expensive_diagnostic_snapshot() const; // O(capacity)、低频逐槽位诊断快照

    // 异常观测与自定义处理（异常计数不依赖 enable_stats）
    // exception_count() 返回 get_queue_stats() 期间累积的 task 异常次数
    // set_exception_handler() 允许替换默认「记录到 stats + 忽略」的行为,
    // 改由用户回调处理(例如记录到全局 logger、计数、转发到 ThreadPool 兜底)
    // QueueStats 字段参考下方 5.5 节。
    uint64_t exception_count() const;
    uint64_t rejected_empty_count() const;            // 空任务提交拒绝次数
    void set_exception_handler(std::function<void(std::exception_ptr)> handler);
};
```

#### 状态快照与背压诊断

`get_status_snapshot()` 返回一个可按值复制的 `QueueStats`，适合由监控线程采样；它不等待生产者或消费者。快照由多个独立原子读取组成，**所有字段均为近似、非同步值**：并发读写可在采样期间推进，字段不保证来自同一时刻，不能作为同步或正确性判定依据。`reserved_count` 和 `ready_count` 由状态转换维护的 relaxed 原子计数提供，因此采样为 O(1)，并发时可能轻微漂移但不会遗漏状态转换事件。`get_queue_stats()` 返回相同的值类型快照。

`expensive_diagnostic_snapshot()` 是公开的低频排障接口：它扫描全部槽位，复杂度为 O(capacity)，**不可在热路径或每次监控采样中调用**。它返回相同的 `QueueStats`，但 `reserved_count` 是扫描得到的 `Reserved`/`Writing` 槽位数，`ready_count` 是扫描得到的 `Published` 槽位数；可用于 dump 卡住的 reservation。逐槽位状态计数需要构造时传入 `enable_stats=true`，且仍是非同步快照。

**底层队列统计（均需 `enable_stats=true`）**：未启用时，底层队列返回零值，因此下列字段不提供有效的队列统计。

| `QueueStats` 字段 | 含义与使用建议 | 需要 `enable_stats=true` |
|---|---|---|
| `total_pushes` / `failed_pushes` / `total_pops` / `empty_pops` | 底层队列累计操作计数。`failed_pushes` 是所有底层入队失败的总和，等于下三栏之和。 | 是 |
| `queue_full_rejections` / `contention_rejection` / `reservation_cancelled_rejections` | 三类底层入队失败原因：队列满、CAS 竞争耗尽重试预算、消费者取消了 producer 的 reservation。 | 是 |
| `batch_pushes` / `batch_pops` | 底层队列批操作计数。 | 是 |
| `current_size` / `peak_size` | 当前近似积压 / 历史峰值。 | 是 |
| `reserved_count` / `ready_count` | 尚未发布的预留槽位 / 已发布可消费槽位；持续增长分别提示生产者停滞或消费者积压。 | 是 |
| `reservation_count` / `reservation_wait_yields` | reservation 操作及等待让出的辅助诊断计数。 | 是 |
| `cancelled_reservation_count` | 消费者恢复停滞 reservation 时取消的次数。 | 是 |
| `fail_reason` | 最近一次底层队列失败原因（`QueueFull` / `Contention` / `ReservationCancelled` / `None`）。 | 是 |

**执行器生命周期、拒绝与异常统计（均不需 `enable_stats=true`）**：这些字段由执行器在读取底层队列统计后独立填充，始终可观察。

| `QueueStats` 字段 | 含义与使用建议 | 需要 `enable_stats=true` |
|---|---|---|
| `queue_capacity` | 调整为 2 的幂后的环形缓冲容量；实际可用槽位为该值减一（环形缓冲保留一个空槽），对象池按同一取整后容量分配，因此提交背压上限就是 `queue_capacity - 1`。例如构造请求 5 时环容量取整为 8，最多可同时入队 7 个任务。 | 否（始终可读） |
| `submission_rejection` | 进入队列前的拒绝：空任务、停止后提交或对象池耗尽；始终累计。 | 否（始终可读） |
| `exception_count` | 任务执行期间累计捕获的异常次数；也可由 `exception_count()` 读取。 | 否（始终可读） |
| `rejected_empty_count` | 因空 `std::function` 输入被拒绝的累计次数；也可由 `rejected_empty_count()` 读取。 | 否（始终可读） |
| `success_rate` | `total_pushes / (total_pushes + failed_pushes)`；始终返回，但未启用统计时分子和分母均为零，结果为 `0.0`，不表示实际队列成功率。 | 否（始终可读；有效值需启用统计） |

因此，即使未启用底层队列统计，也可以观察实际容量、执行器入口拒绝和任务异常；启用统计后还可诊断容量压力（`queue_full_rejections`）、CAS 竞争（`contention_rejection`）、reservation 取消恢复（`reservation_cancelled_rejections` / `cancelled_reservation_count`）及底层队列操作结果。

```cpp
kairo::LockFreeTaskExecutor exec(4096, 2, true);
const auto status = exec.get_status_snapshot();
if (status.ready_count > status.queue_capacity / 2) {
    // 消费者可能落后；考虑扩容或限流。
}

// 仅在告警后低频调用（例如每秒最多一次），定位卡住的 producer reservation。
const auto diagnostic = exec.expensive_diagnostic_snapshot();
if (diagnostic.reserved_count != 0) {
    // Reserved/Writing 槽位持续存在时，检查生产者是否卡在发布前。
}
```

**背压告警示例**：可在 `ready_count > queue_capacity / 2` 连续多个采样周期时发出“消费者落后”预警；`ready_count > queue_capacity * 3 / 4` 时限流或扩容。`reserved_count > 0` 本身只表示某个 producer 正在发布；若它跨多个低频诊断周期持续不降，再收集 `expensive_diagnostic_snapshot()` 和 `cancelled_reservation_count` 排查卡住 reservation。

#### `push_tasks_batch` 详解

| 项目 | 说明 |
|------|------|
| 时间复杂度 | O(count)，一次性申请所有 TaskWrapper，组装后单次调用 exact batch 入队 |
| 线程安全 | 与 `push_task` 相同，线程安全，可多生产者并发调用 |
| 原子语义 | 返回 true 时 `pushed == count`；返回 false 时没有任务入队且 `pushed == 0`。队列空间不足时不会部分入队 |
| 返回 false 时机 | (a) `tasks == nullptr` 或任一 `tasks[i]` 为空；(b) `stop()` 已开始，执行器拒绝新任务；(c) 对象池（ObjectPool）容量不足以一次性分配 count 个 wrapper；(d) 队列剩余空间不足以容纳整个 batch；或 (e) 消费者取消了尚未提交的 reservation。无论原因是什么，整个 batch 都保持不可见，不会有任务入队，`pushed` 为 0 |
| 批量统计 | 每次成功的 `push_tasks_batch` 调用会令 `get_queue_stats().batch_pushes` 递增 1，`total_pushes` 递增 `count`（P-260623-004：与队列 batch 统计语义一致） |
| 空任务统计 | 空任务属于提交拒绝，不进入队列，不增加 `processed_count()` 或 `exception_count()`；可通过 `rejected_empty_count()` 或 `get_queue_stats().rejected_empty_count` 观察 |

#### 退避倍率

`LockFreeTaskExecutor` 将 `backoff_multiplier` 传递给底层 `LockFreeQueue`，用于放大 CAS 失败后的 pause 退避次数。

- `backoff_multiplier` 必须 `> 0`；传入 `0` 会在构造时抛出 `std::invalid_argument`。
- 最大值为 `LockFreeQueue::kMaxBackoffMultiplier`，当前为 `1u << 20`（`1048576`）。
- 大于最大值的输入会被钳制到 `LockFreeQueue::kMaxBackoffMultiplier`，避免内部 `backoff * backoff_multiplier` 算术溢出并保持退避窗口有界。

#### MPSC 预留反压契约

底层 MPSC 队列采用**策略 (b)：保留取消恢复**。生产者取得槽位后，消费者会在该槽位仍为 `Reserved` 时最多 `yield` 64 次（`LockFreeQueue::kDefaultReservationWaitYields`）；若生产者仍未进入不可中断的写入窗口，消费者会显式将该槽位标记为取消并继续推进队列。因而 `push_task()` / `push_tasks_batch()` 可以在这个窗口返回 `false`，即使调用方已经获得了任务 wrapper；调用方必须把 `false` 当作未接受提交并自行重试、回收或降级处理。

启用统计后，`get_queue_stats()` 会提供：

- `reserved_count`：当前仍处于 `Reserved` 或 `Writing` 的槽位数（瞬时快照）。
- `reservation_count`：累计成功预留槽位数；解析后的守恒关系为 `reservation_count == total_pushes + cancelled_reservation_count`。
- `ready_count`：当前已发布、可消费的槽位数（瞬时快照）。
- `failed_pushes`：所有底层入队失败的累计数；静止快照中等于三个失败原因计数之和。
- `queue_full_rejections`：队列满导致的提交拒绝数。
- `contention_rejection`：CAS 竞争耗尽重试预算导致的提交拒绝数。
- `reservation_cancelled_rejections`：已预留槽位被消费者取消导致的提交拒绝数。
- `cancelled_reservation_count`：消费者在有界等待后取消的预留数。
- `reservation_wait_yields`：当前有界等待预算（默认 64）。
- `fail_reason`：最近一次生产者入队失败的 `LockFreeTaskExecutor::QueueFailReason`：`None`、`QueueFull`、`Contention` 或 `ReservationCancelled`。它是最近值而非累计直方图，应结合上述计数使用。

这些统计仅在构造执行器时传入 `enable_stats=true` 后有效；所有瞬时槽位计数均为并发采样，不能用作同步原语。

#### 停止后的提交语义

`LockFreeTaskExecutor` 区分“从未启动”和“已停止”状态：从未调用 `start()` 前仍允许 `push_task()` / `push_tasks_batch()` 预填充队列；一旦 `stop()` 开始，新的提交会被拒绝并返回 `false`。提交路径由一个原子准入门闩保护：“检查是否已停止”与“登记为活动生产者”是同一个原子操作（单一状态字：高位为关闭标志，低位为活动生产者计数）。因此外部线程调用 `stop()` 或 `stop_and_join()` 会等待所有已通过门闩登记的生产者完成，再让消费者线程处理所有已接受任务并退出；`stop_and_join()` 返回后不会再有任何生产者访问执行器成员，对象可以安全销毁，也不会有静默接受但无人消费的任务残留在队列中。

任务或实时周期回调可安全调用 `stop()` / `stop_and_join()` 请求自停止：`stop_and_join()` 在工作线程内返回 `false`，不会尝试等待自身；随后由外部线程调用它完成 join。自停止会丢弃当前批次中尚未执行的任务以及剩余队列，避免在已请求停止后继续 drain。

**典型用法：**

```cpp
kairo::LockFreeTaskExecutor exec(4096);
exec.start();

// 准备批量任务
std::vector<std::function<void()>> tasks;
tasks.reserve(100);
for (int i = 0; i < 100; ++i) {
    tasks.push_back([i]() { process(i); });
}

// 批量提交，检查实际入队数
size_t pushed = 0;
bool ok = exec.push_tasks_batch(tasks.data(), tasks.size(), pushed);
if (!ok) {
    // 空输入、对象池耗尽、队列空间不足或 stop() 后拒绝；
    // 没有任何任务入队，需要等待、修正输入或降级处理
} else {
    // pushed == tasks.size()
}
```

### 5.5 性能特性（LockFreeTaskExecutor）

> 当前提交的可复现基线：[tests/benchmarks/baselines/db589fb.json](../tests/benchmarks/baselines/db589fb.json)。该记录由 `benchmark_lockfree_mpsc` 在 Release 构建中采集；队列容量为 16384，`enable_stats=false`，reservation 等待让出预算为 64。

#### MPSC 提交路径（单生产者）

| 指标 | 值 |
|------|-----|
| P50 提交调用延迟 | 86 ns |
| P99 提交调用延迟 | 92 ns |
| 成功提交吞吐量 | 9,050,750 ops/s |

**指标定义**：一个生产者线程和一个 `LockFreeTaskExecutor` 消费线程运行约 1 秒；吞吐量是成功 `push_task()` 调用数每秒。每第 100 次成功调用采样一次，延迟为生产者侧 `push_task()` 调用耗时，**不是**任务从提交到开始执行的端到端延迟。此结果固定到采集主机的 CPU 24、`powersave` governor；不同 CPU、负载和 pinning 会改变结果。完整的多生产者原始结果、编译器和平台元数据在 sidecar 中。

### 5.6 最佳实践

#### ✅ 推荐做法

**1. 选择合适的生产者数量**
```cpp
// 推荐：1-2 个生产者
kairo::LockFreeTaskExecutor exec(4096);

// ✅ 单生产者：最佳性能
std::thread producer([&]() {
    exec.push_task([]() { /* ... */ });
});

// ✅ 2个生产者：性能良好
std::thread p1([&]() { exec.push_task([]() { /* ... */ }); });
std::thread p2([&]() { exec.push_task([]() { /* ... */ }); });
```

**2. 合理设置队列容量**
```cpp
// 根据任务频率和处理速度设置
// 低频场景：1024-2048
kairo::LockFreeTaskExecutor low_freq(1024);

// 高频场景：4096-16384
kairo::LockFreeTaskExecutor high_freq(8192);

// 容量非 2 的幂时会向上取整（如 5 → 8），对象池与可用槽位按取整后容量统一
```

**3. 正确处理队列满的情况**
```cpp
bool success = exec.push_task([]() { /* ... */ });
if (!success) {
    // 策略1：重试（适合关键任务）
    while (!exec.push_task([]() { /* ... */ })) {
        std::this_thread::yield();
    }

    // 策略2：丢弃（适合日志等非关键任务）
    // 直接忽略

    // 策略3：降级（适合有备选方案的场景）
    // 使用其他执行器或同步执行
}
```

**4. 避免在 lambda 中捕获悬空引用**
```cpp
std::atomic<int> counter{0};

// ❌ 错误：捕获局部变量引用
{
    int local_var = 42;
    exec.push_task([&local_var]() {
        // local_var 可能已被销毁！
        std::cout << local_var << "\n";
    });
}

// ✅ 正确：捕获值或使用全局/静态变量
exec.push_task([value = 42]() {
    std::cout << value << "\n";
});

// ✅ 正确：捕获 shared_ptr
auto data = std::make_shared<int>(42);
exec.push_task([data]() {
    std::cout << *data << "\n";
});

// ✅ 正确：使用原子变量
exec.push_task([&counter]() {
    counter.fetch_add(1);
});
```

**5. 确保正确的生命周期管理**
```cpp
// ✅ 正确：执行器在生产者之前创建，之后销毁
{
    kairo::LockFreeTaskExecutor exec(1024);
    exec.start();

    std::thread producer([&]() {
        exec.push_task([]() { /* ... */ });
    });

    producer.join();
    exec.stop();  // 会处理剩余任务
}  // 执行器在这里销毁
```

#### ⚠️ 注意事项

**1. 避免过多生产者**
```cpp
// ❌ 不推荐：4+ 生产者效率低
std::vector<std::thread> producers;
for (int i = 0; i < 16; ++i) {  // 效率仅 1-2%
    producers.emplace_back([&]() {
        exec.push_task([]() { /* ... */ });
    });
}

// ✅ 推荐：使用线程池或批量提交
ThreadPoolConfig config;
config.min_threads = 16;
config.max_threads = 16;

ThreadPool pool;
pool.initialize(config);
pool.submit([&]() {
    // 单个线程批量提交
    for (int i = 0; i < 1000; ++i) {
        exec.push_task([]() { /* ... */ });
    }
});
```

**2. 避免在任务中执行耗时操作**
```cpp
// ❌ 错误：阻塞消费者线程
exec.push_task([]() {
    std::this_thread::sleep_for(std::chrono::seconds(1));  // 阻塞！
    // 或者执行 I/O、网络请求等耗时操作
});

// ✅ 正确：任务应该是轻量级的
exec.push_task([]() {
    // 快速处理，微秒级
    process_data();
});

// ✅ 正确：耗时操作使用其他执行器
exec.push_task([&async_exec]() {
    async_exec.submit([]() {
        // 在其他线程执行耗时操作
        std::this_thread::sleep_for(std::chrono::seconds(1));
    });
});
```

**3. 注意 `pending_count()` 的近似性**
```cpp
// ⚠️ 注意：pending_count() 返回近似值
size_t count = exec.pending_count();
// 在多生产者场景下，实际值可能略有不同
// 不要依赖精确值做关键决策
```

**4. 避免在析构函数中提交任务**
```cpp
class MyClass {
    kairo::LockFreeTaskExecutor& exec_;
public:
    ~MyClass() {
        // ❌ 危险：析构时提交任务可能导致问题
        exec_.push_task([this]() {
            // this 可能已被销毁！
        });
    }
};
```

### 5.7 异常行为和故障排查

#### 常见问题

**1. 段错误（Segmentation Fault）**

**原因**：Lambda 捕获了悬空引用
```cpp
// ❌ 问题代码
void bad_example() {
    kairo::LockFreeTaskExecutor exec(1024);
    exec.start();

    int local = 42;
    exec.push_task([&local]() {
        std::cout << local << "\n";  // local 可能已销毁
    });

    // 函数返回，local 被销毁
}
```

**解决方案**：
```cpp
// ✅ 方案1：按值捕获
exec.push_task([local]() {
    std::cout << local << "\n";
});

// ✅ 方案2：使用 shared_ptr
auto data = std::make_shared<int>(42);
exec.push_task([data]() {
    std::cout << *data << "\n";
});
```

**2. 任务丢失**

**原因**：队列满时未处理失败情况
```cpp
// ❌ 问题代码
exec.push_task([]() { /* ... */ });  // 忽略返回值
```

**解决方案**：
```cpp
// ✅ 检查返回值
if (!exec.push_task([]() { /* ... */ })) {
    // 处理失败：重试、丢弃或降级
}
```

**3. 性能下降**

**原因**：生产者过多导致 CAS 竞争
```cpp
// ❌ 问题代码：16个生产者，效率仅 1%
for (int i = 0; i < 16; ++i) {
    threads.emplace_back([&]() {
        exec.push_task([]() { /* ... */ });
    });
}
```

**解决方案**：
```cpp
// ✅ 减少生产者数量到 1-2 个
for (int i = 0; i < 2; ++i) {
    threads.emplace_back([&]() {
        exec.push_task([]() { /* ... */ });
    });
}
```

**4. 自停止语义误解**

**原因**：在任务内调用 stop 系列接口并假定它会等待消费者线程退出
```cpp
// ❌ 误用：任务内调用 stop_and_join() 并期望它完成 join
exec.push_task([&exec]() {
    exec.stop_and_join();  // 返回 false：只请求自停止，不会等待自身
});
```

**解决方案**：任务内调用 `stop()` / `stop_and_join()` 是安全的自停止请求，二者都不会等待消费者线程自身——`stop_and_join()` 在工作线程内立即返回 `false`，只请求停止；join 必须由外部线程完成（语义与 §5.4「停止后的提交语义」一致）
```cpp
// ✅ 任务内只请求自停止，外部线程负责最终 join
exec.push_task([&exec]() {
    exec.stop();  // 安全：请求停止并返回，不会死锁
});
// ... 外部线程稍后：
exec.stop_and_join();  // 返回 true，完成 join
```

### 5.8 性能调优建议

1. **队列容量**：根据峰值任务频率设置，避免频繁队列满
2. **生产者数量**：优先使用 1-2 个生产者
3. **任务粒度**：保持任务轻量级（< 10 微秒）
4. **内存对齐**：队列容量设为 2 的幂以优化性能
5. **CPU 亲和性**：考虑将消费者线程绑定到特定 CPU 核心

详细示例见 [examples/lockfree_task_executor_example.cpp](../examples/lockfree_task_executor_example.cpp)。

### 5.9 Facade 注册与生命周期（LowLatency 后端）

`LockFreeTaskExecutor` 不参与 facade 的默认路由；`dispatch_auto` 的
`ExecutionIntent::LowLatency` 需要一个**已启动**的具名无锁执行器作为
`preferred_executor`。注册与生命周期经 facade 完成：

```cpp
auto lf = std::make_unique<kairo::LockFreeTaskExecutor>(4096 /*capacity*/);
ex.register_lockfree_executor("logging", std::move(lf));  // 转移所有权
ex.start_lockfree_executor("logging");                    // 启动消费者线程

kairo::TaskOptions opt;
opt.intent = kairo::ExecutionIntent::LowLatency;
opt.preferred_executor = "logging";
auto r = ex.dispatch_auto(opt, [] { write_log(); });     // accepted = 队列已接收

ex.stop_lockfree_executor("logging");                     // 停止并等待 drain
```

| API | 语义 |
|-----|------|
| `register_lockfree_executor(name, std::unique_ptr<LockFreeTaskExecutor>)` | 注册并转移所有权；重名/空指针拒绝并记 `SubmitRejected` failure 事件 |
| `start_lockfree_executor(name)` | 启动消费者线程；不存在、已在运行或已停止返回 `false` 并记 failure 事件 |
| `stop_lockfree_executor(name)` | 请求停止并等待；可安全重复调用 |
| `get_lockfree_executor_names()` | 已注册执行器名称列表 |

不存在 `Executor::submit_lockfree`。低延迟提交统一走 `dispatch_auto`（§3.7）；
`accepted` 仅表示有界队列已接收，不代表任务完成。需要持有快照的集成代码使用
`ExecutorManager::get_lockfree_executor_snapshot(name)`（§9 高级接口）。

---

## 6. 监控 API

```cpp
void enable_monitoring(bool enable);
void set_monitoring_sampling_rate(double rate);
void set_in_flight_task_capacity(size_t capacity);
void set_in_flight_task_sampling_rate(double rate);

AsyncExecutorStatus get_async_executor_status() const;
RealtimeExecutorStatus get_realtime_executor_status(const std::string& name) const;
ExecutorSnapshot get_snapshot() const;
std::string get_snapshot_text() const;
void set_snapshot_diagnostic_callback(ExecutorSnapshotCallback callback);

TaskStatistics get_task_statistics(const std::string& task_type) const;
std::map<std::string, TaskStatistics> get_all_task_statistics() const;
```

- `enable_monitoring`：开启/关闭任务监控（默认可在 `ExecutorConfig::enable_monitoring` 配置）。
- `set_monitoring_sampling_rate`：设置监控采样率（0.0–1.0），1.0 表示每次任务都采样，较低值可减少监控开销。
- `set_in_flight_task_capacity`：设置 snapshot 保留的默认异步线程池在途诊断容量，默认 128；0 关闭在途表但不关闭聚合任务统计。满容量不会驱逐现有条目，而是累计 `in_flight_dropped_count` 并标记 snapshot 不完整。
- `set_in_flight_task_sampling_rate`：设置在途诊断独立采样率（0.0–1.0）；不改变 `TaskStatistics` 的采样率或任务执行语义。
- `get_async_executor_status`：线程池名称、运行状态、活跃/完成/失败任务数、队列大小、平均任务时间等。
- `get_realtime_executor_status`：实时线程名称、运行状态、周期、周期计数、超时计数、平均/最大周期时间等。
- `get_snapshot`：一次返回 Executor 生命周期、默认异步/实时/Blocking I/O/GPU 后端状态、失败摘要、最近失败事件、任务统计和聚合计数；不会触发默认异步执行器懒初始化。
- `get_snapshot_text`：以稳定的行式文本导出一次新采集的 snapshot，适合日志和故障支持包；JSON 不属于当前 API。
- `set_snapshot_diagnostic_callback`：设置超时及 facade 初始化、注册、启动失败时的低频现场回调。回调接收独立的 `ExecutorSnapshot` 值，并在触发操作的调用线程执行；异常被隔离，不能放入实时周期或任务热路径。
- `format_executor_snapshot_with_metrics`：位于 `kairo::monitor`，用于性能基线；返回文本、`formatting_duration`（纳秒）和 `formatting_allocation_count`。分配次数只统计 formatter 的流缓冲与最终输出字符串，不统计 snapshot 采集或调用方 logger 的分配；常规业务日志仍使用 `get_snapshot_text()`。
- `get_task_statistics` / `get_all_task_statistics`：按 `task_type` 或全部的成功/失败/超时次数及执行时间统计。
- `get_cancellation_status`：取消生命周期独立计数（`request_count`、`queued_cancelled_count`、`running_request_count`、`completed_after_request_count`），不并入 `ExecutorFailureStatus`。
- `closure_graveyard_size`：parked 超时闭包转入的延迟析构墓地当前规模（诊断观测）。规模以"parked 超时触发 + 竞争输家"次数为界，`shutdown()` 终局清空、facade 析构释放；长期高频超时的进程可借此观测常驻内存累积。
- `get_timer_status_summary`：定时任务计数（`pending_count`、`executed_count`、`cancelled_count`），同样独立于 failure 体系。

### 6.1 完整生命周期快照

`get_snapshot()` 是低频、只读的 best-effort 诊断接口，适合健康检查、等待/关闭超时现场和故障支持包的状态采集：

```cpp
const auto snapshot = kairo.get_snapshot();
if (snapshot.lifecycle == kairo::ExecutorLifecycleState::Failed ||
    snapshot.partial) {
    // 保存 snapshot 中的 lifecycle、failures、recent_failures 和后端状态
}
```

等待或关闭超时时，可以同时保留 `WaitResult` 中的完整现场，并通过回调把相同快照交给日志/支持包线程：

```cpp
kairo.set_snapshot_diagnostic_callback(
    [](const kairo::ExecutorSnapshot& snapshot) {
        // 低频写入日志或故障支持包；不要在实时周期线程中序列化。
        std::cerr << kairo::monitor::format_executor_snapshot(snapshot);
    });

const auto wait = kairo.wait_for_completion(std::chrono::seconds{2});
if (wait.timed_out && wait.diagnostic_snapshot) {
    // 该快照与回调收到的快照具有相同的 snapshot_sequence。
    save_diagnostic(*wait.diagnostic_snapshot); // 应用层支持包写入函数
}
```

`ExecutorSnapshot` 固定包含 `schema_version`（当前为 **3**）、单实例内单调递增的
`snapshot_sequence`、采集开始时间 `captured_at`、采集耗时 `collection_duration`（纳秒）、生命周期状态、`partial` /
`consistency_note` 和采集前后校验的 `state_epoch`、`completion`、`async`、`realtime`、`blocking_io`、`gpu`、
`failures`、`recent_failures`、`task_statistics`、有限采样的 `in_flight_tasks` 及其计数，
以及运行/停止后端数、活跃/排队/失败/丢弃工作数。schema 3 新增独立生命周期计数
`cancellation`（`CancellationStatus`）与 `timers`（`TimerStatusSummary`）；
取消与定时取消不并入 `failures` 计数。快照文本对应行前缀为 `cancellation.*`
与 `timers.*`。

在途诊断目前覆盖默认异步线程池和 facade 任务图。普通任务被接受后依次可见为 `Queued` 和 `Running`；`TaskHandle` 在依赖未满足时可见为 `Pending` / `DependencyBlocked`。终态即从表中移除；`in_flight_count`、`in_flight_state_counts` 和 `oldest_in_flight_age` 用于定位队列积压、依赖阻塞和慢任务。`in_flight_tasks` 不含 callable、payload、异常对象或依赖列表。它是有界采样结果，`in_flight_diagnostics_incomplete=true` 或 snapshot `partial=true` 时不得视为完整任务清单。实时、GPU、Blocking I/O 使用各自 backend-specific 状态：realtime 的运行/容量/drop/rejection，GPU 的 active/queued/completed/failed kernel，以及 Blocking I/O 的 running/ready/stop reason/error；它们未伪装成普通 future 任务。

`TaskLifecycleState` 的稳定字符串为 `Pending`、`Queued`、`Running`、`Succeeded`、`Failed`、`TimedOut`、`Rejected`、`Cancelled` 和 `DependencyBlocked`。当前在途表只保留非终态条目；`Rejected`、`Succeeded`、`Failed`、`TimedOut` 和 `Cancelled` 通过 failure/完成统计或 backend 状态观察，不作为无限终态历史。

生命周期状态为 `Created`、`Initializing`、`Running`、`Draining`、`Stopped` 或
`Failed`。它是跨后端摘要，不替代具体后端的 `is_running`、停止原因或队列字段。

快照按 provider 独立读取，不承诺跨所有后端的事务级一致性。Manager 在采集前后
读取轻量 `state_epoch`，发生注册表或生命周期边界变化时最多重试两次；仍不稳定、
provider 不可用或读取异常会设置 `partial=true`，并在 `consistency_note` 中说明
（持续变化时包含 `epoch_changed`）。epoch 不因任务计数变化递增，因此不会让正常
运行中的快照持续重试。
快照不包含任务 callable、业务 payload 或通信 payload，也不应在实时周期线程中调用；
`in_flight_tasks` 只在其有限采样容量内保存任务标识与状态元数据。

### 6.2 快照文本与性能基线

`get_snapshot_text()` 每次调用都会采集并格式化一份新快照，适合低频日志和故障支持包，输出顺序稳定、时间字段带 `*_ns` 或 `*_ms` 单位，枚举输出稳定字符串。JSON 导出不属于当前 API。

性能基线使用独立的 `benchmark_lifecycle_snapshot`，不会改变生产路径：

```text
./build/tests/benchmark_lifecycle_snapshot --iterations 1000
```

输出包含 idle initialized async 场景的快照采集 wall/reported 平均耗时、文本格式化 wall/reported 平均耗时、格式化器本地平均分配次数和输出字节数。该结果受 CPU、编译选项、后端注册数量和失败/统计条目数量影响，只用于同一环境的前后对比，不是性能保证。

---

## 7. 配置与类型

### 7.0 Facade 哲学：默认即最优，失败可观察

kairo 库遵循以下原则 (P019 三阶段 + P019C companion):

1. **默认即最优** — 零配置用户拿到平台/负载下最好的行为
2. **自动决策** — 库在内部探测环境（`hw_concurrency`、timer slack）选最优路径
3. **自动降级可诊断** — 平台探测或系统级调优不可用时退到安全默认，不把调优失败伪装成任务失败
4. **任务失败可观察** — 任务异常、提交拒绝、实时队列丢任务、超时等运行时失败必须通过 `future`、返回值、状态计数或监控统计暴露；调用方可以选择不处理，但库不应让失败无迹可寻
5. **用户覆盖** — 显式设的非默认/非空值永远保留

实现：

- `ThreadPoolConfig.min_threads` / `max_threads` = 0（sentinel，自适应）
- `ThreadPoolConfig.enable_work_stealing` = `true`（默认开）
- `ThreadPoolConfig.cpu_affinity` 空 → auto-allocate [0..hw-1]；Android 使用当前线程允许 cpuset
- `RealtimeThreadConfig.enable_process_memory_lock` = `false`（默认不调用进程级 `mlockall`；仅在显式启用并接受其资源影响时请求）
- `RealtimeThreadConfig.timer_slack_ns` = 1（尽力设置 1 ns；不可用或权限不足时安全回退）
- `RealtimeThreadConfig.cpu_affinity` 空 → 通过 `g_next_rt_cpu_hint` 在当前允许 CPU 集合内 round-robin 自动选择；若可用 CPU 数量 <= 1，则不设置亲和性
- `RealtimeThreadConfig.thread_priority` = 0 → 自适应按 `cycle_period_ns` 建议；Android 保持普通调度，显式设值才尝试
- `task_timeout_ms > 0`: 软超时 (执行前 skip + 记录 timeout_count; future 抛 `TimedOutException`; 不计入 fail_count; C++ 无安全 kill 机制, 执行中不强制中断)

### 7.1 ExecutorConfig / ThreadPoolConfig（线程池配置）

用于 `Executor::initialize()` / `ExecutorConfig` / `ThreadPoolConfig`：

| 字段 | 类型 | 默认 | 说明 |
|------|------|------|------|
| `min_threads` | `size_t` | `0` | 0 = 自适应 sentinel；按 `hw_concurrency` 计算（min 2）；Android 按不超过 4 核的调度预算计算 |
| `max_threads` | `size_t` | `0` | 0 = 自适应 sentinel；默认 hw；Android 默认上限 4；探测失败退到 (2, 4) |
| `queue_capacity` | `size_t` | `1000` | 任务队列容量 |
| `thread_priority` | `int` | `0` | 线程优先级（Linux SCHED_FIFO 1–99，Windows `SetThreadPriority`；Android best-effort，默认不自动提升） |
| `cpu_affinity` | `std::vector<int>` | 空 | 空 = 自适应 sentinel；桌面 Linux 自动填 [0..hw-1]；Android 取 `sched_getaffinity` 允许 cpuset，失败则保持 OS 自由调度；显式设值保留 |
| `task_timeout_ms` | `int64_t` | `0` | > 0: 软超时 (执行前 check elapsed >= timeout 则 skip + 记录 timeout_count; 暴露的 future 抛 `TimedOutException`; 不计入 fail_count; 0 = 不超时; 注意: 执行中不强制中断, C++ 无安全 kill 机制) |
| `enable_work_stealing` | `bool` | `true` | 无锁工作窃取；`max_threads == 1` 时自动关；-10.7% 性能退化关闭 |
| `enable_monitoring` | `bool` | `true` | 是否启用监控 |
| `task_graph_retention_capacity` | `size_t` | `1024` | 已完成任务图 handle 的保留上限；`0` 表示终态 handle 立即过期，活动依赖不会提前回收 |
| `max_in_flight_tasks` | `size_t` | `0` | facade 默认异步提交的总量在途上限（scheduler + 本地队列 + 执行中）；`0` = 不启用（零热路径开销）。与 `queue_capacity`（每 worker 本地队列）语义无关，见 §3.11 |

内部动态 resize 扩容时，新增 worker 的负载元数据会重置为零负载，并将 `last_update` 初始化为当前 `std::chrono::steady_clock::now()`。

### 7.2 RealtimeThreadConfig（实时线程）

用于 `register_realtime_task()`：

| 字段 | 类型 | 说明 |
|------|------|------|
| `thread_name` | `std::string` | 线程名称（Linux 通过 `pthread_setname_np` 设置；Android bionic 同样可用，便于诊断工具识别） |
| `cycle_period_ns` | `int64_t` | 周期（纳秒），如 2 000 000 表示 2 ms |
| `thread_priority` | `int` | 线程优先级（如 SCHED_FIFO 1–99）；== 0 时按 `cycle_period_ns` 自适应建议（≤1 ms → 80，≤10 ms → 50，>10 ms → 0）；Android 默认保持普通调度，显式设值仍 best-effort 尝试 |
| `cpu_affinity` | `std::vector<int>` | CPU 亲和性；空 = 自适应 sentinel，实时线程 start 时通过 `g_next_rt_cpu_hint` 在当前允许 CPU 集合内 round-robin 自动选择；Android 的允许集合受 cgroup/SELinux 限制；显式设值保留。Windows 上逻辑 CPU 编号按处理器组扩展（组 g 覆盖 `g*64 .. g*64+组内数-1`），支持大于 64 CPU 的多处理器组主机；单线程亲和性只能落在同一组内，跨组请求（如 `{0, 64}`）整体拒绝并体现为 `cpu_affinity_applied = false` |
| `cycle_callback` | `std::function<void()>` | 每周期执行的回调 |
| `cycle_manager` | `ICycleManager*` | 可选，外部周期管理器；默认 nullptr 使用内置周期 |
| `max_tasks_per_cycle` | `uint64_t` | 单周期内最多处理的任务数；`0` 表示不限（保留旧行为，但生产环境建议 > 0 以保周期确定性）；默认 64 |
| `enable_allocation_guard` | `bool` | Linux 诊断构建中在 `cycle_callback` 外挂载记录型分配 guard；默认 `false`，仅在构建时启用 `KAIRO_ENABLE_REALTIME_ALLOCATION_GUARD` 后生效。它不构成实时安全证明。 |
| `enable_process_memory_lock` | `bool` | 是否显式请求 Linux `mlockall(MCL_CURRENT \| MCL_FUTURE)`；这是进程级操作，会锁定当前映射及后续映射，默认 `false`。权限或 `RLIMIT_MEMLOCK` 不足时安全回退；Android 普通 App 通常无权限，同样只报告状态。锁定通过引用计数租约持有：实时线程退出时释放本执行器的租约，进程内最后一个持有执行器（实时或阻塞 I/O）停止时才 `munlockall`；若进程在库之外自行 `mlockall`，最后一个租约释放时的 `munlockall` 同样会解除外部锁定 |
| `timer_slack_ns` | `uint64_t` | Linux timer slack（纳秒）；默认 1（1 ns，尽力设置，不可用或权限不足时安全回退）；`0` = 显式 opt-out 保留内核默认 |

### 7.3 状态与统计类型

- **AsyncExecutorStatus**：`name`、`is_running`、`active_tasks`、`completed_tasks`、`failed_tasks`、`queue_size`、`avg_task_time_ms`。`failed_tasks` 表示底层异步执行器已执行并以失败结束的任务数；通过 `Executor` facade 提交的用户任务异常也会让 wrapper 重新抛出，因此会计入该字段，同时计入 facade 的 `ExecutorFailureStatus::task_exception_count`。执行前软超时使用独立 timeout 计数，不计入 `failed_tasks`。
- **ThreadPoolStatus**：`include/kairo/config.hpp:90` 仍定义此结构（与 `AsyncExecutorStatus` 字段几乎重合），并且仍是底层 `ThreadPool::get_status()` 的返回类型；`ThreadPoolExecutor::get_status()` 会读取它并映射为 `AsyncExecutorStatus`。通过 `Executor` facade 或异步执行器编写的新代码优先使用 `AsyncExecutorStatus`；直接使用底层 `ThreadPool` 时仍应按 `ThreadPoolStatus` 处理。若未来要弃用或移除该类型，需要先提供替代的底层状态 API，并在声明处添加 deprecation 标记。
- **RealtimeExecutorStatus**：
  - `name` (std::string)：执行器名称。
  - `is_running` (bool)：是否运行中。
  - `cycle_period_ns` (int64_t)：配置周期（纳秒）。
  - `cycle_count` (int64_t)：累计周期计数。
  - `cycle_timeout_count` (int64_t)：超时周期计数。
  - `avg_cycle_time_ns` (double)：平均周期执行时间（纳秒）。
  - `max_cycle_time_ns` (double)：最大周期执行时间（纳秒）。
  - `priority_applied` / `cpu_affinity_applied` / `timer_slack_applied` (bool)：请求的实时优先级、CPU 亲和性和 timer slack 是否成功应用；未请求或平台不支持/权限不足时为 `false`，用于将调优降级显式上报。
  - `process_memory_lock_applied` (bool)：显式请求的进程级 `mlockall` 是否成功应用；未请求、平台不支持或权限不足时为 `false`。
  - `process_memory_lock_errno` (int)：请求 `mlockall` 失败时保留的 errno；未请求或成功时为 `0`，例如权限或 `RLIMIT_MEMLOCK` 限制可据此诊断。
  - `dropped_task_count` (uint64_t)：总拒绝/丢弃量，覆盖空任务、未运行/已停止、对象池耗尽和队列满四类来源；**始终累计**，不受 `enable_stats` 影响。它不等同于背压：背压仅由 `pool_exhausted_count` 和 `queue_full_count` 构成；应单独分析 `rejected_not_running_count` 与 `rejected_empty_task_count`，以区分生命周期状态拒绝和无效输入。
  - `failed_pushes` (uint64_t)：LockFreeQueue 所有底层失败入队尝试数（仅 `enable_stats=true` 时统计），包括队列满、CAS 竞争和 reservation 取消；它不等同于也不一定是 `dropped_task_count` 的子集。
  - `peak_queue_size` (uint64_t)：队列峰值长度（仅 `enable_stats=true`）。
  - `queue_capacity` (uint64_t)：RT 无锁队列固定容量（结合 `queue_full_count` 分析队列背压比例）。
  - `rejected_not_running_count` (uint64_t)：未运行/已停止时拒绝的累计数。
  - `rejected_empty_task_count` (uint64_t)：空任务拒绝累计数。
  - `pool_exhausted_count` (uint64_t)：对象池耗尽拒绝累计数。
  - `queue_full_count` (uint64_t)：队列满拒绝累计数。
- **TaskStatistics**：`total_count`、`success_count`、`fail_count`、`timeout_count`、`total_execution_time_ns`、`max_`/`min_execution_time_ns`。执行前软超时增加 `timeout_count`，不增加 `fail_count`。
- **ExecutorFailureStatus**：`task_exception_count`、`submit_rejected_count`、`timeout_count`、`realtime_drop_count`、`gpu_failure_count`、`wait_timeout_count`、`tuning_fallback_count`、`capacity_exhausted_count`、`total_count`。`wait_for_completion()` 或 `try_wait_for_completion(timeout)` 等待超时时记录 `FailureKind::WaitTimeout` 并增加 `wait_timeout_count`；这只表示等待动作超时，不表示任务被取消、panic 或抛异常。总量 admission 耗尽时记录 `FailureKind::CapacityExhausted` 并增加 `capacity_exhausted_count`（配置与覆盖范围见 §3.11）。
- **ExecutorResult**：`ok`、`error_code`、`message`，用于 `initialize`、`register_realtime_task`、`start_realtime_task`、`register_gpu_executor` 等。常见 `ExecutorErrorCode`：`AlreadyInitialized`、`AlreadyShutdown`、`InvalidConfig`、`DuplicateName`、`NotFound`、`BackendUnavailable`、`StartFailed`、`PermissionDenied`。失败会写入 failure/diagnostic event，但配置错误不会计入 `task_exception_count`。
- **CompletionStatus**：`executor_name`、`is_initialized`、`is_running`、`is_idle`、`active_tasks`、`queued_tasks`、`pending_tasks`、`completed_tasks`、`failed_tasks`。由 `get_completion_status()` 和 `WaitResult::status` 返回；状态查询不会触发默认异步执行器懒初始化。它仅描述默认异步执行器，不包含实时线程、实时队列或应用自建的多消费者流水线；跨视觉、控制等消费者的 idle 状态由应用定义并汇总。
- **WaitResult**：`completed`、`timed_out`、`timeout`、`status`、`message`、可选 `diagnostic_snapshot`。由 `wait_for_completion(timeout)` 返回；超时会记录 `FailureKind::WaitTimeout`，并保留同一次路径采集的完整生命周期快照。
- **ExecutorSnapshotTextMetrics**：`formatting_duration`（纳秒）和 `formatting_allocation_count`。仅用于 formatter 性能基线；分配计数不包含 snapshot provider、Executor 业务路径或外部日志系统。
- **ExecutorSnapshotTextExport**：`text` 与 `metrics`，由 `kairo::monitor::format_executor_snapshot_with_metrics()` 返回。
- **CycleStatistics**：`name`、`period_ns`、`cycle_count`、`timeout_count`、`avg_cycle_time_ns`、`max_cycle_time_ns`、`is_running`。由 `ICycleManager::get_statistics()` 返回。

### 7.4 通信 facade 通用类型

通信 facade 的阶段 7.0 入口在 `include/kairo/comm.hpp`：

```cpp
#include <kairo/comm.hpp>

kairo::comm::CommResult result =
    kairo::comm::CommResult::failure(
        kairo::comm::CommErrorCode::Timeout,
        "receive timed out");

if (!result) {
    const char* code =
        kairo::comm::comm_error_code_to_string(result.error_code);
}
```

当前已公开的通用类型：

- **CommErrorCode**：`Ok`、`Closed`、`Full`、`Empty`、`Timeout`、`Stale`、`MissedPhase`、`InvalidArgument`、`NotReady`、`Unknown`。
- **CommResult**：`ok`、`error_code`、`message`，支持 `operator bool()`、`success()`、`failure()`。
- **ChannelOptions**：`capacity`、`drop_policy`、`enable_stats`、`name`，用于配置 typed channel。
- **RealtimeChannelOptions**：`capacity`、`max_items_per_cycle`、`drop_policy`、`enable_stats`、`name`，用于配置实时周期内有限 drain 的消息通道。
- **TopicSubscriptionOptions**：每个订阅者独立的 `capacity`、`drop_policy`、`enable_stats` 和 `name`。
- **TopicPublishResult**：`matched_subscribers`、`delivered_subscribers`、`rejected_subscribers`；至少一个匹配订阅者拒绝时 bool 结果为 `false`。
- **DropPolicy**：`RejectNewest`（默认策略）、`DropOldest`、`KeepLatest`。
- **CommStats**：发送/接收/drop/覆盖/stale/关闭后发送/超时、handler 异常、missed phase、当前深度、峰值、容量、producer/consumer lag、最大/平均 latency，以及固定对数桶估算的 P50/P99 latency 等本地累计统计。
- **CommEventKind / CommEvent / CommEventCallback**：低频诊断事件类型、事件负载和回调签名。各组件通过 `set_event_callback(...)` 注册 callback；callback 抛出的异常会被隔离，不改变通信 API 的返回值或组件状态。

Typed Channel、`Topic` / `TopicSubscription`、`LatestMailbox`、`RealtimeChannel`、`PhaseGate`、`Sequencer`、`Snapshot` 和 `DoubleBuffer` 已开放。

通信事件默认只属于 `kairo::comm` 组件本地诊断，不计入 `ExecutorFailureStatus`，也不会触发 `Executor::set_failure_callback(...)`。阶段 7.6 暂不增加 Executor 级聚合入口；调用方如需统一上报，可在各组件 callback 中桥接到自己的监控系统。

`PhaseGate` 与 `DoubleBuffer<T>` / `LatestMailbox<T>` 支持显式的可选 LET 绑定模式，不新增平行的
`LetChannel<T>` 类型。调用 `buffer.bind_to_phase_gate(gate)` 或
`mailbox.bind_to_phase_gate(gate)` 后，写侧使用 `publish_for_current_phase()`，读侧使用
`load_for_current_phase()`：相位 N 的完整输出只在 gate 推进到 N+1 后可见。未绑定的
`DoubleBuffer::publish()` / `load()` 仍是最新完整快照，未绑定的 `LatestMailbox::publish()` /
`try_load()` 仍是 latest-wins。
绑定模式是第一版 SWSR，容量固定为两个槽位，成功周期路径不获取 mutex、等待 condition
variable 或分配内部存储；`T` 必须可无异常复制。失败的 `CommResult` 诊断不属于成功周期路径。
未就绪读取、重复发布、跳相位和相位关闭会返回 `CommResult`，推进与读写竞争返回 `NotReady`，
调用方应在下一个周期重试。

通信原语的当前同步边界如下：

- `MpscChannel<T>` / `RealtimeChannel<T>` 在构造期预分配有界 MPSC 节点，允许多生产者和一个
  逻辑消费者；构造后内部队列存储不再分配。
- `LatestMailbox<T>` / 未绑定的 `DoubleBuffer<T>` 使用四个固定 reader-pin 快照槽。writer 只有在
  槽未被 reader pin 住时才改写，因此复制非平凡 `T` 时不依赖存在 C++ data race 的 seqlock。
  `try_load()` 最多检查四个槽；`try_publish()` 是非等待、系统级 lock-free，但 publication CAS 可在
  其他 publisher 推进时重试，不能宣称单次调用有界或 wait-free。
- `PhaseGate` / `Sequencer` 的状态推进与查询使用原子核心；带 timeout 的 wait API 在该核心上
  spin/yield，仅供普通控制线程使用。
- 这些组件在构造时检查所需同步原子，平台不能提供 lock-free 原子时抛出异常拒绝构造。

`is_synchronization_lock_free()` 只回答组件内部同步原子是否 lock-free
返回相同结果。它不证明操作 wait-free，也不覆盖 `T` 的复制/移动/析构、时钟、字符串和
`CommResult` 构造、诊断 callback、调用方分配、page fault 或 OS 调度。因此必须分别陈述：
data-race-free、系统级同步无锁、固定次数读取尝试、内部存储无分配，以及整条路径满足硬实时预算。
`publish()` / `load()`、`send_for()` / `receive_for()` 和 wait API 为保证成功或等待 timeout 的
spin/yield 兼容适配器，不属于硬实时路径。callback 的配置会分配，callback 调用执行任意用户代码，
两者都属于非实时诊断/控制面。`Topic<T>` 的 registry、订阅快照及 publish fan-out 仍使用 mutex
和动态分配，整体都不是实时路径。

通信 P50/P99 是固定对数桶的近似分位数，组件 latency 不是端到端管线延迟；业务消息应携带源
时间戳，在目标端计算完整管线时间。

Linux 诊断构建可使用 `-DKAIRO_ENABLE_REALTIME_ALLOCATION_GUARD=ON`，再用
`RealtimeAllocationGuard(component, phase)` 包围待验证的周期。它记录当前线程中守卫范围内的
C++ `new` 分配次数和字节数，供测试定位；`RealtimeAllocationViolationPolicy::Abort` 可让测试在
首次分配时终止。`RealtimeThreadConfig::enable_allocation_guard` 默认关闭，显式开启后会在
`cycle_callback` 外自动使用记录型 guard，组件为执行器名、阶段为 `cycle_callback`。
该诊断通过进程级 `operator new` 重载实现，仅限 Linux 诊断构建，可能与宿主 allocator、内存池或
共享库重载冲突；生产实时回调仍应避免分配、阻塞和诊断 callback。

运行 `build/tests/benchmark_realtime_precision --json` 可生成 jitter 证据。JSON 同时记录编译器、
调度策略、采样 CPU 和测量边界：jitter 是周期回调入口相对期望截止时间的偏差，首个样本作为
基线，启动等待不计入。该指标与消息携带时间戳计算的端到端延迟应分别分析。

推荐从综合场景示例 [examples/comm_robot_pipeline.cpp](../examples/comm_robot_pipeline.cpp) 开始阅读：它把采集线程、规划线程、实时控制周期、状态监控、启动顺序、任务依赖和通信诊断串成一条完整流水线。

| 需求 | 推荐组件 |
|------|----------|
| 一名 consumer 按 FIFO 处理每条数据 | `MpscChannel<T>` / `SpscChannel<T>` |
| 多名独立 consumer 各自处理同一后续事件流 | `Topic<T>` / `TopicSubscription<T>` |
| 配置更新只关心最新值 | `LatestMailbox<T>` |
| 实时周期内处理有限条命令 | `RealtimeChannel<T>` |
| 多读者读取完整状态快照 | `DoubleBuffer<T>` / `Snapshot<T>` |
| 启动顺序、阶段推进 | `PhaseGate` |
| 单调 publication watermark；精确等待时检测被越过 ticket | `Sequencer` |
| 任务完成后触发后续任务 | `TaskHandle` + `submit_after()` / `when_all()` |

### 7.5 Typed Channel

`MpscChannel<T>` 提供类型安全的有界多生产者/单逻辑消费者通道，适合采集线程到规划线程、控制线程到通信线程等跨线程数据传递。节点存储在构造期按容量一次预分配；producer 先在私有节点中完成值构造，再以原子方式发布，consumer 从完整节点批次恢复 FIFO 顺序。内部同步不获取 mutex，也不会在构造后为队列节点分配内存，并支持非平凡类型和 move-only 类型。`T` 自身的复制、移动、析构和可能的分配不包含在该保证内。

```cpp
kairo::comm::ChannelOptions options;
options.capacity = 256;
options.drop_policy = kairo::comm::DropPolicy::RejectNewest;
options.name = "sensor_frames";

kairo::comm::MpscChannel<SensorFrame> frames(options);

frames.try_send(SensorFrame{});

SensorFrame frame;
if (frames.receive_for(frame, std::chrono::milliseconds(10))) {
    plan(frame);
}

frames.close();
auto stats = frames.stats();
```

主要 API：

- `try_send(const T&)` / `try_send(T&&)`：非阻塞发送；满队列或关闭后返回 `false`。
- `send_for(T, timeout)`：普通线程在非阻塞核心上 spin/yield 重试；超时返回 `CommErrorCode::Timeout`。
- `try_receive(T&)`：非阻塞接收；空队列返回 `false`。
- `receive_for(T&, timeout)`：普通线程在非阻塞核心上 spin/yield 等待；关闭且已 drain 完时返回 `CommErrorCode::Closed`。
- `close()` / `is_closed()`：以原子状态关闭生产者入口；等待适配器会观察关闭，已缓存数据仍可被 drain。
- `stats()`：返回本地累计 `CommStats`，可观察发送、接收、drop、关闭后发送、超时、当前深度和峰值。
- `is_synchronization_lock_free()`：仅报告内部同步原子是否 lock-free，不评价 payload 或外部运行环境。
- `SpscChannel<T>`：当前是 `MpscChannel<T>` 别名，后续可替换为 SPSC 优化实现。

默认 `RejectNewest` 满队列时拒绝新消息并增加 `dropped_count`；`DropOldest` 满队列时丢弃最旧消息再接收新消息；`KeepLatest` 保留最新值，适合后续 mailbox 风格场景。

`try_send()` / `try_receive()` 是实时调用方应使用的非等待入口。它们扫描有限容量存储，但竞争下的
原子 CAS 仍可能重试，因此同步 lock-free 不等于每次调用 wait-free。单逻辑消费者约束意味着应用
必须指定一个消费 owner；并发消费尝试不会形成第二条消费流。`send_for()` / `receive_for()` 会读取
时钟并持续重试，不是实时 API。已配置的事件 callback 还可能构造诊断字符串并执行用户代码，
实时路径应关闭 callback，把计数快照留给低频监控线程。

### 7.5.1 Topic / Subscription

`Topic<T>` 在同一进程内把一条事件扇出到发布快照中的每个活动订阅者。每个
`TopicSubscription<T>` 有独立的容量、drop policy、队列统计和 callback；慢订阅者满队列只改变
自己的投递结果，不阻塞或回滚其他订阅者。订阅只接收创建成功后的消息，不提供历史重放。

```cpp
kairo::comm::Topic<SensorFrame> frames("sensor_frames");
auto planner = frames.subscribe({.capacity = 256, .name = "planner"});
auto recorder = frames.subscribe({.capacity = 32,
                                  .drop_policy = kairo::comm::DropPolicy::DropOldest,
                                  .name = "recorder"});

const auto published = frames.publish(read_frame());
if (!published) {
    // 至少一个匹配订阅者拒绝了本次消息；检查各 subscription 的 stats。
}

SensorFrame frame;
if (planner.receive_for(frame, std::chrono::milliseconds(10))) {
    plan(frame);
}
```

主要 API 与语义：

- `subscribe(options)`：返回 move-only、RAII 的订阅句柄；Topic 已关闭时返回已关闭句柄。
- `publish(const T&)` / `publish(T&&)`：返回匹配、成功和拒绝订阅者数量；无订阅者发布是成功的空操作。
- `try_receive()` / `receive_for()`：只消费当前订阅的独立 FIFO；关闭后先 drain 已入队消息，再返回 `Closed`。
- `TopicSubscription::close()`：从 registry 注销并原子关闭内部通道；`receive_for()` 轮询后观察 `Closed`。重复关闭安全。析构执行同一 RAII
  注销/关闭，但句柄销毁必须与该句柄仍在执行的成员调用同步，不能依赖销毁 C++ 对象来并发唤醒其成员函数。
- `Topic::close()`：阻止后续订阅和发布，并关闭所有活动订阅者；其等待适配器随后观察 `Closed`。
- `stats()` / `set_event_callback()`：按订阅者观察 drop、overwrite、timeout、深度、lag 和 latency；callback 异常被隔离。

发布取得订阅 registry 快照是匹配集合的线性化点，退订从 registry 移除是退订线性化点。两者并发时，
已进入发布快照的订阅仍计入 `matched_subscribers`，随后可能成功入队，也可能因退订关闭而计入
`rejected_subscribers`；稳定的受管状态保证在途发布不会访问已析构对象。

第一版要求 `T` 可复制，因为多个订阅者需要独立拥有消息。大型不可变负载建议使用
`Topic<std::shared_ptr<const T>>`，库不会隐式共享 mutable object。该实现使用 mutex、动态 registry 和
动态订阅快照；`publish()` 的逐订阅者 fan-out 也会锁 registry、分配快照并复制/投递 payload。
因此 Topic 整体不是网络 broker、可靠广播、lock-free 或硬实时原语；跨进程、持久化、重放、确认、
重连和 QoS 协商应由 ROS 2、NATS、MQTT 等外部系统承担。

### 7.6 Realtime Mailbox / RealtimeChannel

`LatestMailbox<T>` 适合“配置线程发布、实时控制线程每周期只消费最新值”的场景。它只保留最近一次发布的值，并用单调递增的 sequence 帮助实时线程避免重复消费旧配置。

```cpp
kairo::comm::LatestMailbox<ControlConfig> config_box("control_config");

config_box.publish(load_config());

uint64_t seen = 0;
ControlConfig config;
if (config_box.try_load_newer_than(seen, config, seen)) {
    apply_config(config);
}
```

主要 API：

- `publish(const T&)` / `publish(T&&)`：发布最新值；覆盖已有值时增加 `overwritten_count`。
- `try_publish(value, &new_sequence)`：非等待、系统级 lock-free 发布；槽都被 reader pin/writer 占用或 sequence 已耗尽时返回 `false`。竞争 CAS 可重试，不承诺单次调用有界或 wait-free。
- `try_load(T&)`：读取当前最新值；从未发布时返回 `false`。
- `try_load_newer_than(last_seen, out, new_sequence)`：仅在 sequence 更新时返回 `true`，未更新时增加 `stale_read_count`。
- `sequence()` / `stats()` / `set_event_callback(...)`：观察当前版本、统计和低频诊断事件。

mailbox 使用四个固定 reader-pin 快照槽。reader 复制期间槽不会被改写，因而即使 `T` 不是
trivially copyable 也不会产生数据竞争。`try_load()` 最多检查四个槽；`try_publish()` 为系统级
lock-free、非等待入口，但竞争中的 CAS 可重试，不是 per-call bounded/wait-free。`publish()` 为兼容
既有“保证发布”语义会在槽暂时繁忙时 spin/yield，只适合非实时 producer。快照 sequence 当前为
56 位，达到 `2^56 - 1` 后 `try_publish()` 返回 `false`，兼容发布路径抛出 `std::overflow_error`，
不会把永久耗尽误作暂时竞争而无限重试。值的复制/移动、时钟和 callback 不在同步无锁保证内。

`RealtimeChannel<T>` 适合周期线程内 drain 一批消息但不能无限处理的场景。它与
`MpscChannel<T>` 使用相同的构造期预分配 MPSC 节点和单逻辑消费者同步核心。
`drain_for_cycle(handler, max_items)` 不等待 condition variable；`max_items == 0` 时使用
`RealtimeChannelOptions::max_items_per_cycle`。这与 `RealtimeThreadConfig::max_tasks_per_cycle`
的语义保持一致：`0` 表示不限，非 0 表示本周期预算上限；生产环境建议保留明确上限以维持周期确定性。

```cpp
kairo::comm::RealtimeChannelOptions options;
options.capacity = 128;
options.max_items_per_cycle = 8;
options.drop_policy = kairo::comm::DropPolicy::RejectNewest;

kairo::comm::RealtimeChannel<ControlCommand> commands(options);

commands.try_send(ControlCommand{});

commands.drain_for_cycle([&](ControlCommand& command) {
    apply_command(command);
});
```

主要 API：

- `try_send(const T&)` / `try_send(T&&)`：非阻塞发送；满队列或关闭后返回 `false`。
- `drain_for_cycle(handler, max_items = 0)`：实时周期入口，最多处理预算条消息并返回实际处理数量。
- `close()` / `is_closed()`：关闭生产者入口；已缓存消息仍可 drain。
- `stats()`：观察发送、接收、drop/overwrite、关闭后发送、当前深度和峰值。
- `is_synchronization_lock_free()`：检查内部同步原子；构造已在结果为 false 的平台拒绝该组件。

handler 抛异常时，`drain_for_cycle()` 停止本轮 drain，增加 `handler_exception_count`，触发 `HandlerException` 诊断事件，并将异常继续外抛；是否桥接到 `Executor` failure event 由调用方或后续集成层决定。

这里的“同步无锁”和“队列节点构造后无分配”不等于整个 handler 硬实时：payload 操作、
`steady_clock`、handler 本身、异常传播和已配置的 callback 都可能引入不确定耗时或分配。实时配置应
使用非抛出且有界的 `T` 与 handler、保留非零单周期预算，并把 callback/格式化放到普通监控线程。

### 7.7 PhaseGate / Sequencer

`PhaseGate` 适合表达“初始化完成后 worker 才继续”“采集阶段到达后规划阶段再开始”等阶段顺序。phase 与 closed 状态位于同一个原子状态字中，phase 单调递增，不允许倒退或重复 advance 到同一 phase；LET 访问和推进也由原子租约协调。

```cpp
kairo::comm::PhaseGate startup("startup");

std::thread worker([&] {
    auto ready = startup.wait_for(1, std::chrono::seconds(1));
    if (ready) {
        run_worker();
    }
});

startup.advance(); // phase: 0 -> 1
worker.join();
```

主要 API：

- `current_phase()`：读取当前 phase。
- `advance()` / `advance_to(phase)`：推进 phase；倒退或重复 `advance_to()` 返回 `CommErrorCode::MissedPhase`。
- `has_reached(phase)`：当前 phase 是否已经达到或超过目标。
- `wait_for(phase, timeout)`：等待达到或超过目标 phase；超时返回 `Timeout`，关闭返回 `Closed`。
- `wait_for_exact(phase, timeout)`：需要精确观察某个 phase 时使用；如果当前 phase 已超过目标，返回 `MissedPhase`。
- `close()` / `is_closed()`：原子关闭 gate；spin/yield waiter 随后观察 `Closed`。
- `stats()`：观察 advance、wait 成功、timeout、missed phase 和 waiter 数。
- `is_synchronization_lock_free()`：报告内部状态、统计和 callback 指针原子是否 lock-free。

`Sequencer` 维护单调 publication watermark，而不是要求每个 ticket 依次发布。`next_ticket()` 分配
递增 ticket；`publish(ticket)` 可跳过中间 ticket 并把 watermark 推进到更大值；
`is_published(ticket)` 表示 watermark 已达到或越过 ticket。`wait_until_published(ticket, timeout)`
是精确等待：watermark 恰好等于 ticket 时成功，已经越过时返回 `MissedPhase`。它不保存消息，
也不证明每个较小 ticket 都曾被显式 publish。

```cpp
kairo::comm::Sequencer sequencer("pipeline");

uint64_t step = sequencer.next_ticket();

std::thread waiter([&] {
    auto result = sequencer.wait_until_published(step, std::chrono::seconds(1));
    if (result) {
        consume_step(step);
    }
});

sequencer.publish(step);
waiter.join();
```

主要 API：

- `next_ticket()`：返回新的递增 ticket。
- `publish(ticket)`：发布 ticket；重复、倒退或无效 ticket 返回 `MissedPhase`。
- `is_published(ticket)`：publication watermark 是否已经达到或越过 ticket；不表示该 ticket 曾被单独发布。
- `wait_until_published(ticket, timeout)`：等待精确 ticket；超时、关闭和错过 ticket 均可通过 `CommResult` 区分。
- `close()` / `is_closed()` / `published_ticket()` / `stats()` / `set_event_callback(...)`：生命周期、观察和诊断入口。

`PhaseGate` 与 `Sequencer` 的 advance/publish/query 核心不获取 mutex；构造会拒绝所需原子不是
lock-free 的平台。`wait_for*()` 与 `wait_until_published()` 通过时钟检查和 `std::this_thread::yield()`
等待，只是控制面的 timeout 适配器，不是实时线程等待原语。原子 CAS 在竞争下可能重试，因此
“lock-free”也不等于每次调用 wait-free 或满足某个 deadline。

phase 与 ticket 状态的关闭位占用最高位，所以合法状态值必须小于 `2^63`。`PhaseGate::wait_for*()`
对 `phase >= 2^63`、`Sequencer::wait_until_published()` 对 `ticket == 0` 或 `ticket >= 2^63` 会在读取
时钟或进入 spin/yield 循环前立即返回 `InvalidArgument`。`next_ticket()` 在关闭或 ticket 空间耗尽时
返回 `0`。`PhaseGate` 的普通 phase 可达到 `2^63 - 1`，但 LET 槽把该值作为空态哨兵，因此绑定的
`DoubleBuffer` / `LatestMailbox` 只允许 phase `< 2^63 - 1`。

### 7.8 Snapshot / DoubleBuffer

`Snapshot<T>` / `DoubleBuffer<T>` 适合把共享 mutable state 改成“发布完整快照、读者按值读取”的模式。普通契约是 SWMR：读者不会拿到可变引用，也不会看到 writer 更新到一半的对象。未绑定模式使用四个固定 reader-pin 快照槽；显式 LET 绑定模式使用固定双槽和原子相位发布。

```cpp
struct SystemState {
    int tick = 0;
    int checksum = 0;
};

kairo::comm::DoubleBuffer<SystemState> states(SystemState{});

states.update([](SystemState& state) {
    state.tick += 1;
    state.checksum = state.tick * 17;
});

auto snapshot = states.load();
if (snapshot.value.checksum == snapshot.value.tick * 17) {
    monitor(snapshot.value);
}
```

主要 API：

- `Snapshot<T>`：包含 `value`、`sequence`、`timestamp`。
- `try_publish(T, &new_sequence)`：非等待、系统级 lock-free 发布；四个槽都暂时被 pin/占用或 sequence 已耗尽时返回 `false`。竞争 CAS 可重试，不承诺单次调用有界/wait-free。
- `publish(T)`：直接发布一个完整的新状态并返回 sequence；槽繁忙时 spin/yield 重试，sequence 耗尽时抛 `std::overflow_error` 的兼容 API。
- `update(fn)`：把当前完整快照复制为 writer 局部候选值，执行 writer 后一次性发布。
- `try_load(Snapshot<T>&)`：执行一次有界完整快照读取。
- `load()`：读取当前完整快照；竞争导致一次 pin 失败时 spin/yield 重试的兼容 API。
- `load_newer_than(last_seen, out)`：仅在 sequence 更新时返回 `true`，否则返回 `false` 并增加 `stale_read_count`。
- `sequence()` / `stats()` / `set_event_callback(...)`：观察版本、统计和低频诊断事件。
- `is_synchronization_lock_free()`：报告内部 reader pin、版本、统计和 callback 指针原子是否 lock-free。

公开写入模型是单写多读；多写场景建议先通过 `MpscChannel` 汇聚到一个状态 owner，再由 owner 调用
`publish()` 或 `update()`。reader pin 保证普通 `T` 的复制期间 writer 不会改写同一槽，所以完整快照
既 data-race-free，也不获取 mutex。`try_load()` 最多检查四个槽；`try_publish()` 是系统级 lock-free、
非等待操作，但 publication CAS 可重试，不能称为单次有界或 wait-free。`publish()`、`load()` 与
`update()` 为兼容语义可能 spin/yield；当有限的 56 位 sequence 永久耗尽并阻止其重试完成时，兼容
路径抛出 `std::overflow_error`。所有读取都会复制 `T`，其执行时间、异常和内部内存行为不由组件
保证；大型对象需要评估复制成本，也可显式让 `T` 为预先构造的不可变 handle。

需要固定逻辑相位时，可在构造/配置阶段显式绑定：

```cpp
kairo::comm::PhaseGate gate;
kairo::comm::DoubleBuffer<Command> commands(Command{});
commands.bind_to_phase_gate(gate);  // 固定为两个预分配槽位，SWSR。

commands.publish_for_current_phase(Command{/* phase 0 output */});
gate.advance();

kairo::comm::Snapshot<Command> visible;
if (commands.load_for_current_phase(visible)) {
    apply(visible.value);  // 在 phase 1 读取完整的 phase 0 输出。
}
```

`publish_for_current_phase()` 和 `load_for_current_phase()` 返回 `CommResult`。同相位读取不会看到
正在生成的值；遗漏上一相位时读取返回 `NotReady`。绑定模式的 `T` 必须满足无异常复制构造和赋值，
以保证周期路径没有隐式异常恢复。LET 可发布 phase 必须 `< 2^63 - 1`；更大的 phase 因槽状态哨兵
保留而返回 `InvalidArgument`。

`LatestMailbox<T>` 使用同名相位 API；`load_for_current_phase(out, &visible_phase)` 可返回可见的
逻辑相位。绑定 mailbox 是每相位最多一次发布的单值快照，不提供 FIFO，也不沿用未绑定模式的
latest-wins 覆盖；需要逐条消息时继续使用 `RealtimeChannel<T>`，它不自动继承 LET。

### 7.9 TaskPriority

```cpp
enum class TaskPriority { LOW = 0, NORMAL = 1, HIGH = 2, CRITICAL = 3 };
```

与 `submit_priority(int priority, ...)` 的整型对应。

---

## 8. GPU 执行器 API（可选，需 KAIRO_ENABLE_GPU）

GPU 执行器与 CPU 执行器接口分离，通过 `Executor` 注册与提交 GPU kernel，详见 [GPU 执行器设计](design/gpu_executor.md)。

### 8.1 注册与任务提交

```cpp
ExecutorResult register_gpu_executor(const std::string& name,
                                     const gpu::GpuExecutorConfig& config);

template<typename KernelFunc>
auto submit_gpu(const std::string& executor_name,
               KernelFunc&& kernel,
               const gpu::GpuTaskConfig& config)
    -> std::future<void>;
```

- `register_gpu_executor`：按 `config.backend` 创建并注册 GPU 执行器；当前支持 `GpuBackend::CUDA` 和 `GpuBackend::OPENCL`。对应后端还需在编译时启用 `KAIRO_ENABLE_CUDA` / `KAIRO_ENABLE_OPENCL`，并且运行时设备、驱动和平台可用；否则创建或启动失败。
- `register_gpu_executor` 返回可诊断结果，可区分 `InvalidConfig`、`DuplicateName`、`BackendUnavailable` 和 `StartFailed`。例如未编译对应后端、SYCL/HIP 尚未实现、运行时创建失败都会返回 `BackendUnavailable` 或更具体的启动失败信息。
- `submit_gpu`：向指定 GPU 执行器提交 kernel；kernel 可为 `void()` 或 `void(void*)`（流句柄，CUDA 下为 `cudaStream_t`，OpenCL 下为 `cl_command_queue`）。

### 8.2 查询与状态

```cpp
IGpuExecutor* get_gpu_executor(const std::string& name);
std::vector<std::string> get_gpu_executor_names() const;
gpu::GpuExecutorStatus get_gpu_executor_status(const std::string& name) const;
```

### 8.3 GPU 执行器接口（IGpuExecutor）

通过 `get_gpu_executor(name)` 获取指针后，可调用：该指针为非持有高级接口，不能跨或并发于 `shutdown()` 使用。直接使用 `ExecutorManager` 的集成应改取 `get_gpu_executor_snapshot(name)` 并在操作期间持有返回的 `std::shared_ptr`；快照不阻止执行器停止，仍要处理提交 future 和状态中的停止失败。

- **内存**：`allocate_device_memory`、`free_device_memory`；`copy_to_device`、`copy_to_host`、`copy_device_to_device`（均支持异步与流 ID）。`async=false` 在返回前完成指定流中的复制；`async=true` 仅入队，调用方必须在操作完成前保持相关设备缓冲区有效（主机参与的异步复制还必须保持主机缓冲区有效），随后通过 `synchronize_stream`、`synchronize` 或 `wait_for_completion` 等待完成。
- **统一内存**：`allocate_unified_memory`、`free_unified_memory`、`prefetch_memory`（host / device 方向均可）
- **P2P 传输**：`copy_from_peer`（跨 GPU 设备对等拷贝）
- **批量执行**：`submit_kernels_batch`（一次性提交一组 kernel+config，返回等长 `std::vector<std::future<void>>`；关停时每个输入均保证返回一个 future——CUDA 侧已入队的 kernel 继续排空，OpenCL 侧未被 worker 取出的以 `ExecutorStopping` 就绪，见上文 stop() 队列契约）
- **流**：`create_stream`、`destroy_stream`、`synchronize_stream`、`add_stream_callback`。`add_stream_callback` 当前仅支持 CUDA；调用前使用 `supports_stream_callback()` 查询，OpenCL 会返回 `false`，并在 `get_status().last_error_message` 中说明该能力尚未实现（计划通过 `cl_event` 轮询跟进）。
- **执行**：`submit_kernel(kernel, config)`（返回 `std::future<void>`）、`synchronize`、`wait_for_completion`
- **状态**：`get_name`、`get_device_info`、`get_status`、`start`、`stop`

CUDA 的 `stop()` 可被多个外部线程并发调用：其中一个调用方接管并等待 worker 线程，其余调用方安全返回。需要区分调用方时可直接使用 `CudaExecutor::stop_and_join()`；它在外部线程返回 `true`，在 CUDA worker 内调用时只请求停止并返回 `false`，外部线程随后必须调用一次 `stop_and_join()` 完成线程回收。自停止后的重新 `start()` 会在 worker 句柄被外部回收前被拒绝。

`stop()` 的队列契约因后端而异：CUDA 会继续排空已入队的 kernel；OpenCL 会取消 `stop()` 开始时尚未被 worker 取出的 kernel。每个被取消的 OpenCL `std::future<void>` 都会就绪，`get()` 抛出 `kairo::ExecutorStopping`，而不会抛出 `std::future_error(broken_promise)`。取消会计入 OpenCL `GpuExecutorStatus::failed_kernels`，并将 `last_error_message` 更新为取消原因；已经由 worker 开始执行的 OpenCL kernel 正常完成或报告其自身执行错误。

### 8.4 配置与类型

- **GpuExecutorConfig**：`name`、`backend`（支持 CUDA/OpenCL；分别要求 `KAIRO_ENABLE_CUDA` / `KAIRO_ENABLE_OPENCL` 且运行时可用）、`device_id`、`max_queue_size`、`memory_pool_size`、`default_stream_count`、`enable_monitoring`、`enable_unified_memory`（启用 `allocate_unified_memory` 等统一内存 API，CUDA 后端需要 `KAIRO_ENABLE_CUDA` 且硬件支持 managed memory）。`backend` 默认是 `GpuBackend::CUDA`；需要自动选择时可先调用 `gpu::get_recommended_backend()`，推荐逻辑会优先可用 CUDA 设备，其次 OpenCL，最后回到 CUDA 默认值。`device_id` 必须非负；`ExecutorManager::create_gpu_executor` 会拒绝负值，直接构造 `OpenCLExecutor` 时也会记录无效配置并在 `start()` 阶段拒绝负 `device_id`，不会用负下标访问设备数组。
- **GpuTaskConfig**：`grid_size`、`block_size`、`shared_memory_bytes`、`stream_id`、`async`；可选 `priority`。`stream_id == 0` 表示默认流/队列；非 0 值必须来自 `create_stream()` 且尚未 `destroy_stream()`，负数、越界或已销毁的 `stream_id` 不会回退到默认流/队列，相关 copy/submit 操作会失败。
- **CUDA stream 生命周期**：CUDA 后端内部用引用计数 wrapper 管理 `cudaStream_t`。`destroy_stream(stream_id)` 会先从 stream 表中摘除该 slot，并标记旧 wrapper 已销毁；已经拿到旧 wrapper 的并发操作会在 wrapper 锁下完成或观察到销毁状态，不会在已销毁的裸 `cudaStream_t` 上继续调用 CUDA API。销毁后的 copy/prefetch/callback/P2P 操作返回 `false`；销毁后的 `submit_kernel`/`submit_kernels_batch` future 抛出 `gpu::InvalidStreamException`。后续 `create_stream()` 可复用已摘除的 slot，但销毁前已提交的任务仍绑定旧 wrapper，不会误用新 stream。
- **GpuDeviceInfo**：设备名称、后端、设备 ID、厂商、总/空闲内存、计算能力等
- **GpuExecutorStatus**：名称、运行状态、活跃/完成/失败 kernel 数、队列大小、平均 kernel 时间、内存使用、`last_error_message`（最近一次启动/运行失败原因；空表示无错误；CUDA/OpenCL kernel 异常、无效 stream_id 和不支持的 OpenCL stream callback 均会记录）等
- **GpuScheduler**：GPU 任务调度器，支持优先级队列与批量提交策略；可通过 `GpuScheduler::Config` 配置
- **KernelLaunchOptimizer**：自动调优 kernel 启动参数（grid/block 尺寸），减少 kernel 配置开销；`KernelLaunchOptimizer::Config` 可定制
- **TaskSchedulerOptimizer**：优化 GPU 任务调度顺序，提高流水线利用率；`TaskSchedulerOptimizer::Config` 可定制
- **TransferOptimizer**：优化主机↔设备数据传输（合并小传输、异步流水线）；`TransferOptimizer::Config` 可定制

多 GPU 设备间 P2P 拷贝（`copy_from_peer`）为实验性功能。示例见 [examples/gpu_basic.cpp](../examples/gpu_basic.cpp)、[examples/gpu_multi_device.cpp](../examples/gpu_multi_device.cpp)、[examples/gpu_opencl.cpp](../examples/gpu_opencl.cpp)。

### 8.5 GPU 设备查询 API

在创建 GPU 执行器前，可查询系统可用设备及推荐后端：

```cpp
#include <kairo/gpu/device_query.hpp>

// 枚举所有 CUDA 设备
std::vector<gpu::GpuDeviceInfo> enumerate_cuda_devices();

// 枚举所有 OpenCL 设备
std::vector<gpu::GpuDeviceInfo> enumerate_opencl_devices();

// 枚举所有 GPU 设备（CUDA + OpenCL）
std::vector<gpu::GpuDeviceInfo> enumerate_all_devices();

// 获取推荐后端（NVIDIA GPU 优先 CUDA，AMD/Intel GPU 使用 OpenCL）
gpu::GpuBackend get_recommended_backend(int device_id = 0);
```

**使用示例**：

```cpp
// 查询所有设备
auto devices = kairo::gpu::enumerate_all_devices();
for (const auto& dev : devices) {
    std::cout << "Device " << dev.device_id << ": "
              << dev.name << " (" << dev.vendor << ")\n"
              << "  Backend: " << (dev.backend == kairo::gpu::GpuBackend::CUDA ? "CUDA" : "OpenCL") << "\n"
              << "  Memory: " << (dev.total_memory_bytes / 1024 / 1024) << " MB\n";
}

// 自动选择推荐后端
auto backend = kairo::gpu::get_recommended_backend(0);
kairo::gpu::GpuExecutorConfig config;
config.backend = backend;
config.device_id = 0;
exec.register_gpu_executor("gpu0", config);
```

命令行工具：`gpu_device_query` 示例程序可直接查询系统 GPU 设备。

---

## 9. ICycleManager 接口（可选周期管理器）

`ICycleManager` 是可选接口，用于为实时线程提供更精确的周期控制和监控。若不提供，kairo 使用内置的简单周期实现（基于 `std::this_thread::sleep_until`）。

### 9.1 接口定义

```cpp
class ICycleManager {
public:
    virtual ~ICycleManager() = default;

    // 注册周期任务
    virtual bool register_cycle(const std::string& name,
                                int64_t period_ns,
                                std::function<void()> callback) noexcept(false) = 0;

    // 启动周期任务
    virtual bool start_cycle(const std::string& name) noexcept(false) = 0;

    // 停止周期任务
    virtual void stop_cycle(const std::string& name) noexcept(false) = 0;

    // 获取周期统计信息（可选）
    virtual CycleStatistics get_statistics(const std::string& name) const = 0;
};
```

显式标注 `noexcept(false)`，表示周期管理器的实现可以抛出异常；
`RealtimeThreadExecutor` 会捕获异常、递增
`RealtimeExecutorStatus::cycle_manager_error_count`，然后回退或正常返回。
调用 `stop_cycle()` 时不会持有执行器的生命周期互斥锁，因此已注册的周期回调可安全地在该执行器上调用
`stop()` 或 `stop_and_join()`。周期管理器的实现仍须自行保证其回调和停止状态的线程安全。

多个外部线程可以并发调用 `RealtimeThreadExecutor::stop_and_join()`。取得工作线程所有权的调用方负责完成停止收尾；其他调用方会等待其完成工作线程 join、等待正在进行的任务提交结束并清空已排队任务。停止收尾完成前，`start()` 返回 `false`，从而避免新建的实时线程与正在停止的工作线程重叠运行。`push_task()` 的提交路径与 `LockFreeTaskExecutor` 一样由原子准入门闩保护：“检查停止是否已开始”与“登记为在途生产者”是同一个原子操作，因此 `stop_and_join()` 返回后不会再有任何生产者访问执行器成员，对象可以安全销毁，也不会有任务在最终清空之后进入队列。

### 9.2 使用场景

**内置周期（默认）**：
- 使用 `std::this_thread::sleep_until` 实现简单周期控制
- 适合大多数场景，无需额外实现
- 配置 `RealtimeThreadConfig::cycle_manager = nullptr`（默认）

**ICycleManager（可选）**：
- 需要更精确的周期控制（如硬件定时器、RTOS 周期管理）
- 需要统一的周期监控和统计（多个实时线程共享同一周期管理器）
- 需要自定义周期超时检测和恢复策略
- 需要与外部周期管理系统集成

### 9.3 实现示例

以下示例实现一个基于 `sleep_until` 的简单周期管理器：

```cpp
#include <kairo/executor.hpp>
#include <algorithm>
#include <chrono>
#include <functional>
#include <thread>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class SimpleCycleManager : public kairo::ICycleManager {
public:
    struct CycleInfo {
        std::string name;
        int64_t period_ns = 0;
        std::function<void()> callback;
    };

    bool register_cycle(const std::string& name, int64_t period_ns,
                       std::function<void()> callback) override {
        std::lock_guard<std::mutex> lock(mutex_);
        cycles_[name] = {name, period_ns, std::move(callback)};
        stop_requested_[name] = false;
        return true;
    }

    ~SimpleCycleManager() override {
        stop_all_cycles();
    }

    bool start_cycle(const std::string& name) override {
        CycleInfo info;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = cycles_.find(name);
            if (it == cycles_.end()) {
                return false;
            }
            const auto worker = std::find_if(cycle_threads_.begin(), cycle_threads_.end(),
                [&name](const auto& entry) { return entry.first == name; });
            if (worker != cycle_threads_.end()) {
                return false;
            }
            info = it->second;
            stop_requested_[name] = false;
            cycle_threads_.emplace_back(name, std::thread([this, name, info]() {
                auto next_cycle_time = std::chrono::steady_clock::now();
                const auto period_ns = std::chrono::nanoseconds(info.period_ns);

                while (true) {
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        if (stop_requested_[name]) {
                            break;
                        }
                    }

                    if (info.callback) {
                        info.callback();
                    }

                    next_cycle_time += period_ns;
                    std::this_thread::sleep_until(next_cycle_time);
                }
            }));
        }

        return true;
    }

    void stop_cycle(const std::string& name) override {
        std::thread cycle_thread;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_requested_[name] = true;
            const auto worker = std::find_if(cycle_threads_.begin(), cycle_threads_.end(),
                [&name](const auto& entry) { return entry.first == name; });
            if (worker != cycle_threads_.end()) {
                cycle_thread = std::move(worker->second);
                cycle_threads_.erase(worker);
            }
        }
        if (cycle_thread.joinable()) {
            cycle_thread.join();
        }
    }

    kairo::CycleStatistics get_statistics(const std::string& name) const override {
        kairo::CycleStatistics stats;
        stats.name = name;
        // 可在此添加统计信息收集逻辑
        return stats;
    }

private:
    void stop_all_cycles() {
        std::vector<std::thread> cycle_threads;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& stop_requested : stop_requested_) {
                stop_requested.second = true;
            }
            for (auto& worker : cycle_threads_) {
                cycle_threads.push_back(std::move(worker.second));
            }
            cycle_threads_.clear();
        }
        for (auto& cycle_thread : cycle_threads) {
            if (cycle_thread.joinable()) {
                cycle_thread.join();
            }
        }
    }

    std::unordered_map<std::string, CycleInfo> cycles_;
    std::unordered_map<std::string, bool> stop_requested_;
    std::vector<std::pair<std::string, std::thread>> cycle_threads_;
    mutable std::mutex mutex_;
};
```

`SimpleCycleManager` 负责管理每个工作线程。`stop_cycle()` 会向对应工作线程发出停止信号并执行 join；析构函数会在销毁回调或同步状态前，向所有尚未停止的工作线程发出停止信号并完成 join。

### 9.4 注入到实时线程配置

在 `RealtimeThreadConfig` 中设置 `cycle_manager` 指针：

```cpp
// 创建周期管理器实例（用户管理生命周期）
SimpleCycleManager cycle_manager;

// 配置实时任务
kairo::RealtimeThreadConfig rt_config;
rt_config.thread_name = "can_channel_0";
rt_config.cycle_period_ns = 2000000;  // 2ms
rt_config.thread_priority = 99;
rt_config.cpu_affinity = {0};
rt_config.cycle_callback = []() {
    // 周期回调逻辑
    // 注意：当使用 ICycleManager 时，此回调由周期管理器调用
};
rt_config.cycle_manager = &cycle_manager;  // 注入周期管理器

// 注册并启动实时任务
auto& exec = kairo::Executor::instance();
exec.register_realtime_task("can_channel_0", rt_config);
exec.start_realtime_task("can_channel_0");

// ... 使用实时任务 ...

// 停止实时任务（周期管理器会自动停止周期）
exec.stop_realtime_task("can_channel_0");

// 注意：cycle_manager 的生命周期需由用户管理，确保在使用期间有效
```

### 9.5 工作流程

1. **注册阶段**：`RealtimeThreadExecutor::start()` 调用 `cycle_manager->register_cycle(name, period_ns, callback)`，注册周期任务。
2. **启动阶段**：`RealtimeThreadExecutor::start()` 调用 `cycle_manager->start_cycle(name)`，周期管理器开始按周期调用回调。
3. **执行阶段**：周期管理器在每个周期调用 `callback`（即 `RealtimeThreadConfig::cycle_callback`），实时线程在此回调中执行周期逻辑。
4. **停止阶段**：`RealtimeThreadExecutor::stop()` 调用 `cycle_manager->stop_cycle(name)`，周期管理器停止周期循环。

### 9.6 注意事项

- **生命周期管理**：`cycle_manager` 指针的生命周期需由用户管理，确保在实时线程运行期间有效。
- **线程安全**：`ICycleManager` 的实现需保证线程安全（如使用互斥锁保护内部状态）。
- **统计信息**：`get_statistics()` 可用于监控周期执行时间、超时次数等，便于性能分析。
- **多实例共享**：一个 `ICycleManager` 实例可管理多个实时线程的周期，便于统一监控和管理。

### 9.7 完整示例

参见 [examples/realtime_can.cpp](../examples/realtime_can.cpp)，其中展示了 `SimpleCycleManager` 的完整实现和使用。

---

## 10. 底层接口（可选）

- **IAsyncExecutor**：异步执行器抽象（线程池实现），提供 `submit`、`submit_priority`、`get_status`、`start`、`stop`、`wait_for_completion`。
- **IRealtimeExecutor**：实时执行器抽象，提供 `start`、`stop`、`push_task`、`get_status`。
- **ExecutorManager**：管理默认异步执行器与实时执行器注册表；通常通过 `Executor` 间接使用，也可直接调用（见设计文档）。

---

## 11. 使用模式简述

- **单例**：`Executor::instance()` + `initialize`，同一进程内多模块共享线程池。
- **实例化**：`Executor ex; ex.initialize(config);`，独立实例，RAII 析构时释放执行器，适合多项目/多模块隔离。
- **实时场景**：`register_realtime_task` + `start_realtime_task`，在 `cycle_callback` 中做周期逻辑；与线程池之间通过无锁队列等交换数据（见示例 `realtime_can`）。
- **GPU 场景**：`register_gpu_executor` + `submit_gpu`，kernel 与内存/流由 `IGpuExecutor` 管理（见示例 `gpu_basic`、`gpu_multi_device`，设计 [gpu_executor.md](design/gpu_executor.md)）。

更多示例见 [examples/](../examples/) 与 [架构总纲](design/executor.md)。
