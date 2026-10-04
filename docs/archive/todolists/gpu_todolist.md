# GPU 执行器扩展实现任务清单

本文档基于 [GPU 执行器扩展方案设计](../design/gpu_executor.md)，列出 GPU 扩展实现的任务清单。

> **账实核对（2026-09-22）**：阶段 1/2/3 主体实现项均已落地并随 0.2.0–0.5.0 发布；
> 本清单未勾项经逐项核实分为三类——① 已完成未勾账（本轮回填，附证据）；
> ② 门控项（⏸ 需真实 GPU 硬件/工具链，含 SYCL 全部、性能基准与压力测试真机部分）；
> ③ 真实遗留（SYCL 后端、3 个高级示例、专项性能基准文件）。另：§3.5 已勾项中
> "pinned memory" 与 "optimizer" 两处经 2026-09 性能审计证伪，已加勘误注记
> （对应 performance_audit PA-30/PA-31）；性能热点重构由该审计 P5 承接。

---

## 阶段 1：基础框架（MVP）

### 1.1 类型定义与接口

- [x] 补充 `include/executor/types.hpp`
  - [x] 定义 `GpuBackend` 枚举（CUDA、OpenCL、SYCL、HIP）
  - [x] 定义 `GpuDeviceInfo` 结构体
  - [x] 定义 `GpuExecutorStatus` 结构体
  - [x] 定义 `GpuTaskConfig` 结构体
  - [x] 编写类型定义单元测试

- [x] 补充 `include/executor/executor.hpp`
  - [x] 定义 `IGpuExecutor` 接口类
  - [x] 实现模板方法 `submit_kernel`
  - [x] 定义内存管理接口（`allocate_device_memory`、`free_device_memory` 等）
  - [x] 定义流管理接口（`create_stream`、`destroy_stream`、`synchronize_stream` 等）
  - [x] 定义状态查询接口（`get_device_info`、`get_status` 等）

- [x] 补充 `include/executor/config.hpp`
  - [x] 定义 `GpuExecutorConfig` 结构体
  - [x] 添加配置验证函数

### 1.2 CUDA 执行器基础实现

- [x] 创建 `src/executor/gpu/cuda_executor.hpp`
  - [x] 定义 `CudaExecutor` 类（继承 `IGpuExecutor`）
  - [x] 声明私有成员变量（设备ID、流管理、任务队列等）

- [x] 创建 `src/executor/gpu/cuda_executor.cpp`
  - [x] 实现构造函数和析构函数
  - [x] 实现 `start()` 和 `stop()` 方法
  - [x] 实现 `get_name()` 和 `get_device_info()` 方法
  - [x] 实现基础的内存管理（`allocate_device_memory`、`free_device_memory`）
  - [x] 实现基础的内存复制（`copy_to_device`、`copy_to_host`）
  - [x] 实现 `synchronize()` 方法
  - [x] 实现 `submit_kernel_impl()` 方法（基础版本，单流执行）
  - [x] 实现 `get_status()` 方法（基础版本）

- [x] 编写 CUDA 执行器单元测试
  - [x] 测试执行器创建和销毁
  - [x] 测试设备信息查询
  - [x] 测试基础内存管理
  - [x] 测试基础 kernel 提交

### 1.2.1 CUDA 动态加载器实现

- [x] 创建 `src/executor/gpu/cuda_loader.hpp`
  - [x] 定义 `CudaLoader` 类（单例模式）
  - [x] 定义 CUDA 函数指针类型
  - [x] 定义 `CudaFunctionPointers` 结构体
  - [x] 声明 DLL 搜索和加载接口

- [x] 创建 `src/executor/gpu/cuda_loader.cpp`
  - [x] 实现 DLL 搜索逻辑（Windows/Linux）
  - [x] 实现动态 DLL 加载
  - [x] 实现函数指针获取
  - [x] 实现生命周期管理（单例模式）

