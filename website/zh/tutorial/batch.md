---
title: 批量处理传感器帧
description: 在需要或不需要逐项结果时选择 submit_batch、submit_batch_no_future 与 submit_batch_priority。
---

# 批量处理传感器帧

## 学习目标

一次提交一批同类处理工作，并根据是否需要逐项结果选择带 `future` 或 fire-and-forget 路径。

## 场景问题

一个采集周期收到了多帧独立数据。逐个 `submit_auto(lambda)` 当然可行，但提交端明确知道这些工作是一批，应该先表达这一语义，再决定是否需要结果。

## 推荐方案

需要逐项完成确认时，使用 `submit_batch()`：

<<< @/../examples/tutorial/04_batch.cpp{1-29}

完整源码：[`examples/tutorial/04_batch.cpp`](https://github.com/Linductor-alkaid/kairo/blob/master/examples/tutorial/04_batch.cpp)。

```bash
./build/examples/tutorial/tutorial_04_batch
```

## 预期输出

```text
batch processed=6, completed=yes
```

## 三种批量路径

| 需求 | 接口 | 如何观察失败 |
| --- | --- | --- |
| 每项需要完成或异常结果 | `submit_batch()` | 对每个 `future` 调用 `get()`。 |
| 不需要逐项结果 | `submit_batch_no_future()` | 结合失败回调/状态，并使用有界等待或关闭语义。 |
| 一整批控制工作更紧急 | `submit_batch_priority(priority, tasks)` | 与 `submit_batch()` 一样逐项检查 future。 |

示例先检查 `submit_batch()` 的全部 future，再提交 no-future 批次，并以 `wait_for_completion_for()` 做有界收尾。

## 每个批量任务如何接收输入

batch 不提供 `submit_batch(fn, args...)` 形式。它接收的是一组已经绑定好输入、可独立调用的 `void()` callable，常用 `std::vector<std::function<void()>>`：

```cpp
std::vector<std::function<void()>> tasks;
for (SensorFrame frame : frames) {
    tasks.push_back([frame, processor] {
        processor->process(frame);
    });
}
auto futures = executor.submit_batch(tasks);
```

这里每个 lambda 各自拥有一个 `frame` 副本，并共享 `processor` 的生命周期。不要写 `[&frame]` 捕获循环变量；循环进入下一轮或离开作用域后，任务可能读到同一对象或悬空引用。大型帧可捕获有明确归还协议的 buffer handle，或捕获 `shared_ptr<const FrameData>`，但不能只传一个无法证明存活时间的 view。

任务列表和其中的 callable 当前需要可复制；move-only 资源应由可复制的共享 owner 间接持有，或改用逐项 `submit_auto(lambda)` 的移动捕获 lambda。batch future 只表示每个 `void()` callable 是否完成，不自动收集业务返回值。

## 运行假设与所有权

示例只有三项任务，每项只递增一个原子计数器；同一任务列表被提交两次，所以最终计数为六。lambda 捕获的 `processed` 必须活到两批任务全部完成，原子类型只解决这个计数器的数据竞争，不代表任意业务对象都能安全共享。

batch 适合由同一生产者同时产生、相互独立且调度语义一致的工作。返回的 futures 与输入位置一一对应，但任务完成顺序不固定。若第 2 项依赖第 1 项结果，就不再是独立批次，应改用依赖或在单个任务内串行处理。

`submit_batch()` 将任务列表交给一次底层批量接收调用；批次含空任务，或执行器已经停止而无法接收时，每个 future 都会得到异常结果，不应只检查第一个。不要把这理解成业务事务：一项运行失败不会回滚已经执行的其他项。`submit_batch_priority()` 当前逐项走 priority 提交，也不能假设它与普通 batch 有相同的接收边界。

## 为什么这样做

批量接口表达“这些工作属于同一提交批次”，可以减少重复提交路径的开销；但收益取决于任务体、数量、线程数、硬件和构建配置。不要承诺固定倍率，性能判断请运行仓库的 [`benchmark_batch_submit_real.cpp`](https://github.com/Linductor-alkaid/kairo/blob/master/tests/benchmark_batch_submit_real.cpp)。

## 常见错误

- **把 no-future 当成无失败**：它只省去 future 管理，不会让任务异常消失；需要为长期运行程序配置失败可观察路径。
- **只因任务数量多就改用 batch**：少量或彼此有依赖的任务，普通提交或任务依赖通常更清晰。
- **用优先级替代背压策略**：队列持续堆积时，优先级不能消除容量和截止时间问题。

## 故障注入与退出

1. 让批次中间一项抛异常；遍历全部 futures，确认其他独立项的结果仍被逐项处理。
2. 在批次中放入空 `std::function`，以及在 Kairo shutdown 后尝试提交；确认所有返回 futures 都以异常结束，或入口直接给出明确拒绝，而不是永久等待。
3. 在 no-future 任务中抛异常；确认 failure callback/status 能定位 `facade_submit_batch_no_future[index]`。
4. 让任务比退出预算更长；`wait_for_completion_for()` 返回 false 后记录 pending，并执行预先定义的继续等待或快速关闭策略。

关闭前必须保证 `tasks` 捕获的引用仍然有效。no-future 路径尤其容易让调用方误以为函数返回后输入可以销毁；事实上任务可能尚未开始。

## 需求变化时如何演进

| 新需求 | 下一步选择 |
| --- | --- |
| 每项要返回不同结果 | 循环 `submit_auto(lambda)`，或在业务层为结果建立索引容器 |
| 一项失败后其余项必须停止 | 设计共享停止标志和幂等任务；普通 batch 默认独立执行 |
| 数据太大，不宜复制到每个 lambda | 使用所有权明确的 buffer/view，并保证底层存储活到完成 |
| 批次持续到达并造成积压 | 上游限流、分块和容量预算，不无限扩大队列 |
| 任务之间存在先后或汇合 | 使用 `TaskHandle` / `when_all()`，不要依赖提交顺序 |

## 下一步阅读

[让加载、感知和规划按依赖执行](/zh/tutorial/dependencies)说明如何表达先后关系，而不是在任务体内阻塞等待。
