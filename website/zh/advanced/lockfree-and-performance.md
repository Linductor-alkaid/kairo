---
title: 无锁与性能实验
description: 在明确的单消费者高频场景评估 LockFreeTaskExecutor，并正确理解内部队列、对象池和 benchmark。
---

# 无锁与性能实验

## 学习目标

判断 `LockFreeTaskExecutor` 是否比 Facade 更适合一个高频聚合场景，并用正确的统计、压力测试和 benchmark 验证结论。

这是性能专家路径，不是普通任务的默认优化。普通有限工作使用 `submit_auto(lambda)`；只有业务已经确认 MPSC、单消费者和有界拒绝就是正确语义时才进入本页。若希望在统一控制面完成命名、生命周期和接收诊断，先使用 `dispatch_auto(LowLatency)`；它返回 admission，不提供 future 完成承诺。

## 何时使用 LockFreeTaskExecutor

它面向多生产者、单消费者的任务聚合，例如高频日志、异步事件或性能敏感的单消费者分发。Facade 路径要求先注册、启动并显式指定目标：

```cpp
TaskOptions options;
options.intent = ExecutionIntent::LowLatency;
options.preferred_executor = "telemetry";
auto admission = executor.dispatch_auto(options, [] { process_event(); });
if (!admission.accepted) {
    // stopped、queue full、pool exhausted 或 shutdown competition
}
```

`LowLatency` 与 `preferred_executor` 必须同时匹配同名、已启动的无锁执行器；它不会查找其他可用队列，也不会回退线程池。完整规则见[自动路由如何匹配目标](/zh/guides/execution-models-and-routing)。

直接操作执行器仍适合需要精细生命周期或 benchmark 控制的场景：

```cpp
kairo::LockFreeTaskExecutor queue(1024);
queue.start();

if (!queue.push_task([] { process_event(); })) {
    // 空任务、已停止、队列满或对象池耗尽：执行背压策略。
}
queue.stop();
```

