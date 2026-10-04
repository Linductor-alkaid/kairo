# 教程示例

本目录中的示例是网站教程的唯一代码事实源。每个程序都使用独立的 `executor::Executor`，输出只包含稳定、短小、可核对的事实；不输出计时、内存地址、线程 ID 或硬件相关数值。

| 示例 | 场景 | 构建要求 | Smoke test |
| --- | --- | --- | --- |
| `01_first_task.cpp` | 返回值与异常 | 基础 | 是 |
| `02_priority.cpp` | 控制命令优先 | 基础 | 是 |
| `03_delayed_periodic.cpp` | 重试与健康检查 | 基础 | 是 |
| `04_batch.cpp` | 批量处理 | 基础 | 是 |
| `05_dependencies.cpp` | 加载、感知与规划 | 基础 | 是 |
| `06_observability.cpp` | failure callback 与状态 | 基础 | 是 |
| `07_realtime.cpp` | 非特权实时线程路径 | 基础 | 是 |
| `08_communication.cpp` | 最新值、消息流与快照 | 基础 | 是 |
| `09_gpu.cpp` | 无后端 GPU 诊断与 CPU 回退 | 基础；不需要 CUDA/OpenCL | 是 |
| `10_service_data_import.cpp` | 服务端数据导入与部分失败 | 基础 | 是 |
| `11_task_inputs.cpp` | 自由函数、成员函数、捕获与输入所有权 | 基础 | 是 |
| `12_blocking_io_worker.cpp` | `start_worker()` 管理可中断阻塞 worker 的唤醒与停止 | 基础 | 是 |
| `13_cancellation_and_timers.cpp` | 协作取消与定时句柄 | 基础 | 是 |
| `14_scheduling_runtime.cpp` | QoS preset、deadline 准入与可注入调度器（0.6.0 Scheduling Runtime） | 基础 | 是 |

构建并运行全部示例：

```bash
cmake -B build -DKAIRO_BUILD_EXAMPLES=ON -DKAIRO_ENABLE_GPU=OFF
cmake --build build --target tutorial_examples
ctest --test-dir build -L tutorial --output-on-failure
```

网页片段优先使用 VitePress `<<< @` 从这些完整文件引用。修改示例时，同时更新其网页页面的预期输出；不要为网页维护单独的代码副本。
