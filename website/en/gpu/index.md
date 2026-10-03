---
title: GPU
description: Start with optional-backend diagnostics, then use GPU executors and automatic scheduling only when needed.
---

# GPU

GPU is an optional expert path and must not block a first task. Begin ordinary finite work with `submit_auto(lambda)`; enter this topic only when an operation has independent CPU/GPU implementations, its data size can cover transfer and launch cost, and deployment needs a diagnosable GPU backend. When CUDA/OpenCL runtime is unavailable or no device exists, the basic CPU path still works.

1. [Diagnose backend and fall back safely](/en/gpu/diagnostics): verify registration failure and CPU fallback even on a machine without GPU.
2. [Register and submit GPU work](/en/gpu/register-and-submit): register, submit, and query status on a real device.
3. [CPU/GPU automatic selection](/en/gpu/automatic-scheduling): express dual paths with `cpu_gpu_task()`, configure fallback, and understand the legacy overload boundary.

Build options, device query, and examples are in [`docs/BUILD.md`](https://github.com/Linductor-alkaid/kairo/blob/master/docs/BUILD.md), [`examples/gpu_device_query.cpp`](https://github.com/Linductor-alkaid/kairo/blob/master/examples/gpu_device_query.cpp), and [`docs/design/gpu_executor.md`](https://github.com/Linductor-alkaid/kairo/blob/master/docs/design/gpu_executor.md). Any performance conclusion records device, driver, data size, and build type.
