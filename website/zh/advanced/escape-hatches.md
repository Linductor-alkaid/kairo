---
title: 何时使用高级接口
description: 在资源隔离、执行器扩展或 GPU 资源控制确有需要时，谨慎离开 Kairo Facade。
---

# 何时使用高级接口

## 学习目标

判断何时继续使用 `Executor` Facade，何时需要独立实例、`ExecutorManager` 或直接执行器指针，并明确每种选择新增的生命周期和并发责任。

## 默认与高级入口

| 需求 | 默认选择 | 何时进入高级入口 |
| --- | --- | --- |
| 普通后台、周期、依赖、监控 | `Executor` Facade | 不需要。 |
| 与其他子系统隔离线程和关闭时机 | 独立 `Executor` 实例 | 需要独立资源和确定析构。 |
| 注册自定义执行器或统一管理多个执行器 | `ExecutorManager` | 你负责所有权转移、命名与 shutdown。 |
| 直接使用实时队列/周期实现细节 | `get_realtime_executor()` | Facade 推送不足，且能处理状态、拒绝和停止竞争。 |
| 设备内存、stream、P2P 或统一内存 | `get_gpu_executor()` | 已验证后端与资源生命周期。 |

## 实例隔离

`Executor::instance()` 使用进程共享的 manager，适合一个应用的默认执行资源。直接构造 `kairo::Kairo executor;` 会创建独立 `ExecutorManager`，因此线程池、实时/GPU 注册表和关闭时机不与单例共享。库对象析构会按 RAII 清理其 manager；仍建议业务在停止产生任务后显式调用 `shutdown()`，以决定是否等待已提交任务。

不要跨实例传递 `TaskHandle`、实时/GPU executor 指针或依赖关系。它们附着于创建它们的 manager；跨实例共享会失去正确的生命周期与并发语义。

## Manager 与直接指针的责任

`ExecutorManager` 可创建、注册和获取 `IAsyncExecutor`、`IRealtimeExecutor`、`IGpuExecutor`。注册 API 接收 `std::unique_ptr`，所有权随之转移；创建不等于注册。`get_*_executor()` 返回的直接指针由 manager 持有，调用方不得释放、长期缓存到 manager shutdown 之后，或与并发关闭竞态使用。

需要把对象安全持有到一次调用结束，或需要与并发 `shutdown()` 共存时，使用 manager 的 `get_*_executor_snapshot()`。它返回 `std::shared_ptr`：注册表移除后对象会继续存活到该快照释放。它不是“保持运行”句柄，shutdown 仍可请求停止后端；调用方仍须检查提交结果、future 和状态。

Facade 已经把常见的拒绝、failure event 和状态聚合在一起。直接接口可能绕过这些统一观察路径，因此调用方必须自行检查返回值、future、状态和资源关闭。

## 稳定性边界

`include/kairo/` 中的 Facade、配置、接口和 manager 声明是公开 API。`src/` 中的 `ThreadPool`、调度器、队列和对象池是实现细节：可用于理解当前行为或排障，但不能作为集成依赖或兼容承诺。

## 下一步阅读

需要外部时钟时阅读[接入自定义周期源](/zh/advanced/custom-cycle-manager)；想理解当前执行链路请看[任务如何穿过执行器](/zh/advanced/execution-paths)。
