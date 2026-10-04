# 阻塞 I/O 执行器扩展设计


> [!NOTE]
> 历史设计快照：Blocking I/O worker 设计。0.5.2 起推荐 `start_worker(BlockingWorkerSpec)` 一站式入口。

## 概述

本文定义 `BlockingIoExecutor` 的核心库边界：它管理一个**专属、允许有限阻塞、可唤醒退出**的 worker 的注册、生命周期和状态。具体协议、设备、文件描述符或业务数据面由使用方实现，不属于 `executor` 项目。

它补齐当前项目的第三种执行模型：

| 模型 | 当前类型 | 适合的工作 | 不适合的工作 |
| --- | --- | --- | --- |
| 通用异步 | `ThreadPoolExecutor` | 有限的离散任务、并行计算、后台工作 | 永久占用 worker 的收包循环 |
| 周期实时 | `RealtimeThreadExecutor` | 固定周期、短且有预算的控制和发布回调 | 阻塞 I/O、未知耗时的协议解码 |
| 阻塞 I/O | `BlockingIoExecutor`，本设计新增 | 专属接收、事件等待、有限 timeout、可唤醒退出 | 控制决策、长计算、通用任务调度 |

`std::jthread` 是 `BlockingIoExecutor` 的建议内部实现，不是公开的并发模型。
应用不应自行维护未注册的 `jthread`，否则 Executor 无法统一启动、停止、统计和诊断。

## 背景和问题

`RealtimeThreadExecutor` 的实际契约是：每个周期依次执行 `cycle_callback`、有限消费
内部队列、记录周期耗时，再以绝对时间睡眠。周期回调中的任何阻塞都会直接计入
`cycle_timeout_count`，并触发 skip-late 调相。因此把 `lcm.handleTimeout()` 一类调用
放入该回调，会把网络或驱动抖动传递到控制周期。

普通线程池也不是合适替代。将永不返回的 `while` 循环提交到线程池会永久占用一个
worker，使 `wait_for_completion()`、负载均衡、任务超时和队列指标不再表达其原本语义。
即使专门创建单 worker 线程池，也无法表达 transport 的唤醒、收包活性和协议错误。

当前 `ExecutorManager` 管理默认异步、实时和 GPU 执行器；`executor::comm` 已提供
有界 channel、`LatestMailbox`、`RealtimeChannel`、`DoubleBuffer` 和独立 `CommStats`。
新模型必须复用这些边界，不能把通信错误混入普通 task exception，也不能破坏实时路径。

## 目标

1. 将专属、可阻塞 I/O worker 纳入 `ExecutorManager` 的命名、所有权、RAII 和 shutdown。
2. 让 `stop()` 有明确且可验证的退出机制：请求停止后必须解除底层阻塞并 join。
3. 保证 worker 不占用 `SCHED_FIFO` 控制线程；默认使用普通 OS 调度。
4. 提供 worker 生命周期的运行状态可观测性；协议与数据面统计由调用方负责。
5. 保持现有 async、RT、GPU API 的兼容性；新增独立接口，不把 I/O 伪装成实时任务或线程池任务。
6. 只提供协议无关的 worker 生命周期契约；不在核心库实现或维护任何 transport adapter。

## 非目标

- 不实现完整的 reactor/actor/Rx 框架、多路 fd 聚合器或任何协议/设备 adapter。
- 不保证端到端硬实时性，也不因“尽快收包”默认提升到 `SCHED_FIFO`。
- 不把所有通信对象自动注册到 Executor 的 failure 面；通信背压、陈旧数据和协议错误有独立语义。
- 不承诺仅靠 `std::stop_token` 可以取消第三方库的阻塞调用。
- 不改变通信原语的同步契约。`RealtimeChannel<T>` 使用预分配 MPSC 核心，`LatestMailbox<T>` 使用
  四槽 reader pin 快照，但内部同步 lock-free 不覆盖 payload、callback、时钟、缺页或 OS 调度；
  硬实时消费者仍须验证完整路径。动态 `Topic<T>` 连同 publish fan-out 明确保留 mutex 与动态分配，
  只能用于非实时事件分发。

## 项目边界

`executor` 是库项目，不包含应用项目。因此本设计不定义 LCM、socket、串口、CAN 或硬件接入任务，也不定义消息格式、数据新鲜度、重连、设备安全动作、部署参数或硬件性能指标。这些由使用该库的独立应用根据其协议和安全要求实现及验证。

核心库只保证 `IBlockingIoWorker` 的所有权、启动、停止请求、`wakeup()`、join 和状态快照语义；worker 的 `run()` 与 `wakeup()` 如何实现由调用方负责。

## 总体架构

```mermaid
flowchart LR
    C[Library consumer] --> W[IBlockingIoWorker]
    W --> I[BlockingIoExecutor\nordinary scheduling]
    I -. lifecycle and status .-> E[Executor / ExecutorManager]
    E --> I
```

核心库只拥有并驱动 worker；它不解释 worker 的输入、输出或协议状态。调用方可以将 worker 用于任意可中断阻塞工作，但必须自行保证其数据面、错误处理和业务控制的线程安全。

## API 与类型设计

### 配置与状态

建议新增到 `include/executor/config.hpp`：