- [x] 修改 `CudaExecutor` 集成动态加载
  - [x] 集成 `CudaLoader` 到 `CudaExecutor`
  - [x] 将所有 CUDA 函数调用改为通过函数指针调用
  - [x] 实现降级（CUDA 不可用时安全返回）

- [x] 修改 CMake 配置
  - [x] 移除 CUDA 库的静态链接
  - [x] 移除延迟加载配置
  - [x] 保留 CUDA 头文件包含（用于类型定义）

### 1.3 ExecutorManager 扩展

- [x] 扩展 `include/executor/executor_manager.hpp`
  - [x] 添加 `register_gpu_executor()` 方法声明
  - [x] 添加 `get_gpu_executor()` 方法声明
  - [x] 添加 `create_gpu_executor()` 方法声明
  - [x] 添加 `get_gpu_executor_names()` 方法声明
  - [x] 添加 GPU 执行器注册表成员变量

- [x] 扩展 `src/executor/executor_manager.cpp`
  - [x] 实现 `register_gpu_executor()` 方法
  - [x] 实现 `get_gpu_executor()` 方法
  - [x] 实现 `create_gpu_executor()` 方法（工厂方法）
  - [x] 实现 `get_gpu_executor_names()` 方法
  - [x] 在 `shutdown()` 中添加 GPU 执行器关闭逻辑
  - [x] 在析构函数中添加 GPU 执行器清理逻辑

- [x] 编写 ExecutorManager GPU 扩展集成测试
  - [x] 测试 GPU 执行器注册和获取
  - [x] 测试多 GPU 执行器管理
  - [x] 测试生命周期管理（RAII）

### 1.4 Executor Facade 扩展

- [x] 扩展 `include/executor/executor.hpp`
  - [x] 添加 `register_gpu_executor()` 方法声明
  - [x] 添加 `submit_gpu()` 模板方法声明
  - [x] 添加 `get_gpu_executor()` 方法声明
  - [x] 添加 `get_gpu_executor_names()` 方法声明
  - [x] 添加 `get_gpu_executor_status()` 方法声明

- [x] 扩展 `src/executor/executor.cpp`
  - [x] 实现 `register_gpu_executor()` 方法
  - [x] 实现 `submit_gpu()` 模板方法
  - [x] 实现 `get_gpu_executor()` 方法
  - [x] 实现 `get_gpu_executor_names()` 方法
  - [x] 实现 `get_gpu_executor_status()` 方法

- [x] 编写 Executor Facade GPU 扩展集成测试
  - [x] 测试 GPU 执行器注册
  - [x] 测试 GPU 任务提交
  - [x] 测试状态查询

### 1.5 CMake 构建系统扩展

- [x] 扩展 `CMakeLists.txt`
  - [x] 添加 `EXECUTOR_ENABLE_GPU` 选项
  - [x] 添加 `EXECUTOR_ENABLE_CUDA` 选项
  - [x] 添加 CUDA 库查找逻辑（`find_package(CUDAToolkit)` 和 `find_package(CUDA)`）
  - [x] 添加条件编译逻辑（仅在启用 GPU 时编译 GPU 相关代码）
  - [x] 添加 CUDA 头文件包含（用于类型定义，不链接库）

- [x] 创建 `cmake/FindCUDA.cmake`（如果需要）
  - [x] 实现 CUDA 库查找逻辑
    （无需自建模块：CMakeLists.txt 直接用内置 find_package(CUDAToolkit)/find_package(CUDA)
    并做 CMake 4.x 兼容兜底，"如果需要"条件未成立，2026-09-22 回填）
  - [x] 设置 CUDA 包含目录和库目录
    （CUDA_INCLUDE_DIRS 三候选路径探测 + INTERNAL include 兜底，CMakeLists.txt:85-107，
    2026-09-22 回填）

- [x] 测试构建系统
  - [x] 测试禁用 GPU 时的构建（不应包含 GPU 代码）
  - [x] 测试启用 GPU 时的构建（应包含 GPU 代码）
  - [x] 测试动态加载（不依赖静态链接）

### 1.6 基础示例

