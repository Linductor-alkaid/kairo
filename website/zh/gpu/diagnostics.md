---
title: 诊断后端并安全降级
description: 在没有 CUDA、OpenCL 或 GPU 设备时，通过可诊断注册和 CPU 回退保持程序可用。
---

# 诊断后端并安全降级

## 学习目标

在无 GPU 的开发机和 CI 中验证：GPU 后端不可用时，程序得到明确诊断，而普通 CPU 路径继续工作。

## 构建前置

GPU 模块是可选的。CUDA 需要 `KAIRO_ENABLE_GPU=ON` 与 `KAIRO_ENABLE_CUDA=ON`，OpenCL 需要 `KAIRO_ENABLE_GPU=ON` 与 `KAIRO_ENABLE_OPENCL=ON`；还需要相应头文件、运行时、驱动和可访问的设备。运行时动态加载不保证后端一定可用。

基础教程可显式关闭 GPU 后构建：

```bash
cmake -B build -DKAIRO_BUILD_EXAMPLES=ON -DKAIRO_ENABLE_GPU=OFF
cmake --build build
ctest --test-dir build -L tutorial --output-on-failure
```

## 推荐方案

使用 `register_gpu_executor_ex()`，根据 `ExecutorResult` 作出业务回退。教程示例故意选择未实现的 SYCL 后端，因此在任何机器上都会验证诊断路径：

<<< @/../examples/tutorial/09_gpu.cpp{1-27}

完整源码：[`examples/tutorial/09_gpu.cpp`](https://github.com/Linductor-alkaid/kairo/blob/master/examples/tutorial/09_gpu.cpp)。

```bash
./build/examples/tutorial/tutorial_09_gpu
```

## 预期输出

```text
gpu backend=unavailable, submit=diagnosed, failures=2
```

失败的注册应检查 `error_code` 与 `message`：配置错误是 `InvalidConfig`，未编译/未实现/运行时不可用通常是 `BackendUnavailable`，启动时问题会给出 `StartFailed`。如果仍调用未注册名称的 `submit_gpu()`，它会抛出异常并记录拒绝；这正是示例验证的可观察行为。

## 降级策略

注册失败后，不要继续向同名 GPU executor 提交任务。普通 CPU 工作继续使用 `submit_auto(lambda)`；需要双路径回退时，阅读[CPU/GPU 自动选择](/zh/gpu/automatic-scheduling)后使用带 `AllowCpu` 的 `cpu_gpu_task()`。无 GPU 的用户不需要安装 CUDA/OpenCL，也不应因此无法完成普通教程。

## 硬件验证边界

无硬件环境可以持续验证头文件、`_ex` 诊断与拒绝路径；真实 CUDA/OpenCL kernel、设备内存、stream 和多设备表现必须在对应硬件、驱动与构建选项下验证。它们不应成为普通 PR 的稳定性能门禁。

## 下一步阅读

有真实设备时进入[注册并提交 GPU 工作](/zh/gpu/register-and-submit)；需按工作负载选择 CPU/GPU 时阅读[自动调度](/zh/gpu/automatic-scheduling)。