```cpp
struct BlockingIoConfig {
    std::string thread_name;
    std::vector<int> cpu_affinity;       // 空表示由 OS 调度，不自动绑核。
    bool enable_memory_lock = false;     // 默认关闭；仅测量后显式启用。
    std::chrono::milliseconds startup_timeout{1000}; // 0 = 不等待 ready，正值为启动上限。
};
```

建议新增到 `include/executor/types.hpp`：

```cpp
enum class BlockingIoStopReason {
    None,
    Requested,
    WorkerReturned,
    WorkerException,
    StartFailed
};

struct BlockingIoExecutorStatus {
    std::string name;
    bool is_running = false;
    bool stop_requested = false;
    bool ready = false;
    bool cpu_affinity_applied = false;
    bool memory_locked = false;
    uint64_t wakeup_count = 0;
    BlockingIoStopReason stop_reason = BlockingIoStopReason::None;
    std::string last_error_message;
};
```

这些计数仅描述 worker 生命周期。队列深度、协议错误、数据新鲜度和消费者延迟由调用方的数据面自行定义与观测。

### Worker 契约

`include/executor/blocking_io.hpp` 定义 worker 与执行器之间的窄接口：

```cpp
class IBlockingIoWorker {
public:
    virtual ~IBlockingIoWorker() = default;

    // 在 executor 创建的专属线程调用。必须在有限时间内响应 stop_token。
    virtual void run(std::stop_token stop_token) = 0;

    // 必须解除 run() 当前的阻塞等待；可重复调用，且不得抛出。
    virtual void wakeup() noexcept = 0;

};

class IBlockingIoExecutor {
public:
    virtual ~IBlockingIoExecutor() = default;
    virtual bool start() = 0;
    virtual void request_stop() noexcept = 0;
    virtual void stop() = 0;  // request_stop + wakeup + join
    virtual std::string get_name() const = 0;
    virtual BlockingIoExecutorStatus get_status() const = 0;
};
```

`run()` 返回不等于正常关闭：执行器应把未请求停止时的返回记录为 `WorkerReturned`，并将未捕获异常记录为 `WorkerException`。`wakeup()` 是强制契约；仅检查 `stop_token` 不足以中断第三方库的无限等待。

该执行器不提供 `push_task()`：I/O worker 是一个长期服务，不是任务队列。worker 的配置、输入和输出接口由使用方定义。

### Facade 与管理器

在 `ExecutorManager` 中增加独立注册表和锁：

```cpp
bool register_blocking_io_executor(
    const std::string& name,
    std::unique_ptr<IBlockingIoExecutor> executor);
IBlockingIoExecutor* get_blocking_io_executor(const std::string& name);
std::vector<std::string> get_blocking_io_executor_names() const;
```

在 `Executor` facade 提供和 RT 对称的生命周期接口：

```cpp
ExecutorResult register_blocking_io_worker_ex(
    const std::string& name,
    const BlockingIoConfig& config,
    std::unique_ptr<IBlockingIoWorker> worker);
bool register_blocking_io_worker(...);  // 兼容风格的 bool 包装

ExecutorResult start_blocking_io_worker_ex(const std::string& name);
bool start_blocking_io_worker(const std::string& name);
void stop_blocking_io_worker(const std::string& name);
BlockingIoExecutorStatus get_blocking_io_worker_status(
    const std::string& name) const;
```

名称必须在同一个 `Executor` 实例内跨 RT、GPU、I/O 统一唯一，防止监控、日志和 shutdown
依赖歧义。若现有各注册表无法立即改为共享 name registry，第一版至少应在 facade 注册时做
跨表冲突检查，并在后续版本收敛到统一 registry。

注册/启动失败继续复用 `ExecutorResult` 和现有 `ExecutorErrorCode`：非法 worker/config
使用 `InvalidConfig`，重名使用 `DuplicateName`，不存在使用 `NotFound`，线程或 transport
初始化失败使用 `StartFailed`。不新增“看似成功但后台稍后失败”的启动语义：worker 应在
`startup_timeout` 内报告 ready；超时则返回失败并完成清理。

## 线程与停止语义

### 内部实现

`BlockingIoExecutor` 持有 `std::jthread`、状态原子变量和 worker 所有权。启动时在线程内设置
线程名、可选 affinity 和可选 memory lock，再调用 `worker->run(stop_token)`。第一版不暴露
nice/priority 调整：Linux 的 nice 可能影响整个进程，必须先有目标设备测量和更精确的平台语义。
线程创建失败必须回滚 `is_running`，与 `RealtimeThreadExecutor::start()` 的失败回滚一致。

`request_stop()` 的固定顺序：

```text
stop_requested = true
-> jthread.request_stop()
-> worker.wakeup()
```

`stop()` 调用 `request_stop()` 后 join。析构函数也必须走同一路径。不能在未成功 join 时
detach：worker 仍可访问已析构的 transport、mailbox 或 Executor，detach 会把明确的停止问题
变成 use-after-free 风险。

停止时间上限来自 worker 的 interruptibility，而不是 executor 的 join timeout。若底层等待无法由 `wakeup()` 直接中断，worker 必须使用有限 timeout，并在每次返回后检查 stop token。具体等待原语和超时上界由使用方负责。
