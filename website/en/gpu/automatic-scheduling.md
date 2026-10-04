---
title: CPU/GPU Automatic Selection
description: Use cpu_gpu_task to express independent paths, configure fallback, and understand the legacy submit_auto boundary.
---

# CPU/GPU Automatic Selection

## Goal

Understand how `cpu_gpu_task()` plus `submit_auto()` chooses a CPU or GPU path from task characteristics, when to adjust `GpuScheduler::Config`, and why choosing GPU does not automatically handle an unavailable backend.

If you have only an ordinary CPU lambda, return to [Execution Models and Routing Boundaries](/en/guides/execution-models-and-routing): default `Auto` does not implicitly move it to GPU.

For how `.preferred_executor("cuda0")` constrains GPU candidates, why an unnamed task needs exactly one GPU, and when fallback applies, read [how automatic routing matches a target](/en/guides/execution-models-and-routing).

## Recommended path: independent CPU and GPU callables

New code gives CPU and GPU separate callables instead of asking one callable to infer its environment from a null stream:

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

`AllowCpu` permits CPU execution when GPU is unregistered, stopped, in error, or at known hard capacity, and records `RoutingDecision::fell_back`. Default `NoFallback` makes the future ready with an exception; `RequireRequestedBackend` requires a named submit-capable GPU executor. Submission still competes with stop and capacity changes, so handle future exceptions and failure events.

## Default selection rules

The public `Executor` routing path currently uses only explicit preference and thresholds: the Facade does not record completion timings into its internal `GpuScheduler`, so enabling `enable_adaptive` supplies no history for automatic routing.

`GpuScheduler` itself decides in this order when it owns caller-supplied history:

1. Choose GPU when `TaskCharacteristics::prefer_gpu` is true.
2. With adaptive scheduling enabled and at least two CPU and two GPU history samples for similar work, choose the side with lower predicted time.
3. Otherwise choose GPU when data size meets `data_size_threshold` (default 1 MiB) and compute intensity meets `compute_intensity_threshold` (default 2.0).
4. Choose CPU otherwise.

`CpuGpuTask` supplies `data_size`, `compute_intensity`, and `prefer_gpu()` task characteristics. The Facade's actual choice uses explicit GPU preference and thresholds. A GPU enters the candidate set only when it is registered, running, error-free, and below known hard capacity.

## Compatibility path: legacy four-argument overload

`0.3.x` retains this overload for existing code. Its CPU branch invokes one callable with a null stream, and an unready GPU does not implicitly fall back to CPU:

```cpp
auto data = std::make_shared<WorkData>(prepare_work());
auto future = executor.submit_auto(work, "cuda0",
    [data](void* stream) {
        if (stream == nullptr) {
            run_cpu(*data);
        } else {
            run_gpu(stream, *data);
        }
    }, gpu_task_config);
```

Use it only for incremental migration. When the paths need different inputs or lifetimes, use `cpu_gpu_task()` above. This overload receives no compile-time deprecation marker before the next breaking major version.

## No implicit fallback

The new dual-path `submit_auto()` follows `FallbackPolicy`: only `AllowCpu` falls back; `NoFallback` and the legacy overload fail explicitly. First call `register_gpu_executor()` and inspect status before admitting GPU characteristics or `prefer_gpu`.

## Tune configuration from measurement

```cpp
auto config = executor.get_scheduler_config();
config.data_size_threshold = 4 * 1024 * 1024;
config.compute_intensity_threshold = 2.5F;
config.enable_adaptive = true;
config.history_size = 200;
executor.update_scheduler_config(config);
```

Thresholds come from real benchmarks, not intuition. `GpuScheduler::record_performance()` can support a scheduler maintained by the caller, but `Executor` currently exposes no way to write those samples into its internal scheduler; do not treat `enable_adaptive` as Facade learning. Recalibrate thresholds after changing hardware, driver, data shape, or kernel. Scheduling policy does not replace backend availability checks or task-level exception handling.

For stream, resource, or multi-device control, use the advanced interfaces deliberately; complete fields and semantics remain in the [API reference](https://github.com/Linductor-alkaid/kairo/blob/master/docs/API.md).
