---
title: 版本与迁移
description: 当前开发快照、发布版本和 API 迁移的入口。
---

# 版本与迁移

## 当前版本说明

项目 CMake 与最新发布记录的版本均为 `v0.5.3`。本站以该稳定版为基线，同时跟随 `master` 的后续开发；未在稳定 tag 中发布的能力不构成版本承诺。首发不维护历史版本站点；发布时应以 tag 重新核对页面。

| 需要确认什么 | 入口 |
| --- | --- |
| 已发布版本与破坏性变更 | [CHANGELOG.md](https://github.com/Linductor-alkaid/kairo/blob/master/CHANGELOG.md) |
| 从旧 API 的推荐迁移路径 | [MIGRATION.md](https://github.com/Linductor-alkaid/kairo/blob/master/docs/MIGRATION.md) |
| 选项、编译器与后端前置 | [BUILD.md](https://github.com/Linductor-alkaid/kairo/blob/master/docs/BUILD.md) |
| 当前完整签名 | [API.md](https://github.com/Linductor-alkaid/kairo/blob/master/docs/API.md) |

## 0.5.3：评审修复与定时器事件驱动

v0.5.3 是稳定性与性能维护版本，公开 API 签名不变。落地 2026-09-30 全量代码评审四个阶段（P0 内存安全/挂死/数据竞争 9 项、P1 功能正确性 24 项、构建/打包 8 项、热路径性能 10 项），并把定时器线程从 1kHz 轮询改造为事件驱动条件等待（空闲等待 CPU 降约 34 倍，periodic 网格锚定使抖动改善 10-27 倍）。两处可观察的定时器行为变化见 [MIGRATION.md](https://github.com/Linductor-alkaid/kairo/blob/master/docs/MIGRATION.md) 的"从 0.5.2 升级到 0.5.3"一节；无需改代码。

## 0.5.2：依赖驱动调度

v0.5.2 把任务图依赖等待演进为 dependency-driven scheduling：`submit_after` 的 dependent 在依赖未就绪时不再入队占用 worker，parked 超时与 shutdown 结算语义详见 [MIGRATION.md](https://github.com/Linductor-alkaid/kairo/blob/master/docs/MIGRATION.md) 的"从 0.5.0 升级到 0.5.2"一节。公开 API 签名不变（新增诊断接口 `closure_graveyard_size()`）。

## `bool` 到 `_ex` 的迁移

旧入口保持兼容，适合调用方只需成功/失败的场景；新代码在需要诊断、日志或可靠回退时优先使用 `_ex`，读取 `ExecutorResult::error_code` 与 `message`。

<div class="migration-table">

| 迁移 | 适用情况 |
| --- | --- |
| `initialize(config)` → `initialize(config)` | 配置错误、重复初始化或 shutdown 后调用需要区分原因。 |
| `register_realtime_task(name, config)` → `register_realtime_task(name, config)` | 需要区分非法配置、重名、权限/启动问题。 |
| `start_realtime_task(name)` → `start_realtime_task(name)` | 需要区分不存在、重复启动与平台启动失败。 |
| `register_gpu_executor(name, config)` → `register_gpu_executor(name, config)` | 需要区分无效配置与 `BackendUnavailable`。 |
| `wait_for_completion()` → `wait_for_completion_for()` / `_ex()` | 不可无限等待，或超时后需要状态快照。 |
| `IRealtimeExecutor::push_task()` → `Executor::try_push_realtime_task()` | 希望得到拒绝返回、failure event 和背压计数。 |

</div>

`_ex` 不是“总是更好”的第二套业务 API：若调用方只需布尔结果，兼容入口仍有效。迁移的价值在于把失败原因接到业务日志、告警或降级策略，而非改变任务执行模型。

## 0.3.1：从后端优先到意图优先

新代码的默认阅读和接入顺序是先使用 `submit_auto(lambda)`，再在业务明确需要 CPU/GPU 双实现、有界 admission 或长期 worker 生命周期时进入专用路径：

| 已有写法/需求 | 0.3.1 推荐入口 | 保持不变的边界 |
| --- | --- | --- |
| 普通 `submit(lambda)` | 可逐步改为 `submit_auto(lambda)` | 两者都返回 future；`submit()` 仍是显式线程池入口。 |
| 一个 callable 用 `nullptr` 分支 CPU/GPU | `cpu_gpu_task(cpu, gpu)` + `submit_auto()` | legacy 四参数 overload 在 `0.3.x` 保持可用且不隐式回退。 |
| 直接无锁 `push_task()` | 注册后使用 `dispatch_auto(LowLatency)` | `accepted` 只表示接收，单消费者和背压语义不变。 |
| 直接实时 `push_task()` | 已启动后使用 `dispatch_auto(RealtimeQueue)` | `accepted` 不表示后续周期完成，不会回退线程池。 |
| 分别注册、启动 I/O worker | `start_worker(BlockingWorkerSpec)` | `WorkerHandle` 保留 wakeup、stop token、启动超时和退出原因。 |

自动路由不会推断 callable 的实时安全、线程安全、GPU 内存所有权或 I/O 可中断性。`get_executor_capabilities()` 只提供建议性状态快照；所有实际投递仍须处理停止竞争和背压。

## 0.5.0：任务生命周期语义、Android 一期与热路径性能

0.5.0 保持既有公开提交 API 兼容，把任务级协作取消、可取消可重排的定时句柄、
串行执行上下文与总量有界 admission 转正；Android CPU-only 交叉编译纳入一期；
2026-09 性能审查的 P1/P2 两阶段重构显著改善提交吞吐与实时 jitter。发布产物新增
CI 自动打包的 Linux amd64 deb（CUDA devel 容器完整构建）与 Windows x64 静态库。

| 需求 | 0.5.0 入口 | 仍需自行保证的边界 |
| --- | --- | --- |
| 取消排队中/运行中的任务 | `submit_cancellable*` + `request_task_cancel()` | 取消是协作请求而非抢占；运行中任务须检查注入的 `StopToken` 并及时返回。 |
| 可取消、可重排的定时任务 | `submit_delayed/periodic_*_with_handle` + `TimerHandle` | 定时到期派发到普通线程池，不绑定外部事件循环（asio strand 场景见互操作指南）。 |
| 同一执行上下文严格按提交顺序结算 | `submit_on` / `submit_on_with_handle` | 只保证顺序；同上下文单任务耗时过长仍会推迟后续任务。 |
| 结构化过载拒绝 | `ExecutorConfig::max_in_flight_tasks` | 默认 `0` 不启用；达到上限时 future 以 `CapacityExhaustedException` 就绪，须自行处理。 |
| 解析状态快照文本 | `ExecutorSnapshot` schema 3 | 新增 `cancellation`/`timers` 字段为纯新增；按列数或字段总数断言的解析器需要放宽。 |
| Android CPU-only 交叉编译 | NDK r26c/r28b 脚本与 CI | 线程优先级、亲和性、`mlockall` 与 timer slack 均 best-effort，不承诺硬实时。 |

迁移提示：`ExecutorSnapshot` schema 2 → 3、进程内存锁租约与 shutdown 清理
delayed 任务的行为变化见仓库 [MIGRATION.md](https://github.com/Linductor-alkaid/kairo/blob/master/docs/MIGRATION.md)
的“从 0.4.0 升级到 0.5.0”一节。

## 0.4.0：固定同步边界与通信可观测性

0.4.0 将通信同步核心改为构造期固定存储和原子状态，同时保留既有主要调用方式。新代码可按数据语义选择 `Topic<T>`、LET phase-bound 通信、延迟分位数和实时分配诊断；这些能力不会替调用方证明整个业务链路的实时性。

| 需求 | 0.4.0 入口 | 仍需自行保证的边界 |
| --- | --- | --- |
| 向多个普通消费者独立扇出事件 | `comm::Topic<T>` 与 `TopicSubscription<T>` | Topic 使用 mutex 与动态分配，不是实时或无锁数据面。 |
| 只在阶段边界交换一致数据 | 为 `PhaseGate`、`DoubleBuffer`、`LatestMailbox` 绑定 LET phase | 每相位只允许一次发布；转换中的读写和缺少上一相位数据会被拒绝。 |
| 评估通信时延趋势 | `CommStats` 的近似 `p50_latency`、`p99_latency` | 分位数是固定直方图近似值，不能代替端到端时延测量。 |
| 发现受保护实时路径中的分配 | `RealtimeAllocationGuard` 与 `RealtimeThreadConfig::enable_allocation_guard` | 只在启用的 Linux 构建和受保护路径记录；payload、时钟、缺页与调度仍须整体测量。 |
| 限制已完成任务图句柄占用 | `task_graph_retention_capacity` | 被保留的活跃依赖不会提前淘汰；淘汰句柄会明确拒绝为过期。 |
| 在线调整线程池 worker 数 | `ThreadPool::resize()` / `ThreadPoolResizer` | 仅可在初始化配置的范围内调整；应在目标负载下验证吞吐和收敛时延。 |

`MpscChannel`、`RealtimeChannel`、未绑定 `DoubleBuffer`、`PhaseGate` 和 `Sequencer` 的同步核心可通过 `is_synchronization_lock_free()` 检查。该结论只覆盖组件同步原子和固定存储，不覆盖 `T` 的操作、callback、时钟、缺页、调用方分配或 OS 调度。迁移实时路径时优先使用非等待 API，关闭高频 callback，并在目标硬件上验证完整链路。

## 升级检查

1. 阅读目标版本 CHANGELOG，并确认本页所述能力已经在目标 tag 中存在。
2. 用目标编译器、操作系统与 GPU/实时权限重新配置并构建。
3. 将初始化、实时/GPU 注册等关键边界换为 `_ex`；为 `future`、返回值和状态计数保留观察路径。
4. 对实时配置复查亲和性、内存锁与 timer slack 的实际应用状态；对 GPU 复查后端、驱动和设备。
5. 运行测试和教程 smoke tests，再在目标负载下复测超时、背压与性能。

## 术语约定

- **稳定公开 API**：`include/kairo/` 下安装并受兼容约束的声明。
- **兼容入口**：为保留既有调用而存在的 `bool` / `void` API；不等于废弃。
- **开发快照能力**：`master` 中已有但尚未标记到稳定发布版本的内容。
- **测试钩子和内部实现**：测试注入 API、`src/` 类型和实现细节，不作为普通集成依赖。

## 发布前核对

发布维护者应更新 CMake 项目版本、CHANGELOG、MIGRATION、README 和本站版本文本；然后对照[API 覆盖索引](/zh/reference/api)检查 Facade 分组，确保新增公开入口至少有教程、专题、选型或参考说明。
