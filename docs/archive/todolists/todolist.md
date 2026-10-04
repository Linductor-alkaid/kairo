# Executor 项目实现任务清单

本文档基于 [Executor 工具项目架构设计](executor.md) 和 [C++ 项目设计方案](cpp-project-design.md)，列出项目实现的任务清单。

---

## 阶段 0：项目初始化

- [x] 创建项目目录结构
- [x] 创建根 `CMakeLists.txt`
- [x] 创建 `cmake/CompilerWarnings.cmake`
- [x] 创建 `cmake/Sanitizers.cmake`（可选）
- [x] 创建 `cmake/ExecutorConfig.cmake`（可选，用于 find_package 支持）
- [x] 创建 `src/CMakeLists.txt`
- [x] 创建 `tests/CMakeLists.txt`
- [x] 创建 `examples/CMakeLists.txt`

---

## 阶段 1：接口与类型定义

- [x] 实现 `include/executor/config.hpp`（ExecutorConfig, ThreadPoolConfig, RealtimeThreadConfig）
- [x] 实现 `include/executor/types.hpp`（AsyncExecutorStatus, RealtimeExecutorStatus, Task, TaskPriority）
- [x] 实现 `include/executor/interfaces.hpp`（IAsyncExecutor, IRealtimeExecutor, ICycleManager）
- [x] 实现 `include/executor/executor_manager.hpp`（ExecutorManager 类声明）

---

## 阶段 2：工具模块（util）

- [x] 实现 `src/executor/util/lockfree_queue.hpp`（无锁队列）
- [x] 实现 `src/executor/util/exception_handler.cpp`
- [x] 实现 `src/executor/util/thread_utils.cpp`（线程优先级、CPU 亲和性，支持 Linux/Windows/Android（Android 为 CPU-only））
- [x] 编写 util 模块单元测试

---

## 阶段 3：任务模块（task）

- [x] 实现 `src/executor/task/task.cpp`
- [x] 实现 `src/executor/task/task_dependency_manager.cpp`
- [x] 编写 task 模块单元测试

---

## 阶段 4：线程池模块（thread_pool）

- [x] 实现 `src/executor/thread_pool/priority_scheduler.cpp`
- [x] 实现 `src/executor/thread_pool/thread_pool.cpp`（基础版本，暂不实现动态扩缩容）
- [x] 编写 thread_pool 模块单元测试

---

## 阶段 5：线程池执行器

- [x] 实现 `src/executor/thread_pool_executor.cpp`（实现 IAsyncExecutor 接口）
- [x] 编写 ThreadPoolExecutor 集成测试

---

## 阶段 6：执行器管理器

- [x] 实现 `src/executor/executor_manager.cpp`（单例模式 + 实例化模式）
- [x] 实现 `initialize_async_executor` 和 `get_default_async_executor`
- [x] 实现 `register_realtime_executor` 和 `get_realtime_executor`
- [x] 实现 `create_realtime_executor`（便捷方法）
- [x] 实现 `get_realtime_executor_names` 和 `shutdown`
- [x] 实现 RAII 生命周期管理（析构时自动释放所有执行器）
- [x] 编写 ExecutorManager 集成测试

---

## 阶段 7：实时线程执行器

- [x] 实现 `src/executor/realtime_thread_executor.cpp`（实现 IRealtimeExecutor 接口）
- [x] 实现内置 `simple_cycle_loop`（使用 `std::this_thread::sleep_until`）
- [x] 编写 RealtimeThreadExecutor 集成测试

---

## 阶段 8：Executor Facade

- [x] 实现 `include/executor/executor.hpp`（Facade 模式）
- [x] 实现 `src/executor/executor.cpp`（单例模式 + 实例化模式）
- [x] 实现 `submit`、`submit_priority`、`submit_delayed` 等任务提交 API
- [x] 实现 `register_realtime_task`、`start_realtime_task`、`stop_realtime_task` 等实时任务 API
- [x] 实现监控查询 API（`get_async_executor_status`、`get_realtime_executor_status` 等）
- [x] 编写 Executor Facade 集成测试
- [x] 编写示例 `examples/basic_submit.cpp`

---

## 阶段 9：可选功能 - 线程池增强

- [x] 实现 `src/executor/thread_pool/task_dispatcher.cpp`
- [x] 实现 `src/executor/thread_pool/load_balancer.cpp`
- [x] 在 `thread_pool.cpp` 中实现动态扩缩容功能
- [x] 实现工作窃取（Work Stealing）机制
- [x] 编写 LoadBalancer 和动态扩缩容单元测试

---

## 阶段 10：监控模块（可选）

