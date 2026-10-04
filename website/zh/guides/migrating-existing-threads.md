---
title: 从现有线程代码迁移
description: 以数据解析服务为例，把 std::thread、std::async 和手写任务队列逐步迁移到 Kairo，同时保留正确的所有权与关闭语义。
---

# 从现有线程代码迁移

迁移的目标不是消灭所有 `std::thread`，而是把普通有限工作交给 `submit_auto(lambda)`，并让结果、失败、过载和关闭变得可观察。永久循环、设备阻塞读取和有严格周期要求的工作需要进入不同的显式路径。

长期阻塞循环若需要 Facade 管理和 join 语义，可实现 `IBlockingIoWorker` 并阅读[阻塞 I/O worker](/zh/realtime-and-communication/blocking-io-workers)。该库路径只管理生命周期；协议和设备行为仍由调用方负责。

本页用一个数据解析服务贯穿迁移过程。服务接收 `Frame`，并行解析后交给调用方；退出时不再接收新帧，等待已接收工作，并报告未完成数量。

## 迁移前先画清责任

现有代码常把四种责任揉在一个线程函数里：

```text
接收输入 → 创建执行资源 → 执行业务计算 → 决定如何停止
```

迁移前先分别回答：

| 责任 | 必须回答的问题 |
| --- | --- |
| 输入所有权 | 任务执行时，参数和捕获对象仍然有效吗？ |
| 执行资源 | 工作是短任务、永久循环、软周期还是严格周期？ |
| 完成结果 | 谁持有 future，何时取值，异常在哪里处理？ |
| 过载策略 | 输入超过消费能力时，是排队、拒绝、覆盖还是降级？ |
| 生命周期 | 谁停止生产者，谁排空任务，谁最后关闭 Kairo？ |

如果这些答案不清楚，直接替换 API 只会把旧竞态搬到新抽象里。

## 起点：每个请求创建一个线程

下面的写法在低频原型中很直观，但并发量由输入直接决定，线程创建失败和任务异常也很难回到请求边界：

```cpp
void ParserService::accept(Frame frame) {
    std::thread([this, frame = std::move(frame)]() mutable {
        auto parsed = parse(frame);
        publish(parsed);
    }).detach();
}
```

这里至少有四个风险：

- detached 线程捕获 `this`，服务析构后仍可能访问悬空对象；
- 输入高峰会同时创建大量系统线程，没有统一容量边界；
- `parse()` 或 `publish()` 抛异常时，没有调用方结果通道；
- 退出时无法知道哪些帧仍在处理。

不要从“把 `std::thread` 改成 `submit()`”开始。第一步应先禁止新输入，并决定已接收工作的 owner；普通短任务的默认迁移入口是 `submit_auto(lambda)`。

## 第一步：让服务借用 Kairo

应用层拥有 Kairo，业务服务只借用引用。这样服务析构不会意外关闭全局执行资源，应用也能统一安排关闭顺序：

```cpp
class ParserService {
public:
    explicit ParserService(kairo::Kairo& executor)
        : executor_(executor) {}

private:
    kairo::Kairo& executor_;
};
```

使用单例还是独立实例取决于资源边界：进程内普通业务共享线程池时使用 `Executor::instance()`；测试、插件或子系统需要独立排空和关闭时，应用可持有独立 `Executor`。无论哪种方式，都应只有一个明确 owner 调用 `initialize()` 和 `shutdown()`。

## 第二步：先迁移需要结果的工作

让 `accept()` 返回 future，把解析结果和异常交还给调用方：

```cpp
std::future<ParsedFrame> ParserService::accept(Frame frame) {
    return executor_.submit_auto(
        [frame = std::move(frame)]() mutable {
            return parse(frame);
        });
}
```

调用方决定请求预算和异常处理：

```cpp
auto parsed = parser.accept(std::move(frame));

try {
    publish(parsed.get());
} catch (const std::exception& error) {
    report_parse_failure(error);
}
```

这个版本刻意不捕获 `this`。任务按值拥有输入，业务对象可以在 future 完成后再决定如何发布。若任务必须访问服务状态，优先把所需状态复制成不可变输入；确实需要共享对象时，使用能证明生命周期的所有权模型，而不是默认捕获引用。

### 不要立刻把 future 藏起来

迁移初期保留 future 能暴露旧代码中原本静默的异常和完成假设。只有明确属于 fire-and-forget，且已经接入 failure callback/status 与业务关联 ID 的工作，才应省略逐项结果。

## 第三步：从 `std::async` 迁移

`std::async` 适合局部并行，但默认 launch policy 可以由实现选择；大量调用也不提供应用级共享容量、统一监控或排空状态。迁移通常保持返回接口不变：

```cpp
// 迁移前
return std::async(std::launch::async, parse, std::move(frame));

// 迁移后
return executor_.submit_auto([frame = std::move(frame)]() mutable {
    return parse(std::move(frame));
});
```

需要注意的不是语法，而是生命周期变化：Kairo 的线程池由应用统一持有，future 析构不能替你定义服务关闭。请求仍应消费 future，进程退出仍应先停止生产者再排空执行器。

如果现有 `std::async` 依赖“不指定 policy 时可能 deferred”的行为，不能直接等价迁移；先明确它究竟需要异步执行还是调用方线程中的惰性计算。

## 第四步：替换手写线程池或任务队列

现有系统若已有 `queue + mutex + condition_variable + workers`，不要一次删除全部实现。按以下顺序迁移更容易验证：

1. 冻结旧队列入口，记录现有容量、拒绝和关闭语义。
2. 先把一类独立短任务切到 `submit_auto(lambda)`，保留相同业务结果。
3. 用 `get_completion_status()` 对比旧系统的 active/queued 指标。
4. 注入异常、队列积压和退出竞争，确认新路径有明确返回与计数。
5. 所有生产者迁完后，再删除旧 workers 和 condition variable。

