---
title: 执行模型与路由边界
description: 了解默认自动路由，以及任务完成、队列接收和 worker 生命周期之间的区别。
---

# 执行模型与路由边界

`Executor` 提供统一的 Facade，普通开发者从 `submit_auto(lambda)` 就能开始。不过，不同后端返回的结果并不一样。使用实时队列或长期 worker 之前，先确认调用方到底需要知道什么。

## 任务完成、队列接收和 worker 启动是三回事

| 模型 | 入口 | 调用方确认的事实 | 没有确认的事实 |
| --- | --- | --- | --- |
| completion | `submit_auto()`、`submit()`、CPU/GPU 双路径任务 | 工作完成，或 future 中有异常 | 实时周期、worker 或其他后端已结束 |
| admission | `dispatch_auto()` | 指定有界队列接受了本次任务 | 任务已执行、没有 drop 或业务已生效 |
| lifecycle | `start_worker()` | worker 已注册/启动，或得到启动失败 | 协议已握手、设备可用或首条数据已到达 |

这样就不会等错东西：`wait_for_completion()` 只等待默认异步任务的 future，不会等待实时周期、无锁队列或长期 worker。

## 默认自动路由做什么

默认普通任务：

```cpp
auto future = executor.submit_auto([] { return transform(); });
auto value = future.get();
```

`Auto` 只选择默认异步后端。它的价值是新手不必先注册或查找独立执行器，同时每次决定仍可查询：

```cpp
const auto decision = executor.get_last_routing_decision();
```

`RoutingDecision` 回答“根据声明的意图和能力快照选择了什么、为什么”；它不是提交 reservation，也不取代 `future`、`DispatchResult` 或状态计数。

## 路由不是猜测：先看入口，再看字段

“auto”并不表示运行时会分析 lambda、选择最快的机器或在所有注册执行器中任选一个。它只按照**入口类型**和你显式填写的 `TaskOptions` 字段路由：

| 调用形式 | 是否查找专用执行器 | 路由输入 | 目标如何确定 | 不会发生什么 |
| --- | --- | --- | --- | --- |
| `submit_auto(lambda)` | 否 | 无；等价 `intent = Auto` | 固定为 `default` 异步执行器 | 不因 GPU、deadline、priority 或负载改投其他后端 |
| `submit_auto(task(lambda).intent(GeneralCpu))` | 否 | `GeneralCpu`、可选名称/priority | 固定为 `default` 异步执行器 | `preferred_executor` 不能把普通 callable 路由到 GPU/实时/无锁 |
| `submit_auto(cpu_gpu_task(cpu, gpu))` | 是，仅查 GPU | CPU/GPU 双路径、GPU 名称、特征、fallback | 显式 GPU 名称；未填写时只能在**恰有一个**已注册 GPU 时确定候选 | 不把普通 lambda 当 GPU 任务；不在多个 GPU 间随意选择 |
| `dispatch_auto(options, task)` | 是，仅查有界后端 | `LowLatency`/`RealtimeQueue` + `preferred_executor` | 后端类别和名称必须同时精确匹配 | 不扫描“任一可用”队列；不回退默认线程池 |
| `start_worker(spec)` | 否 | worker spec 的名称、配置和实现 | 创建/启动这个 worker | 不是任务路由，也不返回一次任务的完成结果 |

因此，若你的目标是名为 `control` 的实时执行器，名称本身还不够：必须调用 `dispatch_auto()`，并把 intent 写成 `RealtimeQueue`。反过来，给普通 `submit_auto(lambda)` 增加名称或 priority 也不会让它改走 `control`。

## 按调用逐步匹配

### 1. 普通 `submit_auto`：没有目标选择

```cpp
auto future = executor.submit_auto([frame] { return decode(frame); });
```

它记录一个目标为 `default`、原因为 `DefaultPolicy` 的 `RoutingDecision`，随后按普通 future 路径提交。若你要给任务命名或使用默认线程池 priority，可使用 builder；可接受的 intent 只有 `Auto` 和 `GeneralCpu`：

```cpp
auto future = executor.submit_auto(
    kairo::task([frame] { return decode(frame); })
        .name("decode-frame")
        .priority(kairo::TaskPriority::HIGH)
        .intent(kairo::ExecutionIntent::GeneralCpu));
```

