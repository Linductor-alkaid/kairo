---
title: CPU/GPU 自动选择
description: 用 cpu_gpu_task 表达独立路径、配置回退，并理解 GPU 不可用时的显式失败边界。
---

# CPU/GPU 自动选择

## 学习目标

理解 `cpu_gpu_task()` + `submit_auto()` 如何依据任务特征选择 CPU 或 GPU，何时调整 `GpuScheduler::Config`，以及为什么“选择 GPU”不等于自动处理后端不可用。

如果你只有普通 CPU lambda，请返回[执行模型与路由边界](/zh/guides/execution-models-and-routing)：默认 `Auto` 不会隐式把它改投 GPU。

要确认 `.preferred_executor("cuda0")` 如何限制 GPU 候选、未填写名称时为什么只能有一个 GPU，以及 fallback 何时生效，请阅读[自动路由如何匹配目标](/zh/guides/execution-models-and-routing)。

## 推荐路径：独立 CPU/GPU callable

新代码为 CPU 与 GPU 路径提供独立 callable，不再用 `nullptr` stream 在一个 callable 中猜测执行环境：

```cpp
auto future = executor.submit_auto(
    kairo::cpu_gpu_task(
        [data] { run_cpu(*data); },
        [data](void* stream) { run_gpu(stream, *data); })
        .name("segment")
        .data_size(bytes)
        .compute_intensity(3.0F)
        .preferred_executor("cuda0")
        .fallback(kairo::FallbackPolicy::AllowCpu));
future.get();
```

`AllowCpu` 在 GPU 未注册、未运行、错误或达到已知硬容量时允许 CPU 路径，并由 `RoutingDecision::fell_back` 解释；默认 `NoFallback` 则让 future 就绪为异常。`RequireRequestedBackend` 必须显式指定可提交的 GPU executor。实际提交仍可能与 stop 或容量竞争，调用方仍要处理 future 异常和 failure event。

## 默认选择规则

`Executor` 的公开自动路由目前只会使用显式偏好和阈值：Facade 不会在任务完成后向其内部 `GpuScheduler` 写入性能记录，因此即使开启 `enable_adaptive`，也没有可供自动路由使用的历史样本。

`GpuScheduler` 本身在拥有调用方提供的历史时按以下顺序决定：

1. `TaskCharacteristics::prefer_gpu` 为真时选择 GPU。
2. 启用自适应调度且相似任务的 CPU/GPU 历史各至少有两条时，选择预测耗时更短的一侧。
3. 否则，当数据大小达到 `data_size_threshold`（默认 1 MiB）且计算强度达到 `compute_intensity_threshold`（默认 2.0）时选择 GPU。
4. 其余情况选择 CPU。

`CpuGpuTask` 以 `data_size`、`compute_intensity` 和 `prefer_gpu()` 传递任务特征；Facade 的实际选择依赖显式 GPU 偏好和阈值。GPU 必须已注册、运行、无后端错误且未达到已知硬容量，才会进入候选。

## 从 legacy 四参数 overload 迁移（0.6.0 已移除）

0.3.x–0.5.x 曾保留一个四参数 `submit_auto(characteristics, gpu_name, kernel, gpu_config)` overload：单 callable 以空 stream 走 CPU、非空走 GPU。0.6.0 已将其移除——两条路径改由 `cpu_gpu_task()` 的两个独立 callable 表达（见上一节）。旧写法迁移时把"空 stream 分支"移入 CPU callable、"非空分支"移入 GPU callable，并按需拆分各自的输入与生命周期。

## 不会隐式回退的情况

新双路径 `submit_auto()` 会按 `FallbackPolicy` 处理不可用 GPU：仅 `AllowCpu` 会回退 CPU；`NoFallback` 会明确失败，不会偷偷改走 CPU。推荐流程是先完成 `register_gpu_executor()`、检查状态，再允许 GPU 特征或 `prefer_gpu` 进入调度器。

## 调整配置

```cpp
auto config = executor.get_scheduler_config();
config.data_size_threshold = 4 * 1024 * 1024;
config.compute_intensity_threshold = 2.5F;
config.enable_adaptive = true;
config.history_size = 200;
executor.update_scheduler_config(config);
```

阈值应来自真实基准而非直觉。`GpuScheduler::record_performance()` 可用于调用方自行维护的调度器，但当前 `Executor` 没有公开接口将这些样本写入其内部调度器；不要把 `enable_adaptive` 当作 Facade 已自动学习的能力。变更硬件、驱动、数据形状或 kernel 后应重新校准阈值。调度配置是策略，不替代后端可用性检查和任务级异常处理。

## 下一步阅读

需要资源、stream 或多设备控制时进入后续高级专题；完整字段和实现语义以[API 参考](/zh/reference/api)为准。
