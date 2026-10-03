---
title: 返回值与异常
description: 使用 future.get 获取任务结果，并在调用方观察任务异常。
---

# 返回值与异常

## 学习目标

理解 `submit_auto(lambda)` 与显式 future 型接口都会通过 future 返回值和任务异常。

## 推荐方案

对需要结果或需要确认成功的任务，保存 future 并调用 `get()`：

```cpp
auto value = executor.submit_auto([] { return 42; });
std::cout << value.get() << '\n';

auto work = executor.submit_auto([] { /* side effect */ });
work.get(); // 仍然必须观察异常
```

任务内部抛出的异常不会在 worker 线程直接终止程序，而会在对应 `get()` 调用处重新抛出。`submit()` 以及显式 priority、delay、batch 和 dependency 接口仍保留各自的 future 语义。教程示例的第二个任务展示了这一点。

```cpp
try {
    static_cast<void>(failed.get());
} catch (const std::exception& error) {
    std::cerr << "task failed: " << error.what() << '\n';
}
```

不要把异常误当作初始化失败：`initialize()` 的失败通过 `ExecutorResult` 的 `ok`、`error_code` 与 `message` 表达；任务失败则由 future、failure callback 或 failure status 观察。

`DispatchResult::accepted` 和 `WorkerHandle` 不是 future：前者表示有界队列接收，后者表示 worker 生命周期。不要把它们当作完成；先阅读[执行模型与路由边界](/zh/guides/execution-models-and-routing)。

## 不适用场景

明确允许失败仅通过状态、回调或日志处理的 fire-and-forget 工作，可不保留 future；但应为失败选择另一条可观察路径。长期运行服务可结合 `set_failure_callback()` 和状态查询，不能把“没有调用 get”当成“没有失败”。

## 下一步

阅读[初始化与关闭](/zh/quick-start/lifecycle)，了解何时使用 `initialize()` 和 `shutdown(true)`。