- [x] 创建 `examples/gpu_basic.cpp`
  - [x] 实现基本的 GPU 执行器注册
  - [x] 实现基本的 GPU 任务提交
  - [x] 实现基本的 GPU 内存管理
  - [x] 添加注释说明

- [x] 更新 `examples/CMakeLists.txt`
  - [x] 添加 `gpu_basic` 示例的构建规则
  - [x] 添加条件编译（仅在启用 GPU 时构建）

---

## 阶段 2：功能完善

### 2.1 流管理（Multi-Stream）

- [x] 扩展 `src/executor/gpu/cuda_executor.cpp`
  - [x] 实现 `create_stream()` 方法
  - [x] 实现 `destroy_stream()` 方法
  - [x] 实现 `synchronize_stream()` 方法
  - [x] 扩展 `submit_kernel_impl()` 支持指定流
  - [x] 实现流池管理（预创建多个流）

- [x] 编写流管理单元测试
  - [x] 测试流的创建和销毁
  - [x] 测试多流并行执行
  - [x] 测试流同步

### 2.2 异步内存传输

- [x] 扩展 `src/executor/gpu/cuda_executor.cpp`
  - [x] 实现异步 `copy_to_device()`（使用 CUDA stream）
  - [x] 实现异步 `copy_to_host()`（使用 CUDA stream）
  - [x] 实现异步 `copy_device_to_device()`（使用 CUDA stream）
  - [x] 添加内存传输完成回调支持

- [x] 编写异步内存传输单元测试
  - [x] 测试异步传输与计算重叠
  - [x] 测试传输完成回调

### 2.3 多 GPU 设备支持

- [x] 扩展 `src/executor/gpu/cuda_executor.cpp`
  - [x] 实现设备选择逻辑（`cudaSetDevice`）
  - [x] 实现设备间内存复制（P2P）
  - [x] 实现设备间通信优化

- [x] 创建 `examples/gpu_multi_device.cpp`
  - [x] 实现多 GPU 设备注册
  - [x] 实现多 GPU 并行任务提交
  - [x] 实现设备间数据同步

- [x] 编写多 GPU 设备集成测试
  - [x] 测试多设备管理
  - [x] 测试设备间负载均衡

### 2.4 内存池优化

- [x] 创建 `src/executor/gpu/gpu_memory_manager.hpp`
  - [x] 定义 `GpuMemoryManager` 类
  - [x] 定义内存块结构
  - [x] 定义内存池管理接口

- [x] 创建 `src/executor/gpu/gpu_memory_manager.cpp`
  - [x] 实现内存池初始化
  - [x] 实现内存分配（从池中分配）
  - [x] 实现内存释放（回收到池中）
  - [x] 实现内存碎片整理
  - [x] 实现内存使用统计

- [x] 集成内存池到 CUDA 执行器
  - [x] 在 `CudaExecutor` 中使用 `GpuMemoryManager`
  - [x] 添加配置选项（启用/禁用内存池）
  - [x] 添加内存池大小配置

- [x] 编写内存池单元测试
  - [x] 测试内存分配和释放
  - [x] 测试内存碎片整理
  - [x] 测试内存使用统计
  - [x] 性能测试（对比直接分配 vs 内存池）

### 2.5 监控和统计

- [x] 扩展 `src/executor/gpu/cuda_executor.cpp`
  - [x] 实现详细的统计信息收集
    - [x] kernel 执行时间统计
    - [x] 内存使用统计
    - [x] 任务队列大小统计
    - [x] 失败任务统计
  - [x] 实现 `get_status()` 方法（完整版本）
  - [x] 添加性能计数器

- [x] 集成到监控模块
  - [x] 在 `StatisticsCollector` 中添加 GPU 统计支持
  - [x] 在 `Executor` Facade 中添加 GPU 监控查询

- [x] 编写监控功能测试
  - [x] 测试统计信息收集
  - [x] 测试监控查询 API

### 2.6 异常处理

