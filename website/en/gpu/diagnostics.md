---
title: Diagnose Backend and Fall Back Safely
description: Remain available through diagnosable registration and CPU fallback when CUDA, OpenCL, or a GPU device is absent.
---

# Diagnose Backend and Fall Back Safely

## Goal

On a GPU-free development machine or CI, verify that unavailable GPU backend produces a clear diagnosis while the ordinary CPU path continues to work.

## Build prerequisite

GPU is optional. CUDA requires `KAIRO_ENABLE_GPU=ON` plus `KAIRO_ENABLE_CUDA=ON`; OpenCL requires `KAIRO_ENABLE_GPU=ON` plus `KAIRO_ENABLE_OPENCL=ON`. Each additionally needs headers, runtime, driver, and an accessible device. Runtime dynamic loading does not guarantee availability.

The basic tutorial can explicitly build GPU off:

```bash
cmake -B build -DKAIRO_BUILD_EXAMPLES=ON -DKAIRO_ENABLE_GPU=OFF
cmake --build build
ctest --test-dir build -L tutorial --output-on-failure
```

## Recommended path

Use `register_gpu_executor()` and select business fallback from `ExecutorResult`. The tutorial deliberately chooses unimplemented SYCL, so any machine verifies the diagnostic path:

<<< @/../examples/tutorial/09_gpu.cpp{1-27}

```bash
./build/examples/tutorial/tutorial_09_gpu
```

```text
gpu backend=unavailable, submit=diagnosed, failures=2
```

Inspect failed registration `error_code` and `message`: invalid configuration is `InvalidConfig`; backend not compiled/implemented, unavailable runtime, or absent device is usually `BackendUnavailable`; startup trouble reports `StartFailed`. Calling `submit_gpu()` for an unregistered name throws and records rejection—the observable behavior this tutorial verifies.

## Fallback and hardware boundary

After registration failure, do not keep submitting to that GPU executor name. Continue ordinary CPU work with `submit_auto(lambda)`, or use `cpu_gpu_task()` with an explicit `AllowCpu` fallback after reading [automatic selection](/en/gpu/automatic-scheduling). Users without GPU do not need CUDA/OpenCL to complete the normal tutorials.

GPU-free environments continuously validate headers, `_ex` diagnostics, and rejection. A real CUDA/OpenCL kernel, device memory, stream, and multi-device behavior requires matching hardware, driver, and build settings; it should not become a stable ordinary-PR performance gate.

Next: [register and submit GPU work](/en/gpu/register-and-submit), then [automatic selection](/en/gpu/automatic-scheduling).