- [x] 实现 `src/executor/monitor/task_monitor.cpp`
- [x] 实现 `src/executor/monitor/statistics_collector.cpp`
- [x] 在 `Executor` 中实现 `enable_monitoring`、`get_task_statistics` 等 API
- [x] 编写监控模块单元测试

---

## 阶段 11：ICycleManager 集成（可选）

- [x] 在 `RealtimeThreadExecutor` 中支持注入 `ICycleManager`
- [x] 实现 `cycle_loop` 方法（使用外部周期管理器）
- [x] 编写 ICycleManager 集成测试
- [x] 编写示例 `examples/realtime_can.cpp`（展示周期管理器使用）

---

## 阶段 12：测试与示例完善

- [x] 完善所有模块的单元测试覆盖
- [x] 编写端到端集成测试（完整工作流测试）
- [x] 编写性能测试和压力测试
- [x] 编写示例 `examples/realtime_can.cpp`（CAN 通信实时线程示例，已完善多通道演示）
- [x] 编写示例 `examples/multi_project.cpp`（多项目/多模块使用示例）
- [x] 配置 CTest 测试框架
- [x] 配置代码覆盖率工具（如 gcov/lcov）

---

## 阶段 13：文档与发布准备

- [x] 编写 API 使用文档（README.md 和 API.md）
- [x] 编写构建说明文档
- [x] 编写迁移指南（如有）
- [x] 创建 `executorConfig.cmake` 以支持 `find_package(executor)`
- [x] 配置安装规则（头文件、库文件）
- [x] 添加版本号管理
- [x] 编写 CHANGELOG.md
- [x] 代码审查和重构优化
- [x] 性能测试和优化（已添加优化基线性能测试）
- [x] 准备发布包

---

## 阶段 14：Facade 完整度与失败可观察性

- [x] 修正文档宣称：自动调优可安全回退，任务失败/提交拒绝/丢任务/超时必须可观察
- [x] 执行 [Facade 完整度与失败可观察性更新计划](facade_observability_update_plan.md)

---

## 阶段 15：通信与并发辅助 Facade

- [x] 执行 [通信与并发辅助 Facade 更新计划](comm_facade_update_plan.md)
- [x] 落地 [通信与并发辅助 Facade 设计](../design/comm_facade.md)

---

## 阶段 16：使用手册网站

- [x] 完成 [Executor 使用手册网站规划](../design/user_guide_website.md)
- [x] 执行 [Executor 使用手册网站实施计划](user_guide_website_plan.md)（网站已建成并随各阶段同步更新）

---

## 阶段 17：Android 平台适配

- [x] 评审 [Android 适配方案](../design/android_port.md)
- [x] 执行 [Android 适配实施计划](android_port_plan.md)（一期 154 项完成）
- [x] 完成 Android CPU-only 交叉编译 CI（NDK，arm64-v8a / x86_64，static / shared，API 21）
- [x] 完成 `executor::StopToken` 兼容层与 Blocking I/O 生命周期迁移
- [x] 完成 Android best-effort 调度语义和线程数 / cpuset 自适应
- [ ] ⏸ 门控（硬件）：完成 arm64 设备 smoke test、Blocking I/O 与 MPSC 弱内存序压力验证
  （big.LITTLE 真机未到位；模拟器 6/6 用例与 ARM64 Neoverse-N2 runner 600s soak 已覆盖，
  见 docs/performance/android_a3_validation.md；发布前 gate 登记于 docs/RELEASE_CHECKLIST.md）
- [x] 完成 Android 打包与集成文档（NDK CMake / AGP / Prefab / `c++_shared`）

---

## 阶段 18：客户端反馈缺口收敛（协作取消、定时句柄、序列化上下文）

输入来源：heyaki 反馈台账（2026-08-29 盘点）P1-1/P1-2/P1-3；P2-1/P2-2 延后重估。

- [x] 执行 [客户端反馈缺口收敛更新计划](client_feedback_update_plan.md)（C1/T1/S1/S2/D1/D2 已完成）
- [x] 完成任务级协作取消令牌（P1-3）：排队/运行中取消语义、取消可观测
- [x] 完成可绑定生命周期的定时句柄（P1-2）：cancel/reschedule、Scoped 句柄销毁即取消、
  纳入监控（外部 strand 绑定由 T2/S2 门控；S2 已由阶段 19 落地）
- [x] 完成外部事件循环互操作指南（P1-1 第一步），并依据评审结论决定序列化上下文 API 是否落地（SerialExecutionContext 已随阶段 19 合入）
- [ ] ⏸ 门控（下游）：P2-1/P2-2 重估门：待 heyaki M6/M7 消息与文件传输压测后定形