- [x] 扩展 `src/executor/gpu/cuda_executor.cpp`
  - [x] 实现 CUDA 错误检查（`cudaGetLastError`、`cudaDeviceSynchronize`）
  - [x] 实现错误码转换（CUDA 错误码 → 标准异常）
  - [x] 实现异常捕获和记录
  - [x] 集成到 `ExceptionHandler`

- [x] 编写异常处理测试
  - [x] 测试 CUDA 错误处理
  - [x] 测试异常传播
  - [x] 测试错误日志记录

### 2.7 任务队列优化

- [x] 扩展 `src/executor/gpu/cuda_executor.cpp`
  - [x] 实现优先级任务队列
  - [x] 实现批量任务提交
  - [x] 实现任务依赖管理（GPU 任务间依赖）

- [x] 编写任务队列测试
  - [x] 测试优先级调度
  - [x] 测试批量提交性能
  - [x] 测试任务依赖

---

## 阶段 3：高级特性

### 3.1 OpenCL 执行器实现

- [x] 创建 `src/executor/gpu/opencl_loader.hpp`
  - [x] 定义 `OpenCLLoader` 类（单例模式）
  - [x] 定义 OpenCL 函数指针类型
  - [x] 声明动态加载接口

- [x] 创建 `src/executor/gpu/opencl_loader.cpp`
  - [x] 实现 DLL 搜索逻辑（Windows/Linux）
  - [x] 实现动态库加载
  - [x] 实现函数指针获取

- [x] 创建 `src/executor/gpu/opencl_executor.hpp`
  - [x] 定义 `OpenCLExecutor` 类（继承 `IGpuExecutor`）
  - [x] 声明 OpenCL 相关成员变量

- [x] 创建 `src/executor/gpu/opencl_executor.cpp`
  - [x] 实现 OpenCL 平台和设备选择
  - [x] 实现 OpenCL 上下文创建
  - [x] 实现 OpenCL 命令队列管理
  - [x] 实现 `submit_kernel_impl()`（使用 OpenCL）
  - [x] 实现内存管理（OpenCL buffer）
  - [x] 实现流管理（OpenCL command queue）
  - [x] 实现同步操作

- [x] 扩展 CMake 构建系统
  - [x] 添加 `EXECUTOR_ENABLE_OPENCL` 选项
  - [x] 添加 OpenCL 库查找逻辑
  - [x] 添加条件编译逻辑

- [x] 编写 OpenCL 执行器单元测试
  - [x] 测试执行器创建和销毁
  - [x] 测试 kernel 提交
  - [x] 测试内存管理

- [x] 创建 `examples/gpu_opencl.cpp`
  - [x] 实现 OpenCL 执行器使用示例

- [x] 创建 OpenCL 测试环境搭建文档
  - [x] Linux 环境配置说明
  - [x] Windows 环境配置说明
  - [x] 常见问题排查

### 3.2 SYCL 执行器实现

> 2026-09-22 定性：真实遗留项，且 ⏸ 门控（需 Intel oneAPI/DPC++ 工具链与 SYCL 硬件）。
> 现状：types.hpp 仅有 `SYCL` 枚举占位，executor.cpp 显式抛出
> "SYCL backend is not implemented in this build"；除非正式立项，本节保持未勾。

- [ ] 创建 `src/executor/gpu/sycl_executor.hpp`
  - [ ] 定义 `SyclExecutor` 类（继承 `IGpuExecutor`）
  - [ ] 声明 SYCL 相关成员变量

- [ ] 创建 `src/executor/gpu/sycl_executor.cpp`
  - [ ] 实现 SYCL 设备选择
  - [ ] 实现 SYCL 队列管理
  - [ ] 实现 `submit_kernel_impl()`（使用 SYCL）
  - [ ] 实现内存管理（SYCL buffer）
  - [ ] 实现同步操作

- [ ] 扩展 CMake 构建系统
  - [ ] 添加 `EXECUTOR_ENABLE_SYCL` 选项
  - [ ] 添加 SYCL 库查找逻辑（Intel oneAPI 或 DPC++）
  - [ ] 添加条件编译逻辑

