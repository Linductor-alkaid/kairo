# Kairo

<p align="center">
  <img src="docs/kairo.png" width="160" alt="Kairo 标志">
</p>

[![CI](https://github.com/Linductor-alkaid/kairo/actions/workflows/c-cpp.yml/badge.svg)](https://github.com/Linductor-alkaid/kairo/actions/workflows/c-cpp.yml) [![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus)](https://isocpp.org/) [![CMake](https://img.shields.io/badge/CMake-3.16%2B-064F8C?logo=cmake)](https://cmake.org/) [![License](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE) [![Platform](https://img.shields.io/badge/Platform-Linux%20%7C%20Windows%20%7C%20Android-1793D1)](https://github.com)

> English: [README.md](README.md)

📖 **在线使用手册：[linductor-alkaid.github.io/kairo](https://linductor-alkaid.github.io/kairo/)**

**Kairo 是一个面向 C++20 应用的进程内并发执行基础设施库。**

它通过统一 Facade 管理普通异步任务、低延迟队列、周期实时线程、长期 Blocking I/O 和可选 GPU 工作，并提供有界通信、任务编排、背压与生命周期诊断。

大多数用户只需要从 `submit_auto()` 开始；只有遇到明确的周期、容量、I/O 或数据传递约束时，才需要进入专用路径。

## 为什么使用 Kairo

- **一个入口，多种执行模型**：线程池、无锁低延迟、专用实时线程、Blocking I/O 和 GPU 由同一个 `Executor` 管理。
- **保留真实结果语义**：普通任务返回 `future`，有界投递报告是否接收，长期 worker 返回生命周期 handle，不把不同模型伪装成同一种接口。
- **常用并发能力开箱即用**：支持优先级、延迟、软周期、批量、任务依赖、协作取消、定时句柄、FIFO 串行上下文，以及 FIFO、最新值、快照、阶段同步和 Topic 扇出。
- **失败和过载可观察**：提交拒绝、任务异常、超时、实时丢弃、队列深度、总量 admission 拒绝和通信延迟都可通过结果、状态或 callback 观察。
- **渐进式接入**：默认 CPU 路径无需先理解底层执行器；实时、无锁和 GPU 能力可以按实际需求逐步启用。

典型场景包括后台计算、机器人与设备控制、传感器采集、长期 I/O 服务，以及需要 CPU/GPU 混合执行的本地应用。

## 五分钟上手

### 构建与测试

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

要求 C++20 和 CMake 3.16+。支持 Linux 与 Windows；Android 提供 CPU-only 支持，可通过 NDK 工具链构建，见 [PACKAGE_ANDROID.md](docs/PACKAGE_ANDROID.md)。GPU 后端是可选能力，没有可用 GPU 时不影响普通 CPU 路径。

### 提交第一个任务

```cpp
#include <kairo/executor.hpp>
#include <iostream>

int main() {
    auto& ex = kairo::Executor::instance();
    auto result = ex.submit_auto([] { return 42; });

    std::cout << result.get() << '\n';
    ex.shutdown();
}
```

`submit_auto(lambda)` 默认使用普通异步执行器。`future::get()` 返回结果，并在任务失败时重新抛出异常。需要自定义线程数、容量或监控配置时，再显式调用 `initialize()`。

完整的构建、链接和异常处理过程见[第一个任务](website/zh/quick-start/first-task.md)。

## 按工作语义选择入口

先确定调用方需要确认的是“工作完成”“队列接收”，还是“长期 worker 已启动”。

| 工作类型 | 推荐入口 | 调用方得到什么 |
| --- | --- | --- |
| 普通有限 CPU 工作 | `submit_auto(lambda)` | `future` 中的返回值或异常 |
| 优先级、延迟、软周期、批量或依赖 | 对应的显式 Facade API | `future`、task ID 或 `TaskHandle` |
| 已验证的低延迟或周期实时路径 | `dispatch_auto(...)` | 有界队列是否接收，不代表完成 |
| 长期、可中断的 I/O 循环 | `start_worker(...)` | 启动结果与 `WorkerHandle` 生命周期 |
| 独立的 CPU/GPU 实现 | `submit_auto(cpu_gpu_task(...))` | 已选路径的完成或异常 |
| 长期运行线程之间传递数据 | `kairo::comm` | FIFO、最新值、快照、阶段或订阅语义 |

`Auto` 不会为了追求性能而静默选择无锁或实时后端。详细选型见[如何选择提交接口](website/zh/guides/choosing-submit-api.md)。

> **批量性能说明**：`submit_batch()` 与 `submit_batch_no_future()` 可以减少重复提交开销，但项目不承诺固定加速比。基准数据日期为 2026-07-09，结果与环境记录见 [batch_submit_baseline_2026-07-09.json](docs/performance/batch_submit_baseline_2026-07-09.json)。可通过 `cmake --build build --target benchmark_batch_scales benchmark_batch_submit_real benchmark_batch_submit_concurrent -j2` 构建，并依次运行 `./build/tests/benchmark_batch_scales`、`./build/tests/benchmark_batch_submit_real` 和 `./build/tests/benchmark_batch_submit_concurrent` 复现。

## 跨线程通信

`kairo::comm` 按数据语义提供进程内通信组件：

| 需求 | 组件 |
| --- | --- |
| 每条消息由一个消费者按 FIFO 处理 | `MpscChannel<T>` |
| 控制循环每周期消费有限命令 | `RealtimeChannel<T>` |
| 只关心最新配置或目标值 | `LatestMailbox<T>` |
| 多个读者读取完整一致的状态 | `DoubleBuffer<T>` |
| 初始化、标定和运行按阶段推进 | `PhaseGate` / `Sequencer` |
| 多个模块各自接收同一事件流 | `Topic<T>` / `TopicSubscription<T>` |

容量、丢弃策略和关闭语义属于业务契约，而不是实现细节。选择建议与实时边界见[如何选择通信组件](website/zh/guides/choosing-communication.md)。综合场景可参考[机器人数据流水线示例](examples/comm_robot_pipeline.cpp)。

## 能力边界

Kairo 有意保持以下边界：

- 它不是协程运行时，也不提供 coroutine scheduler。
- 它不是分布式消息系统或数据流框架；Topic 只负责进程内扇出，不提供网络传输、持久化、重放或确认。
- 它不是硬实时操作系统；最终 jitter 仍取决于任务体、操作系统、权限、CPU 隔离、内存驻留和目标硬件。
- 它不能安全地强制终止任意正在运行的 C++ 函数；取消（`request_task_cancel` / `StopToken`）是协作请求而非抢占，长期工作必须轮询停止令牌；`TaskOptions::deadline` 仍只是路由/诊断提示，不会自动触发取消。
- `submit_periodic()` 是普通线程池上的软周期任务，不等同于专用实时线程。
- 默认异步提交默认不限制总量在途数；需要结构化过载拒绝时显式配置 `max_in_flight_tasks`（覆盖普通/优先级/批量/串行等 facade 提交，不含 timer 派发与 realtime/GPU，见 `docs/API.md` §3.10）。`queue_capacity` 只是每 worker 本地队列参数，不是背压边界。
- Facade 定时句柄（`TimerHandle`）把到期工作派发到普通线程池；它不绑定外部事件循环（asio strand），必须在同一 strand 上执行并销毁的定时器仍由应用自行管理。见[外部事件循环互操作指南](docs/external_event_loop_interop.md)。

自 0.4.0 起，通信组件为关键同步路径提供固定存储和原子实现，但“同步无锁”不覆盖 payload 操作、callback、缺页或 OS 调度。`Topic<T>` 属于普通控制面，不是实时原语。精确保证见[迁移说明](docs/MIGRATION.md)。

## 安装与集成

```bash
cmake --install build --prefix /usr/local
```

在消费者项目中：

```cmake
find_package(kairo REQUIRED)

add_executable(myapp main.cpp)
target_link_libraries(myapp PRIVATE kairo::executor)
```

也可以通过 `add_subdirectory(path/to/kairo)` 直接集成。静态库、动态库、构建选项和发布包说明见 [BUILD.md](docs/BUILD.md)。

## 继续阅读

| 目标 | 文档 |
| --- | --- |
| 判断 Executor 是否适合项目 | [Kairo 是什么](website/zh/getting-started/what-is-kairo.md) |
| 从构建到第一个真实任务 | [快速开始](website/zh/quick-start/build.md) |
| 理解主要类型和完整契约 | [API 文档](docs/API.md) |
| 接入实时线程和通信组件 | [实时与通信](website/zh/realtime-and-communication/index.md) |
| 注册并诊断 GPU 后端 | [GPU 执行](website/zh/gpu/index.md) |
| 从旧版本升级 | [迁移指南](docs/MIGRATION.md) |
| 查看版本变化 | [CHANGELOG](CHANGELOG.md) |

更多可运行代码见 [examples](examples/) 和 [tutorial](examples/tutorial/)。使用 AI 辅助接入时，可让 agent 先读取 [Kairo integration skill](docs/skill/kairo-integration/SKILL.md)。

## 版本与许可

当前版本：**v0.6.1**

Kairo 使用 [MIT License](LICENSE)。