完整示例：[`examples/lockfree_task_executor_example.cpp`](https://github.com/Linductor-alkaid/kairo/blob/master/examples/lockfree_task_executor_example.cpp)。它不是普通 `Executor` 的“更快替代品”：没有 future 返回值、任务依赖、统一 failure callback 或多 worker 执行能力。多数业务任务仍应先使用 Facade。

## 生命周期与可观察性

调用 `start()` 启动唯一消费者，`stop()` 停止并等待线程。`push_task()` 失败必须由调用方处理；检查 `pending_count()`、`processed_count()`、`exception_count()`、`rejected_empty_count()` 与 `get_queue_stats()`。任务异常由 worker 捕获并累计；如需异常对象，注册 `set_exception_handler()`，且 handler 本身要短小、线程安全、不抛出。

## 状态快照与背压诊断

监控线程推荐使用 `get_status_snapshot()` 取得一份按值可复制的 `QueueStats`：

```cpp
kairo::LockFreeTaskExecutor exec(4096, 2, /*enable_stats=*/true);
exec.start();

std::thread monitor([&] {
    while (auto s = exec.get_status_snapshot(); s.processed_count < kDone) {
        if (s.ready_count > s.queue_capacity / 2) {
            // 消费者落后；考虑扩容、降采样或临时限流。
        }
        std::this_thread::sleep_for(100ms);
    }
});
```

快照由多个独立原子读取组成，**所有字段均为近似、非同步值**：不能用作同步原语或正确性判定依据，仅适合趋势与告警。

`failed_pushes` 汇总所有底层入队失败；在静止快照中，下列三个原因计数之和等于它，分别对应不同的处置：

| 字段 | 来源 | 典型处置 |
| --- | --- | --- |
| `queue_full_rejections` | 队列满 | 扩容或提高消费者吞吐。 |
| `contention_rejection` | CAS 竞争耗尽重试预算 | 降低 producer 数或调高 backoff multiplier。 |
| `reservation_cancelled_rejections` | 消费者取消了生产者的 reservation | 检查生产者抢占和 reservation 窗口。 |
| `cancelled_reservation_count` | 消费者在有界等待（默认 64 次 yield）后取消了 reservation | 检查生产者是否在 `Writing` 窗口被抢占或持有锁。 |
| `submission_rejection` | 执行器入口拒绝：空任务、已停止、对象池耗尽 | 多为上游逻辑或 `stop()` 竞态，少量空任务是调用方 bug。 |

持续 `reserved_count` 升高而 `ready_count` 不动说明生产者卡在预留窗口；持续 `cancelled_reservation_count` 升高说明消费者侧取消恢复频繁；`submission_rejection` 持续增长而 `contention_rejection` 为零则把注意力放在调用方与生命周期，而不是队列本身。完整字段表与默认 64 次 yield 预算见 [`docs/API.md` §5.5](https://github.com/Linductor-alkaid/kairo/blob/master/docs/API.md)（“状态快照与背压诊断”章节）。新测试 `tests/test_lockfree_queue_status.cpp` 覆盖了按值复制与并发采样不阻塞生产者的契约。

## 当前内部结构

### 生产、消费与停止

每次 `push_task()` 先拒绝空函数，再通过 `active_pushes_` 登记生产者，随后从固定容量 `ObjectPool` 获取 wrapper，写入 `std::function` 后尝试压入有界 `LockFreeQueue`。任一环节失败都返回 `false`；调用方要把它视作明确背压，而不是重试到无限期。

唯一消费者一次最多 `pop_batch(32)` 项，逐个执行并归还 wrapper。空队列时采用三级退避：短暂 CPU pause、自旋后 `yield`，再退化为 1 微秒 sleep；停止时先阻止新 producer、等待已登记 producer 离开，再停止 worker 并 drain 剩余队列。这个顺序避免 producer 在最终 drain 后继续放入任务。

### “无锁”的实际范围

当前实现中的 `LockFreeQueue` 提供有界入队/出队；`ObjectPool` 预分配任务包装以避免热路径分配。对象池的正确性优先于“所有东西都无锁”：其 acquire/release 使用 mutex 保护 free list，以避免 ABA、外来指针和重复释放问题。异常 handler 的读取/更新同样受 mutex 保护。换言之，队列是无锁的，执行器整体不是 lock-free 进度保证；这是用可验证的内存回收正确性换取的工程选择。

## MPSC 槽位协议：为什么每个槽有序列号

队列容量会向上取整为 2 的幂，`index = position & (capacity - 1)`，因此环绕只需要位运算。真正防止“消费者读到半写数据”或“生产者覆盖未消费数据”的不是 index，而是每个槽位的 `sequences_[index]`：

```text
初始化槽 i：sequence = i

producer 预留 position p：要求 sequence == p
producer 写 buffer[i]
producer release-store sequence = p + 1

consumer 读取 position p：要求 sequence == p + 1
consumer 读 buffer[i]
consumer release-store sequence = p + capacity
```

下一轮生产者要复用同一槽位时，看到 `p + capacity` 才知道消费者已经读完。`enqueue_pos_` 的 CAS 只负责多个 producer 竞争一个位置；它不代表数据已经可读，所以消费者必须继续检查 slot sequence。

### acquire/release 在这里各自保证什么

- producer 写完 `buffer_[index]` 后 release-store sequence；consumer acquire-load sequence 成功后，才能读取这次写入。
- consumer 读完 buffer 后 release-store“下一轮可写”的 sequence；producer acquire-load 看到它后，才能覆盖槽位。
- position CAS 使用 relaxed，因为它只分配唯一位置；数据可见性由 sequence 的 release/acquire 配对承担。

如果把 sequence store 改成 relaxed，x86 上可能长时间“看起来正常”，但弱序 ARM 上 consumer 可能先看到 ready 标记再读到未完整发布的数据。修改内存序必须在目标架构和 TSAN/压力测试上验证，而不是凭单机 benchmark 判断。

### 空、满和近似值

队列保留一个空槽；当 `enqueue_pos - dequeue_pos >= capacity - 1` 时拒绝 push。`empty()` 从消费者下一个槽位的 sequence 判断“现在能否 pop”，`size()` 从两个 position 的差估计容量占用；并发下它们都只是瞬时近似，可能短暂不一致。

因此：

- 用 `pop()`/`push()` 的返回值决定实际同步结果；
- 不用 `empty()` 做严格条件变量谓词；
- 不用 `size()` 精确决定应分配多少内存或是否安全关闭；
- `pending_count()` 和监控中的 current size 只能做趋势和告警。

## 多生产者退避与公平性

CAS 失败后，队列执行 `PAUSE`/平台 yield 风格的指数退避，最多重试固定次数；队列满或槽位状态不匹配则返回失败。退避乘数可以降低高竞争下总线压力，但会提高单次 push 等待时间，不能把它当成阻塞队列的“自动等待直到成功”。

高并发下 producer 之间没有 FIFO 公平保证：某个线程可能连续 CAS 成功，另一个线程反复失败。若业务要求每个请求都被接受，应在上层使用有界阻塞/重试策略并设置 deadline；无限重试会把明确背压变成线程饥饿。

## 批量路径为什么要先检查所有槽位

`push_batch()` 先读取一段连续槽位的 sequence，确认整批候选位置仍处于可写状态，再 CAS 一次性预留位置，最后逐槽写入并 release-store ready sequence。CAS 之后才发现中间槽位不可用，会让 position 已经跳过但数据尚未完整发布，消费者可能看到空洞，因此当前实现把预校验放在 CAS 之前。

普通 `push_batch()` 在剩余容量不足时尽量写入可容纳部分，并通过 `pushed` 告诉调用方实际数量；`push_batch_exact()` 则要求整批可用，否则整批失败。上层批量 API 必须对未推送部分保留 future 的拒绝/异常兑现，不可把“函数返回成功”误当作每一项都接受。

## LockFreeTaskExecutor 的对象生命周期

队列里存的不是 `std::function` 本身，而是预分配 wrapper 指针：

```text
push_task
  → 检查函数非空
  → active_pushes_++
  → ObjectPool::acquire
  → 写 wrapper->func
  → LockFreeQueue::push(pointer)

consumer pop
  → 执行 wrapper->func
  → 清理函数
  → ObjectPool::release
```

对象池耗尽与队列满是两种不同的拒绝原因；前者可能发生在队列尚有空间时。`stop()` 先阻止新 producer，等待 `active_pushes_` 归零，再停止消费者并 drain 剩余 wrapper，避免 pool 销毁后 producer 还持有指针。

这也是为什么不能把一个外部指针直接塞进队列后立刻释放，也不能在 task wrapper 执行期间销毁它捕获的业务对象。无锁只减少队列同步，不替代对象所有权协议。

## 从性能数字回到机制

看到 p99 变差时按机制分流：

| 现象 | 可能机制 | 先查什么 |
| --- | --- | --- |
| producer 数增加后 push p99 急升 | CAS 竞争和退避 | failed pushes、producer 数、backoff multiplier |
| 吞吐高但失败率也高 | 队列容量或消费速率不足 | `failed_pushes`、capacity、consumer drain 速率 |
| 单 producer 很快，多 producer 反而下降 | 共享 enqueue position/cache line 争用 | 固定任务体和 CPU 亲和性后重测 |
| `pending_count()` 偶发为 0 但仍有工作 | 近似快照，不是同步屏障 | 以 pop/completed 对账，别用 size 做完成判断 |
| stop 偶发等待 | producer 尚未退出或 wrapper 执行阻塞 | active pushes、任务体停止点、drain 计数 |

不要只优化 CAS 循环。对象池 mutex、函数对象分配、任务体成本、CPU 频率和消费者执行时间可能才是端到端瓶颈。

线程池内的 `WorkerLocalQueue`、工作窃取和对象池同样是实现细节，不能从 `src/` 直接集成。

## 如何验证性能

| 问题 | 验证方法 |
| --- | --- |
| 是否存在数据竞争或生命周期错误？ | TSAN、并发压力测试和边界测试。 |
| 满队列时业务是否正确降级？ | 断言失败返回、drop/拒绝计数与恢复行为。 |
| 特定硬件上是否更快？ | 固定构建类型、CPU、负载、线程数和数据规模后运行 benchmark。 |
| 是否改善真实端到端延迟？ | 测量提交、排队、执行和尾延迟，而非只看单次 microbenchmark。 |

性能数字只在测量环境内成立。记录版本、编译器、构建类型、硬件、频率/功耗策略、任务体、并发度与统计方法；不要把一次基准结果写成接口承诺。

源码级修改至少做三组验证：

1. **协议正确性**：单 producer、多个 producer、满队列、批量部分成功、stop 竞争；对账 `accepted = completed + rejected`。
2. **内存模型**：TSAN/压力测试，至少覆盖弱序架构或交叉编译环境；不要只依赖 x86。
3. **性能分布**：固定 producer 数、容量、任务体和 CPU 集合，保存吞吐、失败率、p50/p99、最大延迟与 CPU 占用。

基准中的 `size()`、`pending_count()` 和 queue stats 是观察值，不要用它们替代完成计数或 future 对账。

对应源码入口：[`src/kairo/lockfree_task_executor.cpp`](https://github.com/Linductor-alkaid/kairo/blob/master/src/kairo/lockfree_task_executor.cpp)、[`include/kairo/lockfree_task_executor.hpp`](https://github.com/Linductor-alkaid/kairo/blob/master/include/kairo/lockfree_task_executor.hpp)、[`src/kairo/util/lockfree_queue.hpp`](https://github.com/Linductor-alkaid/kairo/blob/master/src/kairo/util/lockfree_queue.hpp) 和 [`src/util/object_pool.hpp`](https://github.com/Linductor-alkaid/kairo/blob/master/src/util/object_pool.hpp)。

## 下一步阅读

先用[源码结构与阅读路线](/zh/advanced/source-architecture)理解同步域，再使用[性能测量与回归检查](/zh/advanced/performance-measurement)固定实验环境和指标定义；回到[API 参考](/zh/reference/api)确认公开接口。性能报告和内部优化历史可供参考，但不是用户程序可以依赖的契约。