- [ ] 编写 SYCL 执行器单元测试
- [ ] 创建 `examples/gpu_sycl.cpp`

### 3.3 统一内存支持（Unified Memory）

- [x] 扩展 `src/executor/gpu/cuda_executor.cpp`
  - [x] 实现统一内存分配（`cudaMallocManaged`）
  - [x] 实现统一内存管理接口
  - [x] 实现内存预取（`cudaMemPrefetchAsync`）
  - [x] 添加统一内存配置选项

- [x] 编写统一内存测试
  - [x] 测试统一内存分配
  - [x] 测试内存预取
  - [x] 性能对比测试（统一内存 vs 显式传输）

### 3.4 智能调度（CPU/GPU 自动选择）

- [x] 创建 `include/executor/gpu/gpu_scheduler.hpp`
  - [x] 定义 `GpuScheduler` 类
  - [x] 定义任务特征分析接口（`TaskCharacteristics`）

- [x] 创建 `src/executor/gpu/gpu_scheduler.cpp`
  - [x] 实现任务特征分析（数据量、计算复杂度等）
  - [x] 实现 CPU/GPU 选择策略（基于阈值的启发式决策）
  - [x] 实现性能预测模型
  - [x] 实现自适应调度（基于历史性能数据）

- [x] 扩展 `Executor` Facade
  - [x] 添加 `submit_auto()` 方法（自动选择执行器）
  - [x] 添加调度策略配置（`update_scheduler_config` / `get_scheduler_config`）

- [x] 编写智能调度测试
  - [x] 测试任务特征分析
  - [x] 测试 CPU/GPU 选择
  - [x] 测试性能预测准确性

### 3.5 性能优化

- [x] Kernel 启动优化
  - [x] 实现 kernel 参数缓存
  - [x] 实现 kernel 启动批量化
  - [x] 优化 kernel 启动延迟

- [x] 内存传输优化
  - [x] 实现传输批量化
  - [x] 实现传输与计算流水线
  - [x] 优化小数据传输（使用 pinned memory）
    （⚠ 勘误 2026-09-22：2026-09 性能审计证实全库尚无 pinned host memory，
    cuda_loader.cpp 仅 pageable 拷贝——本项承诺未兑现，已登记 performance_audit
    PA-31 待修）

- [x] 任务调度优化
  - [x] 实现任务优先级调度
  - [x] 实现任务依赖图优化
  - [x] 实现负载均衡（多 GPU）

- [x] 性能测试和基准测试
  - [x] 创建性能测试套件
  - [x] 对比不同优化策略的性能
  - [x] 编写性能测试报告
    （⚠ 勘误 2026-09-22：报告对象为三个 optimizer（docs/performance/
    gpu_performance_optimization_report.md），而审计 PA-30 证实 optimizer 未接入
    执行器路径、优化收益为零——去留待用户反馈，见 performance_audit P5）

### 3.6 高级示例

> 2026-09-22 定性：演示性内容，价值存疑——除非有用户诉求，建议整体放弃或降级。
> 现有 GPU 示例已覆盖 gpu_basic / gpu_device_query / gpu_multi_device / gpu_opencl /
> gpu_unified_memory；CPU-GPU 混合目标已由 submit_auto + cpu_gpu_task 与
> website automatic-scheduling 指南覆盖。

- [ ] 创建 `examples/gpu_matrix_multiply.cpp`
  - [ ] 实现矩阵乘法 GPU kernel
  - [ ] 展示内存管理和数据传输
  - [ ] 展示性能对比（CPU vs GPU）

- [ ] 创建 `examples/gpu_image_processing.cpp`
  - [ ] 实现图像处理 GPU kernel
  - [ ] 展示多流并行处理
  - [ ] 展示异步内存传输

- [ ] 创建 `examples/gpu_deep_learning.cpp`
  - [ ] 实现简单的深度学习推理
  - [ ] 展示 Tensor Core 使用（如果支持）
  - [ ] 展示批处理推理

