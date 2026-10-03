# GPU

## Use It For

Optional CUDA/OpenCL execution after a correct CPU path exists. Build and query the required backend before registering a device.

## Minimal Usage

```cpp
executor::gpu::GpuExecutorConfig config;
config.name = "cuda0";
config.backend = executor::gpu::GpuBackend::CUDA;

if (!executor.register_gpu_executor_ex("cuda0", config)) return run_on_cpu();
executor::gpu::GpuTaskConfig task;
auto done = executor.submit_gpu("cuda0", [](void* stream) { launch_kernel(stream); }, task);
done.get();
```

Use `submit_auto(cpu_gpu_task(cpu, gpu))` when CPU and GPU paths are both implemented and the chosen fallback policy permits CPU execution.

## Expert APIs

Use `enumerate_cuda_devices()`, `enumerate_opencl_devices()`, or `enumerate_all_devices()` before assuming `device_id = 0`. `GpuScheduler` exposes standalone heuristic/history decisions; Executor does not automatically import caller-recorded scheduler history. `KernelLaunchOptimizer`, `TaskSchedulerOptimizer`, and `TransferOptimizer` are standalone planning/measurement helpers: they do not submit work, allocate device memory, or synchronize a kernel for the application.

For direct device memory, streams, batch kernel submission, dependency waits, unified memory, or peer access, obtain `IGpuExecutor` only when the application can prove its resource lifetime and serialize it with shutdown. Prefer Facade registration/submission/status for ordinary integration.

## Integration Pitfalls

- Registration does not prove device readiness, kernel completion, or a speedup. Inspect `GpuExecutorStatus` and handle errors at the returned future.
- GPU availability and CPU fallback are explicit policy decisions. Do not assume a selected GPU or unavailable backend silently runs CPU work.
- Bind and retain host/device buffer ownership through future completion. Advanced raw executor pointers are non-owning and cannot race with or outlive `shutdown()`.
- Device, driver, data shape, transfer, and build type determine performance; benchmark the deployed workload.

## Related Guide

`website/en/gpu/`, `docs/BUILD.md`, and `docs/design/gpu_executor.md`.
