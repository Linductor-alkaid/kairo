# 外部事件循环互操作指南（asio strand / io_context）

本指南回答台账 P1-1 的第一步诉求：当应用已经拥有自己的事件循环（典型是 asio 的
`io_context` + `strand`）时，如何与 executor 正确协作、哪些派发是 executor 看不见的、
在"看不见"的边界内应当遵守什么纪律。

- executor 核心库**不依赖 asio** 或任何第三方事件循环；本文只描述互操作模式。
- 本文不承诺任何未实现的 API。executor 自带的 `SerialExecutionContext` /
  `submit_on` 已在 S2 提供，用于不依赖第三方事件循环的 FIFO 派发纳管；直接调用
  `asio::post(strand, ...)` 仍处于 executor 的可见性盲区。
- 本文所有建议模式都可编译复现：`examples/event_loop_interop.cpp` 是最小可运行
  伴随示例（用互斥量 + 条件变量实现的串行循环等价复现 strand 语义，注册于 CTest）。

## 1. 现行合规模式：事件循环托管为 Blocking I/O worker

executor 提供的托管入口是 `Executor::start_worker()`（`IBlockingIoWorker` 契约）。
把外部事件循环挂进去的正确形态（等价于 heyaki 的 `AsioWorker` 路线）：

```cpp
class AsioLoopWorker final : public executor::IBlockingIoWorker {
public:
    explicit AsioLoopWorker(boost::asio::io_context& io) : io_(io) {}

    // 托管线程上运行事件循环；stop_token 置位后退出并排空。
    void run(executor::StopToken stop_token) override {
        // 用一个空任务观察 stop 请求（io_context 自身没有 stop token 概念）。
        boost::asio::post(io_, [this, stop_token] {
            std::thread([this, stop_token] {
                while (!stop_token.stop_requested()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                io_.stop();
            }).detach();
        });
        io_.run();
    }

    // executor 停止路径调用：让 run() 醒来检查 stop_token。
    void wakeup() noexcept override {
        boost::asio::post(io_, [] {});  // 唤醒阻塞的 run()
    }

private:
    boost::asio::io_context& io_;
};

executor::WorkerHandle worker = executor.start_worker(
    {"asio_loop", worker_config, std::make_unique<AsioLoopWorker>(io)});
```

该模式下收益的是**生命周期与诊断**：worker 线程命名、启停顺序、`BlockingIoExecutorStatus`
（is_running / stop_requested / wakeup_count / stop_reason）都进入 executor 的状态体系，
`Executor::get_snapshot()` 能看到该后端。asio 的定时器与回调仍由 asio 自己调度——
这不是缺陷，是边界（见 §3）。

收尾语义：`worker.request_stop()` 置位 stop_token 后由 `worker.stop()` join 托管线程；
需要"整批工作完成后再关闭"时，用 `executor::comm::PhaseGate` 做批次收尾（§4）。

## 2. strand 延续派发：合法但不可见

最常见的交互是线程池任务把延续派发回串行上下文：

```cpp
auto state = std::make_shared<PipelineState>(...);

auto task = executor.submit([state, &strand]() {
    state->prepare();                    // pool 线程上生产
    boost::asio::post(strand, [state] {  // 延续回到串行上下文消费
        state->consume();
    });
});
```

这是合法且经常必要的模式：strand 保证这些回调串行执行，对象状态在移交之后只在
strand 上访问。

**必须认清的盲区**：`asio::post(strand, ...` 之后的派发

- 不经过 executor 的任何提交路径：没有 admission 判断、没有排队计数；
- 不进入 `TaskStatistics` / in-flight 诊断：快照里看不到这些在途回调；
- 不进入 facade 失败事件体系：回调里抛出的异常由 asio 吞掉或终止 io_context，
  executor 的 `ExecutorFailureStatus` 对此一无所知。

这不是使用错误，而是**当前边界**：executor 的可见性止于它自己接受的提交。