- [x] 创建 `examples/gpu_hybrid_compute.cpp`
  - [x] 展示 CPU-GPU 混合计算
    （被取代 2026-09-22：由 submit_auto(cpu_gpu_task(...)) + examples/tutorial/09_gpu.cpp
    + website automatic-scheduling 指南覆盖，不再单列示例）
  - [x] 展示数据流水线处理
    （同上被取代）
  - [x] 展示负载均衡
    （同上被取代）

---

## 阶段 4：测试与文档完善

### 4.1 单元测试完善

> 2026-09-22 回填：本节全部落地，专项测试文件均存在并注册 CTest。

- [x] GPU 类型定义测试
  - [x] 测试所有类型定义的正确性
    （test_gpu_executor_direct_config_validation.cpp 覆盖类型/配置校验）
  - [x] 测试类型转换和序列化
    （GpuBackend→字符串序列化见 executor_snapshot_formatter.cpp:54，
    test_executor_snapshot / test_api_doc_status_fields.cpp 断言）

- [x] CUDA 执行器完整测试
  - [x] 测试所有接口方法
    （test_cuda_executor.cpp 18 个用例：创建/设备信息/内存/拷贝/kernel/同步/流等）
  - [x] 测试边界条件
  - [x] 测试错误处理
    （test_cuda_status_records_last_error.cpp）
  - [x] 测试并发安全性
    （test_cuda_executor_concurrent_stop.cpp、test_cuda_stream_destroy_race.cpp、
    test_gpu_wait_for_completion_race.cpp）

- [x] 内存管理器测试
  - [x] 测试各种内存分配场景
    （test_gpu_memory_manager.cpp 多场景分配/池复用/大块溢出回退）
  - [x] 测试内存碎片处理
    （test_gpu_defragment.cpp）
  - [x] 测试内存泄漏检测
    （ASAN/LSAN（Sanitizers.cmake）+ CI 覆盖；越界校验 test_gpu_memory_validation.cpp）

- [x] 流管理测试
  - [x] 测试多流并发
  - [x] 测试流同步
    （test_cuda_executor.cpp 流管理用例 + test_cuda_stream_callback.cpp）
  - [x] 测试流资源管理
    （含 destroy 后同步不崩溃、流耗尽路径）

### 4.2 集成测试

> 2026-09-22 回填：前两组全部落地；混合执行调度层已覆盖，真机并行部分门控。

- [x] GPU 执行器与 ExecutorManager 集成测试
  - [x] 测试多 GPU 执行器管理
    （test_executor_manager_gpu.cpp）
  - [x] 测试生命周期管理
    （含 test_gpu_loader_failure.cpp 降级路径）
  - [x] 测试资源清理

- [x] GPU 执行器与 Executor Facade 集成测试
  - [x] 测试 GPU 任务提交流程
    （test_executor_facade.cpp GPU 注册/提交/查询/错误处理）
  - [x] 测试状态查询
  - [x] 测试错误处理
    （test_submit_gpu_missing_executor_records_failure.cpp）

- [x] CPU-GPU 混合执行集成测试
  - [x] 测试 CPU 和 GPU 任务并行执行
    （调度层：test_gpu_scheduler.cpp 12 用例 + test_executor_auto_routing_stage1.cpp；
    真机并行 ⏸ 门控于 GPU 硬件）
  - [x] 测试数据共享和同步
    （stub 层；真机数据共享 ⏸ 门控于 GPU 硬件）
  - [x] 测试资源竞争处理
    （stub 层；真机资源竞争 ⏸ 门控于 GPU 硬件）

### 4.3 性能测试

> 2026-09-22 定性：系统性专项基准文件确实不存在（真实遗留）；需要真 GPU 的
> 对比/压力项 ⏸ 门控。已有替代物：test_gpu_perf_optimizer.cpp、
> docs/optimization/gpu_queue_benchmark.json、gpu_performance_optimization_report.md。
> 混合负载基准统一并入 performance_audit P5 验收口径，不再双台账记账。

