---
title: GPU
description: 从可选后端诊断开始，再按需使用 GPU 执行器和自动调度。
---

# GPU

GPU 是可选专家路径，不应阻塞首次任务。普通有限工作从 `submit_auto(lambda)` 开始；只有业务已经拥有独立 CPU/GPU 实现、数据规模足以覆盖传输与启动成本，且部署需要可诊断 GPU 后端时才进入本专题。CUDA/OpenCL 运行时不可用或没有设备时，基础 CPU 路径仍可工作。

1. [诊断后端并安全降级](/zh/gpu/diagnostics)：无 GPU 机器也能验证注册失败和 CPU 回退路径。
2. [注册并提交 GPU 工作](/zh/gpu/register-and-submit)：在真实设备上注册、提交、查询状态。
3. [CPU/GPU 自动选择](/zh/gpu/automatic-scheduling)：使用 `cpu_gpu_task()` 表达两条实现、配置回退并理解 legacy overload 的兼容边界。

构建选项、设备查询和现有示例见 [`docs/BUILD.md`](https://github.com/Linductor-alkaid/kairo/blob/master/docs/BUILD.md)、[`examples/gpu_device_query.cpp`](https://github.com/Linductor-alkaid/kairo/blob/master/examples/gpu_device_query.cpp) 与 [`docs/design/gpu_executor.md`](https://github.com/Linductor-alkaid/kairo/blob/master/docs/design/gpu_executor.md)。性能结论必须注明设备、驱动、数据规模和构建类型。