---

## 阶段 19：Mira 反馈缺口收敛（总量 admission、串行 facade wrapper 安全）

输入来源：Mira 仓库 `docs/executor_feedback/ledger.md`（2026-08-30）EXE-20260830-001/002/003，
三条缺口已在 master `2af11a3` 上经代码核查确认。

- [x] 执行 [Mira 反馈缺口收敛更新计划](mira_feedback_update_plan.md)（W0/W1/A0/A1/D3
  已完成；Mira 台账 Accepted 与 compatibility boundary 移除统计待发布版本后回填）
- [x] 完成串行 facade wrapper 非阻塞共享状态重构（EXE-20260830-002/003）：消除多 worker
  饥饿与栈条件变量 notify/析构竞争，TSAN 与两 worker × 10,000 突发压测通过
- [x] 完成默认异步提交的总量有界 admission（EXE-20260830-001）：跨 scheduler 与本地队列的
  可配置总容量、可区分 capacity rejection、终态恰好一次释放
- [x] 完成 API/迁移/README/网站同步，并回写 Mira 台账状态（Proposed → Accepted，
  引用 issue #178/#179 与上游 master 提交 def5200）

---

## 阶段 20：性能审查收敛（热路径锁竞争、无锁承诺兑现、文档同步）

输入来源：2026-09-08 master `e0924c4` 四路性能与文档审查（核心线程池热路径 /
comm·timer·取消原语 / GPU·监控路径 / 文档同步），问题登记 PA-1 ~ PA-36，
逐条含 file:line 证据。

- [x] 执行 [性能审查收敛计划](performance_audit_2026-09_plan.md)（阶段 D0 文档同步
  随本批完成；P1 线程池热路径三连、P2 无锁兑现、P3/P4 定时器·comm·facade·监控、
  P5 GPU 待排期）
- [x] P1：线程池提交热路径三连（全局锁 + 持锁谓词 + 每次派发堆分配）与 Task 复制链
  （随 0.5.0 合入，详见 performance_audit_2026-09_plan.md 阶段 P1 ✅）
- [x] P2：ObjectPool 无锁化与 RT 优先级反转消除、LockFreeWorkerQueue 兑现
  （随 0.5.0 合入，详见 performance_audit_2026-09_plan.md 阶段 P2 ✅）
- [x] P3：timer 1kHz 轮询改条件编译驻停、comm 阻塞原语退避、Topic RCU、（0.5.3 已完成：定时器线程事件驱动改造）
  取消/周期 tick 热路径分配（2026-09-22 逐项核实确未实施）
- [ ] P4：task graph 分片、默认执行器原子快照、TaskMonitor 先查再锁
  （PA-14 已随 P1 顺带落地；PA-6 的调度侧唤醒深化转入阶段 21）
- [ ] P5：GPU 持锁阻塞调用挪出、loader 函数表缓存、pinned memory、optimizer 去留
  （⏸ 门控：GPU 性能项需真实 CUDA/OpenCL 环境记录数据；optimizer 去留待用户反馈）

---

## 阶段 21：v0.5.x —— 待办账实清理与 dependency-driven scheduling

v0.5.x 两条主线：待办账实对齐（2026-09-22 对 15 份计划文档逐项审计核实），
以及 submit_after 从"worker 占用等待条件变量"演进为"调度侧唤醒、依赖驱动调度"
（承接 performance_audit PA-6 的深化方案）。

- [x] 待办账实清理：15 份计划文档逐项核对——回填已完成未勾项、修正失实勾选、
  门控项显式标注（⏸）、发布前清单重定性为可重跑模板
- [x] 设计文档 docs/design/dependency_driven_scheduling.md：依赖未就绪不入队
  （parked ready 结构），依赖终态级联入队、失败即时结算、task_graph_cv_ 退役；
  载明语义决策：queued soft timeout 计时起点、shutdown 对 parked 任务的结算、
  admission 占额语义、priority/执行器快照沿用
- [x] 实施调度侧唤醒改造：复用 resolve_task_graph_dependents_locked 级联骨架与
  取消 phase CAS 仲裁；保持"计数先于 future"不变式（PR #177）与有界 admission 语义
  （PR-1 #197 + PR-2 #198 + PR-3：CV 退役/Queued 补记/墓地观测，随 0.5.x 合入）
- [x] TSAN 全量回归 + 依赖等待路径并发基准（承接 performance_audit PA-6 验收口径；
  benchmark_task_graph_paths 交付并回填台账数据；TSAN/ASAN 全零，
  独立验证四轮终局 PASS，随 PR-3 合入收口）
