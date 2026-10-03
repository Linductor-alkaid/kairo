---
title: 初始化与关闭
description: 在需要配置时先初始化，并在程序边界有序关闭 Kairo。
---

# 初始化与关闭

## 场景问题

最小程序可以依赖懒初始化；但当线程数、队列容量或监控需要自定义时，必须在首次提交前固定配置。

## 推荐方案

```cpp
kairo::ExecutorConfig config;
config.min_threads = 2;
config.max_threads = 4;

auto& executor = kairo::Executor::instance();
auto initialized = executor.initialize(config);
if (!initialized) {
    throw std::runtime_error(initialized.message);
}

// submit_auto(lambda) ... future.get() ...
executor.shutdown(true);
```

`initialize()` 返回带错误码和消息的 `ExecutorResult`，比兼容的 `bool` 初始化接口更适合诊断。`shutdown(true)` 会等待已接受的异步任务完成；如果等待超过默认上限，库会记录超时诊断并改走非等待关闭，不能把它理解为无限等待。

单例在进程退出时有 `shutdown(false)` 兜底，独立 `Executor` 实例由析构负责回收；两者都不替代应用在业务边界显式决定是否等待的责任。

低频健康检查或关闭现场记录可使用 `get_snapshot()`。它返回当前生命周期、已注册后端状态、失败摘要和聚合计数，且只读查询不会触发默认异步执行器懒初始化；因此未初始化实例查询后仍为 `Created`。应把它当作 best-effort 诊断快照，而不是提交 reservation 或全局事务读。

## 常见错误

- 首次 `submit_auto()` 或 `submit()` 后再修改初始化配置：已完成懒初始化时，配置不会按预期替换。
- 进程退出前既不等待 future，也不调用关闭，使业务完成顺序不清楚。
- 把任务异常和初始化失败混为一谈：前者通常经 future 观察，后者由 `ExecutorResult` 表达。

## 下一步

查看[如何选择提交接口](/zh/guides/choosing-submit-api)，按业务问题进入优先级、延迟、周期和批量教程。
