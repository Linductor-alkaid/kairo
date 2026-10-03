---
title: Register and Submit GPU Work
description: Register a GPU executor, submit work, and query runtime status on a CUDA or OpenCL device.
---

# Register and Submit GPU Work

## Goal

In a genuinely available CUDA or OpenCL environment, register a GPU executor through the Facade, submit kernel work, and query status. Enter advanced APIs only when resource management is required.

## Build and inspect devices

Choose one backend:

```bash
# CUDA (NVIDIA)
cmake -B build -DKAIRO_BUILD_EXAMPLES=ON -DKAIRO_ENABLE_GPU=ON -DKAIRO_ENABLE_CUDA=ON

# OpenCL (Intel / AMD / NVIDIA)
cmake -B build -DKAIRO_BUILD_EXAMPLES=ON -DKAIRO_ENABLE_GPU=ON -DKAIRO_ENABLE_OPENCL=ON
cmake --build build
./build/examples/gpu_device_query
```

If the query is empty, stop at diagnostics; never assume `device_id = 0` exists.

## Register, submit, observe

```cpp
kairo::gpu::GpuExecutorConfig config;
config.name = "cuda0";
config.backend = kairo::gpu::GpuBackend::CUDA;
config.device_id = 0;

const auto registered = executor.register_gpu_executor_ex("cuda0", config);
if (!registered) {
    return run_on_cpu();
}

kairo::gpu::GpuTaskConfig task;
auto completed = executor.submit_gpu("cuda0", [](void* stream) {
    launch_kernel(stream);
}, task);
completed.get();

const auto status = executor.get_gpu_executor_status("cuda0");
```

`submit_gpu()` returns `future<void>`; `get()` remains the exception boundary for one kernel submission. Use executor names, individual/all `GpuExecutorStatus` to observe registration, queue, active/completed/failed kernels, memory use, and `last_error_message`.

## Bind callable inputs and honor lifetime

`submit_gpu()` does not take ordinary `fn, args...`. It receives a callable with already-bound business input, either `void()` or `void(void* stream)`:

```cpp
auto buffers = std::make_shared<DeviceBuffers>(prepare_buffers());
auto completed = executor.submit_gpu("cuda0", [buffers](void* stream) {
    launch_kernel(stream, buffers->input(), buffers->output());
}, task);
completed.get();
```

The `void*` form accesses a backend stream; the no-argument form needs no explicit stream. Capture scalars, device pointers, and buffer owner into the callable and keep them valid through future completion. `shared_ptr` preserves a host owner, not automatic GPU allocation/synchronization/free. Do not reuse or free host/device buffers a kernel may access until the future and backend semantics establish completion; raw device pointers require a provable allocation owner.

## Configuration and performance boundary

`GpuExecutorConfig` requires a nonempty name, valid backend, nonnegative device ID, positive queue capacity and stream count. `GpuTaskConfig` carries grid/block, shared memory, stream, async, and priority. A nondefault stream must come from this executor's `create_stream()` and remain undestroyed.

Stay with the Facade for registration/submission/status. For device memory, stream lifetime, unified memory, or P2P, use `get_gpu_executor()` only with its added resource-lifecycle responsibility. It returns a non-owning pointer and cannot be retained across or used concurrently with `shutdown()`.

Registered does not mean faster. Any conclusion records model, driver, backend, data size, kernel, build type, measurement, and CPU comparison. See [`gpu_basic.cpp`](https://github.com/Linductor-alkaid/kairo/blob/master/examples/gpu_basic.cpp), [`gpu_multi_device.cpp`](https://github.com/Linductor-alkaid/kairo/blob/master/examples/gpu_multi_device.cpp), and [`gpu_opencl.cpp`](https://github.com/Linductor-alkaid/kairo/blob/master/examples/gpu_opencl.cpp).

Next: [CPU/GPU automatic selection](/en/gpu/automatic-scheduling).