- [ ] 创建 `tests/test_gpu_performance.cpp`
  - [ ] 实现 GPU 任务提交延迟测试
  - [ ] 实现内存传输性能测试
  - [ ] 实现 kernel 执行性能测试
  - [ ] 实现多流并行性能测试
  - [ ] 实现多 GPU 并行性能测试

- [ ] ⏸ 门控（GPU 硬件）：性能基准测试
  - [ ] 对比不同 GPU 后端的性能
  - [ ] 对比不同配置的性能
  - [ ] 对比 CPU vs GPU 性能

- [ ] ⏸ 门控（GPU 硬件）：压力测试
  - [ ] 高并发任务提交测试
  - [ ] 大内存分配测试
  - [ ] 长时间运行稳定性测试

### 4.4 文档完善

> 2026-09-22 回填：本节全部落地（API.md §8、README/BUILD、website 中英 GPU 指南、
> gpu_executor.md 设计文档、CHANGELOG 0.2.0–0.5.0 记录）。

- [x] 更新 API 文档
  - [x] 添加 GPU 执行器 API 说明
    （docs/API.md §8 "GPU 执行器 API"）
  - [x] 添加 GPU 配置说明
  - [x] 添加 GPU 使用示例

- [x] 更新 README.md
  - [x] 添加 GPU 支持说明
  - [x] 添加 GPU 构建说明
    （docs/BUILD.md 三个开关默认值与启用命令）
  - [x] 添加 GPU 依赖说明

- [x] 创建 GPU 使用指南
  - [x] 编写 GPU 执行器使用教程
    （website/zh/gpu/ 与 website/en/gpu/ 各 4 页，中英双语）
  - [x] 编写 GPU 内存管理最佳实践
  - [x] 编写性能优化指南
  - [x] 编写故障排查指南
    （含 docs/setup/opencl_setup.md 环境配置与排查）

- [x] 更新设计文档
  - [x] 更新架构图（包含 GPU 执行器）
    （docs/design/gpu_executor.md，约 1100 行含架构图）
  - [x] 更新系统架构说明

- [x] 更新 CHANGELOG.md
  - [x] 记录 GPU 执行器功能添加
    （0.2.0 GPU 执行器/OpenCL/统一内存）
  - [x] 记录 API 变更
  - [x] 记录性能改进

### 4.5 代码审查和优化

> 2026-09-22 定性：代码审查已发生（2026-09 四路逐行审查，GPU 路径问题登记为
> performance_audit PA-25~34，修复由其 P5 承接）；其余结构性重构项为真实遗留。

- [x] 代码审查
  - [x] 审查 GPU 执行器实现代码
    （2026-09 性能审查覆盖 cuda_executor.cpp/opencl_executor.cpp，产出 PA-25~28/36）
  - [x] 审查内存管理代码
    （产出 PA-31/34）
  - [x] 审查异常处理代码
  - [x] 审查线程安全性
    （产出 PA-25/26 锁窗口结论）

- [ ] 代码重构
  - [ ] 提取公共代码
  - [ ] 优化代码结构
  - [ ] 改进错误处理
  - [x] 改进性能热点
    （被取代 2026-09-22：由 performance_audit P5（PA-25~28/31~34）以更具体方案承接，
    本清单不再单独推进）

- [ ] 静态分析
  - [ ] 使用静态分析工具检查代码
    （sanitizer 路径已落地：Sanitizers.cmake ASAN/UBSAN/TSAN + CI 专用 TSAN job；
    clang-tidy/cppcheck 类工具未配置）
  - [ ] 修复发现的问题
    （性能类发现已登记 PA 台账；其余无工具产出可修）
  - [x] 检查内存泄漏
    （ASAN/LSAN + test_gpu_memory_validation.cpp + P-007 内存校验回归，2026-09-22 回填）

---

## 阶段 5：发布准备

### 5.1 构建系统完善

- [x] 完善 CMake 配置
  - [x] 测试所有构建选项组合
    （CI 矩阵多 os/compiler/build_type 验证，2026-09-22 回填）
  - [x] 测试跨平台构建（Linux、Windows）
    （含 release.yml runner 无 CUDA 自动降级，2026-09-22 回填）
  - [ ] ⏸ 门控（GPU 硬件）：测试不同 GPU 后端组合
  - [ ] 优化构建时间
    （无相关证据/记录，真实遗留）

