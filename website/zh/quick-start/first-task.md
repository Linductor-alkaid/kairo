---
title: 第一个任务
description: 使用 submit_auto 和 future.get 执行任务、获取返回值、异常与默认路由决策。
---

# 第一个任务

## 学习目标

使用 `Executor::instance()` 获取 Facade，通过 `submit_auto()` 提交一个返回 `42` 的任务，并通过 `future.get()` 获取结果和异常。

## 场景问题

你需要把一段计算移到后台执行，但调用方仍要可靠地获得结果或得知它失败了。

## 推荐方案

使用 `submit_auto()`。普通 lambda 的默认意图是 `Auto`，当前只选择默认异步线程池；它返回 `std::future`：任务成功时 `get()` 返回值，任务抛出异常时 `get()` 在调用方线程重新抛出异常。首次提交会使用默认配置完成懒初始化。

这不是“从所有执行器中自动选最快一个”：普通 lambda 不会改投 GPU、无锁或实时执行器。需要知道 `submit_auto`、`dispatch_auto` 和 `start_worker` 如何按 intent 与名称精确匹配目标时，阅读[自动路由如何匹配目标](/zh/guides/execution-models-and-routing)。

<<< @/../examples/tutorial/01_first_task.cpp{4,7-16,18-24,26}

完整源码：[`examples/tutorial/01_first_task.cpp`](https://github.com/Linductor-alkaid/kairo/blob/master/examples/tutorial/01_first_task.cpp)。

```bash
./build/examples/tutorial/tutorial_01_first_task
```

## 预期输出

```text
answer=42
task failed: expected tutorial failure
```

## 为什么这样做

- `future.get()` 不只是取值，也是异常传播边界；忽略它会让失败不再由这里观察。
- `get_last_routing_decision()` 显示本次默认选择；它解释“为什么走这条路径”，不替代 future 的完成或异常结果。
- 默认配置允许懒初始化，适合这个最小示例。
- 需要自定义线程数、队列容量或监控时，必须在第一次提交前调用 `initialize()`。
- 示例最后调用 `shutdown()`；这里没有待处理任务，`shutdown()` 与 `shutdown(true)` 的完成结果相同。业务程序应按下一页的规则选择关闭语义。

## 常见错误

- **把 `Auto` 当成性能自动优化**：普通 `Auto` 不会偷偷改投无锁、实时或 GPU 后端；它只是安全的默认异步入口。
- **只提交，不保存 future**：适合明确的 fire-and-forget 任务，但你失去返回值与异常传播路径。
- **把 `submit_periodic()` 当实时任务**：它是普通线程池的软周期调度；严格控制循环请使用后续的实时教程。
- **首次提交后再初始化**：配置可能已经无法按预期生效；请先完成初始化。

## 下一步阅读

先阅读[提交自己的函数与数据](/zh/quick-start/task-inputs-and-ownership)，学习如何传入自由函数、成员函数、参数和业务对象；然后由[返回值与异常](/zh/quick-start/return-values-and-errors)解释如何选择和补充失败观察路径。需要判断何时离开默认路径时，阅读[执行模型与路由边界](/zh/guides/execution-models-and-routing)。