把 builder 的 intent 改成 `LowLatency`、`RealtimeQueue` 或 `BlockingWorker` 会让 future 直接成为异常：这些协议必须使用下面的 typed API，而不是让普通 callable 被猜测路由。

### 2. CPU/GPU `submit_auto`：GPU 名称 + 候选检查 + 策略

先提供两个独立实现，再声明 GPU 目标和可接受的回退：

```cpp
auto work = kairo::cpu_gpu_task(
    [input] { run_cpu(input); },
    [input](void* stream) { run_gpu(input, stream); })
    .name("segmentation")
    .preferred_executor("cuda0")
    .data_size(input.bytes())
    .compute_intensity(3.5F)
    .fallback(kairo::FallbackPolicy::AllowCpu);

auto future = executor.submit_auto(std::move(work));
```

匹配顺序如下：

1. `preferred_executor("cuda0")` 时，只查询名为 `cuda0` 的 GPU；名称不存在、未注册、未运行、缺少 GPU 提交能力或达到已知 capacity hint 时，不会换到别的 GPU。
2. 未填写名称时，只有注册表中**恰好一个** GPU 才能成为候选；零个或多个 GPU 都无法自动决定目标。
3. 对可提交 GPU，`prefer_gpu(true)` 优先选择 GPU；当前 Facade 不会自动记录性能样本，因此其余情况按数据量与计算强度阈值决定 CPU/GPU。`GpuScheduler` 的自适应历史仅适用于调用方自行维护并记录样本的调度器。
4. `RequireRequestedBackend` 跳过上述启发式，要求指定 GPU 可提交；没有名称即拒绝。
5. `AllowCpu` 在 GPU 不可用、未运行、已知容量不足或实际 GPU 提交被拒绝时改走默认 CPU，并在决策中写 `fell_back = true`。`NoFallback` 则让 future 以异常就绪。

`preferred_executor` 是“锁定候选名称”，不是“保证该任务一定跑在 GPU”。实际提交仍可能与 stop 或容量变化竞争；请同时观察 future 和 routing decision。

### 3. `dispatch_auto`：intent 和名称必须同时匹配

先注册并启动目标，随后填写完整的 `TaskOptions`：

```cpp
TaskOptions options;
options.name = "publish-telemetry";
options.intent = ExecutionIntent::LowLatency;
options.preferred_executor = "telemetry";

const auto result = executor.dispatch_auto(options, [] { publish(); });
```

路由器执行以下精确查找：

| intent | 要查找的后端类别 | 必填目标名称 | 投递前快照检查 |
| --- | --- | --- | --- |
| `LowLatency` | `LockFree` | 已启动的无锁执行器 | 已注册、运行中、且未达到已知 capacity hint |
| `RealtimeQueue` | `Realtime` | 已启动的实时执行器 | 已注册、运行中、且未达到已知 capacity hint |

名称缺失、类别不对、未注册、未启动或快照已满时，`accepted == false`，并在 `result.decision.reason` / `detail` 中说明原因。通过快照检查后还会尝试真实入队；若其间 stop、队列满或对象池耗尽，仍返回 `accepted == false`，原因为 `Rejected`。两种情况都不尝试其他同类执行器，也不回退 `default`。

### 4. `start_worker`：名称是生命周期标识，不是路由目标

```cpp
BlockingWorkerSpec spec{"serial-rx", config, std::move(worker)};
auto handle = executor.start_worker(std::move(spec));
```

`serial-rx` 用于注册、状态查询和 stop/wake/join 生命周期；它不参与 `submit_auto()` 或 `dispatch_auto()` 的候选匹配。`WorkerHandle::started()` 只说明启动结果，不能证明设备、协议或第一条数据已经就绪。

## 先发现，再投递

控制面可用 capability snapshot 展示当前可命名的后端：

```cpp
for (const auto& capability : executor.get_executor_capabilities()) {
    std::cout << capability.name << " running=" << capability.running
              << " pending=" << capability.pending_work << '\n';
}
```