- [x] 安装规则
  - [x] 添加 GPU 相关头文件安装规则
    （src/CMakeLists.txt install(DIRECTORY executor/gpu/)，2026-09-22 回填）
  - [x] 添加 GPU 相关库文件安装规则
    （install(TARGETS executor) + 顶层 include 安装，2026-09-22 回填）
  - [x] 测试安装流程
    （release.yml deb/tgz 打包实测 + executorConfigVersion.cmake 支持 find_package，
    2026-09-22 回填）

- [x] 打包脚本
  - [x] 更新打包脚本支持 GPU 选项
    （package_deb.sh 在 nvidia/cuda:12.4.1-devel 容器内完整构建，无 CUDA 运行时自动降级，
    2026-09-22 回填）
  - [x] 测试打包流程
    （PACKAGE_DEB/LINUX/WINDOWS/ANDROID 文档齐备，2026-09-22 回填）

### 5.2 版本管理

> 2026-09-22 回填：本节全部落地（GPU 功能自 0.2.0 起随版本发布，已远超计划预期的
> "如 v0.2.0"）。

- [x] 版本号更新
  - [x] 确定版本号（如 v0.2.0）
  - [x] 更新版本号定义
    （CMakeLists.txt VERSION 0.5.0）

- [x] 发布说明
  - [x] 编写 GPU 执行器功能说明
    （CHANGELOG 0.2.0–0.5.0 各版本 GPU 小节）
  - [x] 编写迁移指南（如有 API 变更）
    （docs/MIGRATION.md）
  - [x] 编写已知问题说明
    （如 CHANGELOG "P2P 拷贝为实验性，未在多 GPU 实机充分测试"）

### 5.3 持续集成

- [x] CI/CD 配置
  - [x] 添加 GPU 测试到 CI 流程
    （gpu-headers-build：伪造 CUDA stub 头编译完整 GPU 路径并运行
    test_cuda_executor/test_opencl_executor/test_executor_manager_gpu 等，2026-09-22 回填）
  - [ ] ⏸ 门控（CI 无 GPU 硬件）：配置 GPU 测试环境（如果 CI 支持）
    （"如果 CI 支持"条件未满足：GitHub runner 无 GPU；真机测试维持门控）
  - [x] 添加 GPU 构建测试
    （同 gpu-headers-build 任务，覆盖 GPU/CUDA/OpenCL 全 ON 组合，2026-09-22 回填）

---

## 注意事项

### 开发环境要求

- **CUDA 开发**：
  - NVIDIA GPU 硬件
  - CUDA Toolkit（版本 >= 11.0 推荐）
  - 支持 CUDA 的编译器（nvcc 或支持 CUDA 的 gcc/clang）

- **OpenCL 开发**：
  - OpenCL SDK（Intel/AMD/NVIDIA）
  - OpenCL 运行时库

- **SYCL 开发**：
  - Intel oneAPI 或 DPC++ 编译器
  - SYCL 运行时库

### 测试环境要求

- 至少一个 GPU 设备用于测试
- 多 GPU 测试需要多个 GPU 设备
- 不同 GPU 后端的测试需要相应的硬件和驱动

### 性能目标

- GPU 任务提交延迟：< 10μs
- 内存传输带宽：接近硬件峰值
- Kernel 启动延迟：< 100μs
- 监控开销：< 5%（相对于任务执行时间）

### 兼容性

- 保持与现有 CPU 执行器的完全兼容
- GPU 支持为可选功能，不影响现有功能
- 支持渐进式迁移（可以逐步启用 GPU 功能）

---

## 参考文档

- [GPU 执行器扩展方案设计](../design/gpu_executor.md)
- [Executor 工具项目架构设计](../design/executor.md)
- [C++ 项目设计方案](../design/cpp-project-design.md)
