---
title: 让加载、感知和规划按依赖执行
description: 用 TaskHandle、submit_after 与 when_all 构建可观察的任务依赖。
---

# 让加载、感知和规划按依赖执行

## 学习目标

使用 `submit_with_handle()` 保留任务结果和依赖句柄；以 `submit_after()` 和 `when_all()` 表达“加载与感知完成后再规划”。

## 场景问题

规划不能在地图加载和感知预处理完成前启动。在业务 lambda 中直接 `future.get()` 会隐藏依赖关系，也无法让 Facade 校验句柄并统一传播前置失败。

## 推荐方案

先提交带句柄的前置任务，汇合句柄后再提交规划：

<<< @/../examples/tutorial/05_dependencies.cpp{1-25}

完整源码：[`examples/tutorial/05_dependencies.cpp`](https://github.com/Linductor-alkaid/kairo/blob/master/examples/tutorial/05_dependencies.cpp)。

```bash
./build/examples/tutorial/tutorial_05_dependencies
```

## 预期输出

```text
plan score=42
```

## 构建关系

- 单个前置任务：`submit_after(load.handle, plan)`。
- 多个前置任务：`submit_after({load.handle, sense.handle}, plan)`；示例使用等价的 `when_all()` 先创建汇合句柄。
- 还要继续依赖规划时，使用 `submit_after_with_handle()`；它与 `submit_with_handle()` 一样同时给出 `future` 和 `TaskHandle`。
- `when_all(handles)` 只表达“全部完成”的汇合点，可嵌套使用。

## 依赖任务的输入何时保存

`submit_with_handle(fn, args...)`、`submit_after(handle, fn, args...)` 与显式 `submit(fn, args...)` 使用相同的 callable 和参数模型，但 dependent 的输入在**提交依赖任务时**就被保存，不是等前置成功后才从调用方读取。普通无依赖有限工作仍从 `submit_auto(lambda)` 开始：

```cpp
PlanConfig config = load_plan_config();
auto plan = executor.submit_after(prerequisites, run_planner, config);
```

后续修改调用方的 `config` 不会更新任务已经保存的副本。若依赖任务要消费前置任务的返回值，`TaskHandle` 本身不会传值；应让前置任务把结果写入所有权明确的共享对象，或像本例一样保留对应 future，并只在依赖已满足后读取。

引用输入必须从 dependent 提交时一直活到它最终执行完成，等待前置任务的时间也算在内。因此依赖链越长，越应该按值保存不可变配置，或捕获 `shared_ptr`；不要把调用栈局部变量通过 `[&]` 交给一个可能很久以后才运行的 dependent。

## 当前容量边界

任务句柄让完成关系和失败传播显式化。依赖调度为 dependency-driven：依赖未就绪的 dependent 不入队、不占用 worker，驻留调度侧，依赖全部成功后才按提交时的 priority 入队——低线程数、长依赖链或先提交 dependent 都不再占住 worker；`max_in_flight_tasks` 与 `task_graph_retention_capacity` 仍是图的规模边界。

实际使用时：

- 依赖等待期计入 `task_timeout_ms` 的 queued soft timeout 预算（自提交起算）；不需要超时就保持默认 0；
- 不在 dependent 中继续进行无界阻塞；
- 控制同时在途的依赖链数量；
- 用生产配置中的最小线程数进行压力测试；
- 大规模动态 DAG 需要专门图调度器时，不要只靠增大线程池和队列规避问题。

## 运行假设与所有权

示例只有两个前置任务和一个 dependent，并固定两个 worker。`load`、`sense` 及其 futures 都由 `main()` 持有到 `plan.get()` 完成；dependent lambda 捕获引用是安全的，仅因为这些局部对象在整个等待期间仍然存在。把同样 lambda 从函数中返回会产生悬空引用。

`TaskHandle` 只表达完成状态，不携带任务返回值。dependent 仍需通过对应 future 或业务状态取得结果，因此 handle owner、future owner 和结果数据 owner 都要写清楚。`when_all()` 是汇合状态，不会生成一个结果 tuple。

## 失败如何观察

前置任务失败时，其 `future.get()` 会重新抛出异常；依赖任务默认不会运行，并在自己的 `future.get()` 上给出失败。无效句柄和跨 `Executor` 实例的句柄也是可观察的拒绝，不要把句柄跨实例保存或混用。

## 故障注入与退出

1. 让 `load` 抛异常；确认 `plan` 任务体不执行，`load.future` 与 `plan` future 都能解释失败。
2. 传入默认构造或另一 Kairo 创建的 handle；确认 dependent future 得到可观察拒绝。
3. 用最小线程数提交多组长依赖链；观察 active/queued 和完成情况——依赖等待不再占住 worker，链应全部完成而非饿死。
4. 在依赖未完成时开始退出；先停止创建新图，保留所有 futures，再有界等待或记录未完成图的业务 ID。

Kairo shutdown 不会把内存中的任务图持久化。若流程必须跨进程恢复，应把阶段和输入写入外部存储，并让任务幂等。

## 需求变化时如何演进

| 新需求 | 下一步选择 |
| --- | --- |
| 继续依赖 plan 的结果 | 使用 `submit_after_with_handle()` 保留新 handle |
| 任一前置失败后执行补偿 | 在调用方观察 futures 后显式提交补偿，不把失败路径藏进成功 DAG |
| 大规模动态 DAG | 使用专门图调度器并定义资源配额 |
| 持续帧流逐条处理 | 使用 channel；任务图不是数据流 |
| 初始化、标定、运行阶段推进 | 使用 `PhaseGate`；它表达阶段而非计算返回值 |

## 何时不选

任务图适合数量可控、完成关系明确的依赖，不适合大规模非阻塞 DAG、连续数据流、最新值覆盖或严格周期阶段；这些问题应使用专门图调度器、通信组件或实时路径。

## 下一步阅读

[有界等待与状态快照](/zh/tutorial/waiting-and-status)说明如何在关闭或阶段切换时等待工作完成，而不无限阻塞。