## 3. 盲区内的纪律

对于直接调用 `asio::post(strand, ...)` 的盲区派发，按以下纪律使用：

1. **状态所有权显式移交**：跨界状态用 `shared_ptr` 拥有；post 出去的延续捕获该
   `shared_ptr`，原线程在 post 之后不再读写该状态。
2. **延续自捕获异常**：盲区里没有人替你记录失败。延续内部 `try/catch` 并把错误
   写入应用自己的错误通道（或 `LatestMailbox` 的错误槽）。
3. **不用盲区派发承载关键路径**：需要 admission、背压或失败计量的工作必须留在
   executor 提交路径上（`submit*` / `dispatch_auto`），只把轻量延续放到 strand。
4. **收尾用 PhaseGate，不用 sleep 轮询**：strand 完成批次后 `advance_to(phase)`，
   等待方 `wait_for(phase, timeout)`（无锁、可超时、可关闭）。
5. **strand 上的阻塞调用自己负责可中断**：需要打断的等待用 asio 定时器/取消，
   或改写为 executor 协作取消（`submit_cancellable` + StopToken，见 `docs/API.md`）。
   executor 的取消不会跨进 asio 的内部等待。

## 4. 收尾同步：PhaseGate 模式

批次收尾的推荐形态（完整可运行版本见 `examples/event_loop_interop.cpp`）：

```cpp
executor::comm::PhaseGate gate("batch");

// 串行侧：每完成一步推进相位。
for (uint64_t i = 0; i < kBatchSize; ++i) {
    strand_post([i, &gate] { /* step */ gate.advance_to(i + 1); });
}

// pool 侧：等待整批完成（有超时，不会永久等待）。
auto done = executor.submit([&gate] {
    return gate.wait_for(kBatchSize, std::chrono::seconds(5)).ok;
});
```

## 5. 定时器与取消：哪些可以迁移、哪些不可以

C1/T1 已提供的能力（`submit_cancellable*`、`TimerHandle`、`ScopedTimerHandle`）
可以迁移的是**不依赖 strand 所有权**的自建定时工作：

- 应用侧用 `sleep_until` + 标志位手写的延迟/周期循环 → `submit_delayed`
  / `submit_periodic`；
- 私有 deadline 轮询取消 → `submit_cancellable` + `StopToken` 协作取消；
  迁移边界见 `docs/MIGRATION.md`。

**不得迁移**的是必须在同一 strand 上执行与销毁的 asio timer（如 node/relay 中
`asio::steady_timer` 绑定 strand 访问对象的场景）：executor 的 facade 定时器把
到期工作派发到默认异步线程池，不保证与任何外部 strand 同上下文执行或销毁。
该类 timer 在 T2 通过验收前继续由应用侧管理；`SerialExecutionContext` 只负责 FIFO
任务派发，不改变 facade timer 的到期线程或外部对象销毁上下文。

## 6. 后续：T2 门控

S2 已交付 `SerialExecutionContext` / `submit_on(context, task)`，将自建 FIFO
派发纳入 admission、统计、失败事件和句柄取消。它不适配 asio strand，也不改变
外部事件循环的线程或销毁语义。T2 仍是后续门控：只有在外部 context adapter 的
到期、取消、重排和销毁位置得到验证后，才会更新 asio timer 的迁移指引。

## 附：示例索引

| 模式 | 示例位置 |
| --- | --- |
| 事件循环托管为 Blocking I/O worker | `examples/event_loop_interop.cpp` 模式 1 |
| pool -> strand 延续（盲区纪律） | `examples/event_loop_interop.cpp` 模式 2 |
| PhaseGate 批次收尾 | `examples/event_loop_interop.cpp` 模式 3 |
| Blocking I/O worker 基础 | `examples/tutorial/12_blocking_io_worker.cpp` |
| 协作取消与定时句柄 | `examples/tutorial/13_cancellation_and_timers.cpp` |
