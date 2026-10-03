---
title: 接入自定义周期源
description: 使用 ICycleManager 把外部时钟或周期框架接入实时任务，并承担完整生命周期责任。
---

# 接入自定义周期源

## 学习目标

仅在内置 `sleep_until` 周期无法满足外部时钟、硬件触发或既有控制框架时，实现 `ICycleManager` 并注入 `RealtimeThreadConfig`。

## 接口契约

`ICycleManager` 有四个责任：`register_cycle(name, period_ns, callback)` 保存周期定义，`start_cycle(name)` 运行周期，`stop_cycle(name)` 请求停止，`get_statistics(name)` 返回周期统计。Kairo 不拥有该对象；它必须在实时任务注册、运行和停止的整个期间保持有效。`start_cycle()` 通常在实时线程启动路径中同步运行周期循环，因此实现者还必须保证它只在 `stop_cycle()` 后返回，并且不会意外阻塞调用方的关闭路径。

最小结构如下：

```cpp
class ExternalClock final : public kairo::ICycleManager {
public:
    bool register_cycle(const std::string& name, int64_t period_ns,
                        std::function<void()> callback) override;
    bool start_cycle(const std::string& name) override;
    void stop_cycle(const std::string& name) override;
    kairo::CycleStatistics get_statistics(const std::string& name) const override;
};
```

将对象地址放到 `RealtimeThreadConfig::cycle_manager` 后，仍通过 Facade 注册与停止实时任务：

```cpp
ExternalClock clock;
kairo::RealtimeThreadConfig config;
config.cycle_manager = &clock;
config.cycle_callback = [] { run_control_cycle(); };

executor.register_realtime_task_ex("control", config);
executor.start_realtime_task_ex("control");
// 停止前确保 clock 仍然存活。
executor.stop_realtime_task("control");
```

完整的最小实现见 [`examples/realtime_can.cpp`](https://github.com/Linductor-alkaid/kairo/blob/master/examples/realtime_can.cpp)。

## callback 与周期源如何接收输入

`register_cycle()` 收到的是已经绑定输入的 `std::function<void()>`。自定义实现必须按值保存这个 callback，并在周期触发时调用；不能只保存对 `register_cycle()` 参数的引用。业务输入应在 `config.cycle_callback` 中按值或稳定 owner 捕获，规则与内置实时周期一致。

`config.cycle_manager` 则是一个借用的裸指针：Kairo 不复制也不拥有 `ExternalClock`。周期源对象、它保存的 callback，以及 callback 捕获的业务对象必须按停止顺序逆序释放：先停止实时任务并让 `start_cycle()` 返回，再销毁业务对象和周期源。若周期源自己启动辅助线程，还要在析构前 join，不能让线程继续调用已经销毁的 callback。

## 生命周期与失败

- 注册失败时不要启动周期源；检查 `ExecutorResult` 并清理任何已分配资源。
- `start_cycle()` 应清楚表达启动失败，且不得在失败后留下后台线程。
- `stop_cycle()` 必须能唤醒或终止等待，避免 `stop_realtime_task()` 无限等待。
- callback 必须遵守实时预算：避免无限等待、动态分配和不可控锁；统计应反映周期数、超时和运行状态。

外部周期源接管触发责任，不会消除线程优先级、亲和性、内存锁和平台权限的部署检查；仍需查询实时状态并处理降级。

## 何时不选

普通健康检查、后台刷新和一般实时控制循环应继续使用内置周期。为了“更精确”而引入外部周期管理器会增加停止顺序、异常隔离、线程所有权和测试负担。

## 下一步阅读

[任务如何穿过执行器](/zh/advanced/execution-paths)说明内置实时循环怎样执行 callback 和有界队列工作。