不要用一个巨大 Kairo 队列模仿旧系统所有通道。不同输入若有不同“过时”语义，应在业务入口选择：必须处理每条消息使用有界 channel，只关心最新值使用 mailbox，需要计算结果才提交任务。

## 第五步：把依赖从阻塞改为调度关系

旧代码可能在线程任务中等待另一个 future：

```cpp
auto load = executor_.submit(load_model);
auto plan = executor_.submit([&] {
    load.get();
    return build_plan();
});
```

第二个任务把依赖关系藏在任意 lambda 中，库无法校验句柄或统一传播依赖失败。迁移为任务句柄：

```cpp
auto load = executor_.submit_with_handle(load_model);
auto plan = executor_.submit_after(load.handle, build_plan);

load.future.get();
auto result = plan.get();
```

依赖关系现在由 Facade 显式表达，前置失败会传播到后续 future。`TaskHandle` 只属于创建它的 Kairo 实例，不能跨实例或跨已销毁运行时保存。

这项迁移改善的是正确性表达，不应被误解为“依赖等待不占 worker”。当前实现会把 dependent wrapper 提交到线程池，并在前置状态确定前等待；低线程数、长依赖链或大量先提交 dependent 的场景仍可能造成线程池饥饿。提交顺序应先前置、后依赖，并在目标最小线程数下做压力测试。若需要大规模 DAG 的完全非阻塞调度，应使用专门的图调度器，而不是扩大队列掩盖问题。

## 第六步：把永久循环留在正确的位置

下列代码不适合普通共享线程池：

```cpp
executor_.submit_auto([this] {
    while (running_) {
        auto frame = device_.blocking_read();
        process(frame);
    }
});
```

它永久占用一个 worker，I/O 可能无限阻塞，`shutdown(true)` 也无法安全中断它。按需求选择：

- 设备阻塞读取：需要 Kairo 管理 stop/wake/join 时用 `start_worker(BlockingWorkerSpec)`；否则由组件拥有可停止的 `std::jthread`，读取后以 `submit_auto(lambda)` 提交短计算；
- 软周期刷新：`submit_periodic()`，保存 task ID 并在退出时取消；
- 有 jitter 预算的控制循环：专用实时任务；
- 长期线程间传数据：`kairo::comm` 中与数据语义匹配的组件。

Kairo 与专用线程可以共存。成熟的迁移往往减少线程数量和重复调度代码，而不是让项目源码中再也看不到 `std::thread`。

## 第七步：建立可执行的关闭协议

服务增加 draining 状态，拒绝关闭开始后的新工作：

```cpp
std::future<ParsedFrame> ParserService::accept(Frame frame) {
    if (!accepting_.load(std::memory_order_acquire)) {
        throw std::runtime_error("parser is draining");
    }
    return executor_.submit_auto([frame = std::move(frame)]() mutable {
        return parse(std::move(frame));
    });
}

void ParserService::stop_accepting() {
    accepting_.store(false, std::memory_order_release);
}
```

应用退出顺序：

```cpp
parser.stop_accepting();

auto drained = executor.wait_for_completion(std::chrono::seconds{2});
if (!drained.completed) {
    log_pending(drained.status.pending_tasks);
}

executor.shutdown(drained.completed);
```

`wait_for_completion()` 超时不等于任务已取消；`shutdown(false)` 也不能替业务函数创造安全中断点。所有 I/O 和长任务仍应有自身的超时或协作停止机制。

注意：关闭后的 Kairo 不能重新初始化。需要“停止后重新启动”语义时，应重建拥有独立 Kairo 的组件，而不是复用已 shutdown 的实例。

## 迁移验收矩阵

| 场景 | 应观察到什么 |
| --- | --- |
| 正常解析 | future 返回结果，completed 计数增加 |
| 解析抛异常 | `future.get()` 抛出，failure status/callback 可观察 |
| 队列无法接收 | future 得到提交拒绝异常，拒绝计数增加 |
| 等待预算耗尽 | `WaitResult.timed_out` 为真，并包含 pending 快照 |
| draining 后提交 | 业务入口明确拒绝，不再进入 Kairo |
| 服务对象销毁 | 不存在仍捕获其引用的在途任务 |
| 进程退出 | 先停生产者，再排空，最后销毁任务依赖对象 |

建议用低线程数、小队列和故意阻塞的任务做压力验证；只运行正常路径无法证明迁移消除了竞态。

## `std::thread`、`std::async` 与 Kairo 怎么选

| 需求 | 默认选择 | 原因 |
| --- | --- | --- |
| 一个长期、可停止的阻塞 I/O owner | `start_worker(BlockingWorkerSpec)` 或 `std::jthread` | 前者由 Facade 管理 stop/wake/join；两者都避免占用共享 worker |
| 一个局部作用域内少量并行计算 | `std::async` 或同步算法 | 不一定需要引入共享运行时 |
| 多模块短任务、统一容量与诊断 | Kairo `submit_auto(lambda)` | 共享执行资源、future、路由解释、状态和关闭入口 |
| 数量可控、完成关系明确的多阶段计算 | Kairo 任务依赖 | 显式校验关系并统一传播前置失败 |
| 严格周期循环 | Kairo 实时任务 | 专用线程、周期配置与状态 |
| 跨线程持续传递数据 | `kairo::comm` | 数据语义、背压与关闭比任务提交更重要 |

迁移完成后，继续用[生产接入检查清单](/zh/guides/production-readiness)评审容量和关闭；遇到可疑写法时对照[并发架构反模式](/zh/guides/concurrency-antipatterns)。