用 `backend`、`name`、`registered`、`running`、`supports_future_submission`、`supports_bounded_dispatch`、`supports_gpu_kernel`、`pending_work` 和 `capacity_hint` 构建诊断或配置 UI。`pending_work` 对实时后端目前恒为 `0`，因为状态 API 不提供瞬时队列深度；实时积压应以 drop、队列满和周期超时计数判断。不要把这份快照当 reservation：显示可用后，实际投递仍必须处理拒绝。

## 如何读取一次路由

在提交或 dispatch 后读取 `RoutingDecision`：

```cpp
if (const auto decision = executor.get_last_routing_decision()) {
    log(decision->selected_executor_name, decision->detail);
}
```

- `selected_backend` / `selected_executor_name`：路由器尝试的后端与名称；
- `reason`：默认策略、显式 intent、首选名称、GPU heuristic/history、后端不可用/未运行/容量压力、回退或拒绝；
- `fell_back`：仅 `AllowCpu` 实际改走 CPU 时为真；它不是用户任务失败；
- `detail`：面向诊断的具体匹配原因。

路由决策解释“为什么尝试这条路径”，future、`DispatchResult` 和 `WorkerHandle` 分别说明完成、接收和生命周期的实际结果。

## 何时路由 GPU

GPU 不是普通 lambda 的隐式加速器。只有业务具有不同的 CPU 和 GPU 实现时，才构造双路径任务：

```cpp
auto future = executor.submit_auto(
    cpu_gpu_task([input] { run_cpu(input); },
                 [input](void* stream) { run_gpu(input, stream); })
        .preferred_executor("cuda0")
        .fallback(FallbackPolicy::AllowCpu));
```

GPU 运行条件和队列容量仍会在实际投递时变化。`AllowCpu` 会以 `RoutingDecision::fell_back` 解释 CPU 回退；`NoFallback` 或 `RequireRequestedBackend` 的拒绝则通过 future 异常和 failure event 观察。后端注册、设备和 stream 细节进入[GPU 专题](/zh/gpu/)。

## 何时路由有界队列

无锁和实时路径是 opt-in。只有调用方已经知道单消费者、有界背压或周期消费正是业务语义时，才能使用 `LowLatency` 或 `RealtimeQueue`，并且必须填写运行中后端的 `preferred_executor`。

```cpp
TaskOptions options;
options.intent = ExecutionIntent::LowLatency;
options.preferred_executor = "telemetry";
auto admission = executor.dispatch_auto(options, [] { publish(); });
```

`accepted == true` 只表示队列接收；即使接收后，任务也可能在执行时抛异常，或在实时周期中晚于你期望的时间被处理。未启动、队列满、对象池耗尽和关闭竞争都应通过 `DispatchResult`、failure event 和后端状态计数处理，绝不静默投递到默认线程池。

## 何时创建长期 worker

Blocking I/O 的工作单位不是一次 callable，而是可被唤醒和停止的长期循环：

```cpp
BlockingWorkerSpec spec{"serial-rx", config, std::move(worker)};
auto handle = executor.start_worker(std::move(spec));
if (!handle.started()) {
    report(handle.start_result().message);
}
```

`WorkerHandle::request_stop()` 只请求 stop 并唤醒，`WorkerHandle::stop()` 再 join。worker 自己必须实现 `run(stop_token)` 和不抛异常的 `wakeup()`；详细约束见[Blocking I/O worker](/zh/realtime-and-communication/blocking-io-workers)。

## 能力快照与实际投递

`get_executor_capabilities()` 可用于控制面、监控 UI 或投递前诊断：它列出后端类型、名称、运行状态、协议支持、pending work 和 capacity hint。它不是锁定后端的预约；快照与实际投递之间仍可能发生 stop 或容量变化。

把它用于解释和预检，而不是把“快照显示可用”当作“任务必然完成”的承诺。

## 下一步阅读

现在可以回到[如何选择提交接口](/zh/guides/choosing-submit-api)做场景选型；需要专家控制时，再进入[实时控制](/zh/realtime-and-communication/realtime-control)、[GPU 与降级](/zh/gpu/)、[Blocking I/O worker](/zh/realtime-and-communication/blocking-io-workers)或[高级与原理](/zh/advanced/)。
