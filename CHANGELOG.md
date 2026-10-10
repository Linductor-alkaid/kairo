# Changelog

本文档记录 kairo 项目的版本变更。版本号遵循 [语义化版本](https://semver.org/lang/zh-CN/)。

---

## [Unreleased] - 0.7.0 开发中

v0.7.0 前置清债（M0，`docs/design/roadmap_v0.7.md` §2.1）：在引入任何
自适应调度代码之前，先消除会污染反馈测量或放大自适应风险的遗留项。
全部为 additive 或纯结构改动；默认行为与 0.6.1 一致。

### 结构

- **executor.cpp 拆分**（纯结构，零行为变化）：2242 行单体实现按职责
  拆为 `executor_lifecycle` / `executor_task_graph` / `executor_timers` /
  `executor_backends` / `executor_routing` 五个编译单元，共享辅助提取到
  内部头 `src/kairo/executor_detail.hpp`。方法集合（117 个）与导出符号
  面逐一核验不变。

### 线程池

- **可选防饿死 aging（CR-024，additive，默认关闭）**：
  `PriorityScheduler::set_aging_policy(enabled, boost_interval_ns)`。
  开启后 dequeue 在全部非空队列堆顶中按
  （有效优先级, EDF, 提交 FIFO）选取——堆顶任务每等待
  `priority_aging_interval_ns` 提升一级有效优先级（至多 CRITICAL），
  队列内部堆序与任务自身 priority 不改写。关闭时出队路径与 0.6.1
  逐位一致。`ThreadPoolConfig` / `ExecutorConfig` 新增
  `enable_priority_aging`（默认 false）与
  `priority_aging_interval_ns`（默认 100ms）。NN-05 本地队列倒置窗口
  维持文档化现状（worker 扫描顺序未变）。

### 可观测性

- **TaskMonitor 分片化与热路径门控（CR-071/CR-163）**：单把全局 mutex
  改为哈希分片（id 映射与 in-flight 快照按 task_id 分 16 片，聚合统计
  按 task_type 分 8 片），dropped/evicted/in-flight 计数改原子。
  新增热路径快速门：统计采样率为 0 时不触碰 id 映射与统计分片；
  in-flight 采样率为 0 或容量为 0 时不触碰 in-flight 分片——两门全关时
  `record_task_start/complete/timeout` 零取锁。配套语义补全（对齐
  `set_enabled(false)`）：
  - `set_sampling_rate(0.0)` 现在清空 task_id→type 映射：已开始未结算
    的统计样本随清空丢弃（语义即"关闭统计采样"）；
  - `set_in_flight_sampling_rate(0.0)` 现在清空 in-flight 快照（此前
    已采样条目会残留至被 complete 擦除）。
  in-flight 容量语义微调：分片化后为跨分片软上界（原子计数预留，
  并发下至多短暂超限 1-2 条）。验收：采样率 0 时多线程提交吞吐与
  关闭监控持平 ±5%（配对实测 ±1.2%）。

### 提交路径

- **每任务堆分配基线（CR-107，第一阶段）**：`submit()` 主路径的
  promise 与 ready 标志合并为单控制块（`detail::PromiseCell`）；
  `generate_task_id` 经 `std::to_chars` 单次拼接；池提交路径消除
  monitor_id 中间拷贝。新增 `benchmark_submit_allocations` 基准
  （替换全局 operator new，确定性计数）：submit 路径 10.00 →
  **9.00 allocs/task**（816 → 768 字节/task），tracked 路径 19.13 不变。
  剩余分配项受结构约束（move-only 可调用需间接层、双 std::function 是
  后端接口、Task 含 atomic、快照字符串是公开契约），留待接口层调整。

### 任务依赖

- **依赖环检测增量化（CR-052）**：`TaskDependencyManager` 引入
  Pearce-Kelly 风格增量拓扑序（节点序号 + 反向边表）。加边快路径
  O(1)（序约束已满足），不再做锁内全图 DFS；违序时三段式受限重排，
  环检测并入同一次遍历。链式建链从 O(n²)（n=4000 约 3.9s）降至
  n=20000 约 28ms；与插入序完全相反的最坏形态仍为 O(n²) 纯内存遍历
  （与旧实现同阶，facade 真实形态全走快路径）。新增
  `test_task_dependency_topo`（17 用例，含 n=20000 规模回归与固定种子
  随机对拍）。

### 性能

- M0 前后基准对比记录见
  `docs/performance/m0_debt_paydown_results.md`：两个既有热路径基准
  （`benchmark_scheduling_paths` / `benchmark_thread_pool_hotpath`）
  配对验证无回归；EDF dequeue 的关闭路径仅增加一次原子读。

---

## [0.6.1] - 2026-10-06

0.6.1 不扩张调度策略维度，对 0.6.0 建立的 Scheduling Runtime 边界做
**稳定化、可观测性补全与工程验证**。全部为 additive extension：不改变
提交协议、调度模型语义，也不包含任何自适应调度行为。设计文档：
`docs/design/scheduling_runtime.md` §6。

### Scheduling Runtime 可观测性

- **结构化路由决策**：`RoutingDecision` 新增 `status`
  （`Accepted` / `AcceptedDegraded` / `Rejected`，接受/拒绝的权威判据）、
  `diagnostics` 位掩码（`RoutingDiagnostics::AffinityMismatch` /
  `ResourceInfeasible`）与稳定 reason code——过期 deadline 拒绝从泛化
  `Rejected` 细化为 `DeadlineExpired`，affinity 不相交从 detail 前缀
  升级为 `AcceptedDegraded` + `AffinityMismatch`。`detail` 仅供人阅读，
  不再是程序判断调度结果的唯一接口。新增
  `routing_status_to_string()` / `routing_reason_to_string()`。
- **0.6.0 风格自定义调度器兼容**：只设置拒绝类 reason、未设置 status
  的决策由 `Executor::route_task()` 归一化为 `Rejected`，行为不漂移。
- **调度指标**：`Executor::get_scheduling_metrics()` 返回
  `SchedulingMetrics` 单调计数（accepted / accepted_degraded /
  rejected / deadline_rejected / backend_unavailable_rejected /
  capacity_rejected / resource_rejected / affinity_mismatch /
  deadline_missed / feedback_reported）。计数在
  `record_routing_decision` 内以 relaxed 原子无条件累加，不受 CR-106
  观测开关影响，无需预先配置即可读取调度健康度。
- **执行期反馈（measurement contract）**：`SchedulingFeedback` 扩展
  backend / executor_name / queue_wait_ns / execution_duration_ns /
  had_deadline / deadline_missed / failure_kind（success 时
  `FailureKind::None`）；`IScheduler::wants_feedback()`（默认 false，
  `DefaultScheduler` 不消费）作为热路径开关——关闭时反馈通道零开销，
  开启时按任务付 2 次时钟采样 + 一次 worker 线程同步
  `on_task_completed()`（异常隔离）。覆盖实际开始执行的默认池任务；
  v0.7.0 前不实现任何基于反馈的自适应策略。

### 契约锁定与文档治理

- deadline 准入边界锁定：严格已过（`now > deadline`）才拒绝，恰好相等
  接受；错过仍"记录 miss 但执行"。
- capability snapshot 弱一致语义文档化：未知即宽容（跳过检查）、已知
  但不满足即结构化拒绝（`ResourceInfeasible` 诊断位）、admission
  feasibility 不等于 execution guarantee（TOCTOU 由后端执行失败报告）。
- CpuGpuTask 的 heuristic-CPU + 非 AllowCpu fallback 组合在记录决策前
  修正为 `Rejected`，计数与最终投递结果一致（0.6.0 会先记录 Accepted
  再拒绝）。
- 新增 `scripts/check_docs_drift.sh` 并接入 docs CI：扫描当前文档层的
  旧命名（`executor::` / `<executor/`）、旧 CMake 用法、0.6.0 已删除
  API 与失效相对链接；历史快照（横幅标记）与 `docs/archive/` 豁免。
  修复了扫描发现的 8 处真实漂移（README 失效链接、API.md 相对路径、
  design/performance 文档旧 API 名等）。

### 测试与基准

- 新增 `tests/test_scheduling_contract_v061.cpp`（独立验证代理编写）：
  结构化决策/指标/反馈断言、set_scheduler 时序与所有权、shutdown
  竞争、高并发 route()、deadline 边界与同时 deadline EDF 稳定性、
  capability 缺失退化、旧式拒绝归一化。
- 新增 `tests/benchmark_scheduling_paths.cpp`：submit_auto 裸/全
  spec/feedback 包装、EDF 堆操作、DefaultScheduler route 纯策略 vs
  能力快照路径、能力快照采集的调度路径基线。

---

## [0.6.0] - 2026-10-03

0.6.0 是破坏性变更窗口，包含三部分：项目更名（Executor → kairo）、历史
兼容层全面清理、以及 0.6.0 的核心主题 **Scheduling Runtime**——调度器与
执行器解耦，并建立统一的 deadline / QoS / affinity / resource 任务调度
模型。设计文档：`docs/design/scheduling_runtime.md`；迁移指南：
`docs/MIGRATION.md` 0.6.0 节。

### 项目更名（Executor → kairo）

项目边界已超出"执行器"单一概念，更名为 kairo；executor 保留为领域概念
（执行后端类名 `Executor`、`ThreadPoolExecutor` 等不变，挂于
`kairo::` 命名空间）：

- 命名空间 `executor::` → `kairo::`；include 路径 `<executor/...>` →
  `<kairo/...>`（目录 `include/kairo/`）。
- CMake：`find_package(kairo)`、target `kairo::kairo`、产物 `libkairo`、
  选项/宏 `EXECUTOR_*` → `KAIRO_*`。
- 打包 `libkairo`（deb/prefab/Windows zip）、CI artifact/release 命名、
  README/website/skill 品牌同步；项目图标更换为 `docs/kairo.png`。

### 兼容层清理（breaking）

历史版本为迁移期保留的弱 API 全部移除，可诊断的 Result 版本接管主名：

- 7 对 facade API：`initialize_ex → initialize`（ExecutorResult）、
  `wait_for_completion_ex → wait_for_completion`（WaitResult）、
  `register/start_realtime_task_ex`、`register/start_blocking_io_worker_ex`、
  `register_gpu_executor_ex` 全部去后缀；旧 bool/void 版本删除。
- `IRealtimeExecutor::push_task()` 从 void 改为返回 ExecutorResult
  （0.2.2 P-001 的 ABI 兼容约束解除）；`push_task_ex()` 删除。
- 定时器字符串 ID 体系删除：`submit_delayed` 返回 `TimerSubmission`、
  `submit_periodic` 返回 `TimerHandle`、`cancel_task(task_id)` 由
  `TimerHandle::cancel()` 取代；`_with_handle` 拼写随之消失。句柄版取消
  后记录保留为 Cancelled 终态（`is_running=false`，仍可查询），二次取消
  返回 `AlreadyCancelled`。
- 4 参 legacy `submit_auto`、comm `is_lock_free()` 别名、
  `RealtimeExecutorStatus::memory_locked` 兼容字段删除。
- 保留：`StopToken`/`JThread` 平台兼容层、`submit_with_handle`/
  `submit_after_with_handle`（TaskHandle 任务图族）。

### Scheduling Runtime（核心）

- **IScheduler 解耦**：路由/准入决策从 facade 内联代码移入可注入的
  `IScheduler`（`Executor::set_scheduler()`），默认实现 `DefaultScheduler`
  组合意图路由（TaskRouter）与调度模型约束。调度器只产出决策，投递仍经
  既有后端协议执行；热路径不变式（admission 先于 future、queued 诊断
  先于 enqueue、CR-106 惰性能力采集、PA-2 驻停代次唤醒）不回退。
- **deadline 模型**：`TaskBuilder::deadline()` 从纯诊断升级为真实调度
  输入——同优先级内 EDF 排序；提交时已过期明确拒绝；开始执行时已错过
  记录 `FailureKind::DeadlineMissed`（`deadline_missed_count`），契约
  保持"取消是请求不是中断"，错过的任务仍执行。
- **QoS 模型**：`QosClass{BestEffort, Standard, Interactive, Critical}`
  在未显式设置 priority 时映射默认排队优先级；QoS 定位为排队优先级
  preset，不提供抢占、延迟界或实时性保证（确定性周期使用
  RealtimeQueue 意图 + 专用实时线程）；严格优先级无 aging 的 CR-024
  契约文档化（BestEffort 可被饿死，防饿死在应用层拆分）。
- **affinity 模型**：per-task advisory 亲和提示，与后端绑核集合不相交时
  在路由决策 detail 给出 `AffinityMismatch` 警告（不拒绝、不重绑线程）。
- **resource 模型**：声明式 `gpu_device`/`memory_bytes` 与能力快照核对，
  不满足时以 `BackendUnavailable`/`CapacityPressure` 拒绝而非隐式降级；
  `ExecutorCapability` 新增绑核集合与 GPU 设备/内存维度。
- 新增公开头：`<kairo/scheduling.hpp>`、`<kairo/scheduler.hpp>`；测试
  `test_scheduling_runtime` 32 项行为用例（EDF 排序、准入/错过、QoS 映射、
  affinity 诊断、resource 拒绝矩阵、调度器注入）。

---

## [0.5.3] - 2026-10-02

0.5.3 是稳定性与性能维护版本，公开 API 签名与既有语义保持兼容。主线是
2026-09-30 全量代码评审（`docs/CODE_REVIEW_2026-09-30.md`）四个阶段的落地：
P0 内存安全/挂死/数据竞争 9 项、P1 功能正确性 24 项、构建/打包 8 项、
热路径性能 10 项；随后定时器线程从 1kHz 轮询改造为事件驱动条件等待
（等待 CPU 降约 34 倍，periodic 抖动改善 10-27 倍）。每项修复均先以独立
复现测试确证再修复，并经独立验证通道复验。

### 修复与改进

**代码评审 Phase 1（9 项 P0：内存安全/挂死/数据竞争，PR #202）**：

- **单例退出 UAF（CR-001）**：`~Executor` 在单例模式下经 `shutdown(true)`
  排空默认执行器——在途 tracked 任务此前可触碰静态析构期已销毁的 facade
  成员（实测 SEGV）。
- **before_publish hook 数据竞争（CR-004）**：`LockFreeTaskExecutor` 钩子
  字段的普通写被生产者线程无同步读取（TSAN 2 处 data race + 空指针调用
  SEGV），改为原子不可变快照指针、trampoline 单次加载。
- **参数绑定异常泄漏注册槽（CR-010/011）**：注册后绑定参数抛异常会永久
  泄漏取消注册表槽位、admission 计数与任务图节点（65536 次失败即耗尽
  注册表、任务图无界增长），两条 catch 路径均改为完整终态结算；非默认
  构造/不可赋值参数类型与 `reference_wrapper` 解包语义经 API 兼容探针
  保持不变。
- **SerialExecutionContext 生命周期 UAF（CR-012）**：`submit_on` 闭包改持
  `shared_ptr<Shared>`（pimpl + detach），上下文先于派发销毁不再使 worker
  永久阻塞于已释放互斥量；detached 发布以 `ExecutorStopping` 结算。
- **monitor 回调抛异常挂死 future（CR-020）**：任务体启动前 monitor 回调
  抛异常导致 `future::get()` 永久挂起，`execute_task` 的 catch 对未启动
  任务触发 `on_timeout` 结算。
- **初始化失败后提交挂死（CR-022）**：初始化失败/回滚后接受的提交永不
  执行（future 挂死），`task_timeout_ms` 锁外读取与 `initialize()` 竞争。
  三条 submit 路径改锁内检查 `initialized_`，超时配置锁内读取。
- **CUDA 启动失败队列滞留（CR-002）**：`start()` 中途失败留下未消费任务，
  后续 `stop()` 在 `wait_for_completion` 永久挂起；失败路径以
  `set_exception` 排空队列，未启动 worker 时排空谓词直接退出。
- **OpenCL 重启泄漏（CR-003）**：restart 覆盖 `context_`（每次重启泄漏）
  并无界追加 `queues_`；重初始化先回收上一代（存活队列逐一 `clFinish`）
  再清理。
- **环检测递归爆栈（CR-052）**：任务图环检测 DFS 持写锁递归，约 4k 深度
  即 SIGSEGV（256KB 栈）；改为显式栈迭代 DFS。

**代码评审 Phase 2（24 项 P1 功能正确性，PR #203）**，要点：

- **KeepLatest/DropOldest 竞争下丢最新值（CR-040）**：位移路径有界重试，
  2s 观测 20999 次虚假拒绝 → 0。
- **worker park 丢唤醒（CR-005）**：park 改 `fetch_or`；旧代码可把已入队
  任务搁置到下一次 push（秒级 P99 尖刺），修复后稳定。
- **GPU 内存池对齐破坏（CR-060）**：块头部不再破坏 256B 对齐（float4
  访问出错）。
- **GPU 完成集无界增长（CR-062）**：+72MB/百万任务 → +0.2MB；
  `remove_task` 不再留下可运行的依赖者。
- **GPU optimizer 配置数据竞争（CR-063）**：TSAN 10/10/9 处报告 → 0。
- 其余项覆盖线程池、通信原语、GPU 后端、监控与平台工具，全表见评审
  文档 "Phase 2 修复执行记录"。

**诊断顺序与 CI 稳定性（PR #204）**：

- **queued 诊断先于 enqueue（NN-12）**：三条 submit 路径此前在 enqueue +
  唤醒后锁外补记 `record_task_queued`，worker 可在补记前取走任务执行，
  诊断条目以 Queued 状态在任务已运行时创建，快照 `in_flight_state_counts`
  缺 Running 键导致 `map::at` abort（注入窗口 10/10 重现）。现移入
  `mutex_` 临界区、enqueue 之前，建立 queued→start→complete 严格顺序，
  顺带消除 complete 先于补记时诊断表滞留僵尸 Queued 条目；锁序
  pool.mutex_ → monitor.mutex_ 保持单向。
- **测试加固**：快照测试改 `wait_in_flight_drained()`（future-vs-cleanup
  竞态，满载排水 p99 3.0ms、26 倍裕量）；偷取测试竞争窗口改"批次耗尽即
  收尾 + 墙钟预算"。

### 性能

**热路径优化（评审 Phase 4，10 项，PR #206；结论均以同参 Release 多轮
中位数口径测量）**：

- **路由惰性能力采集（CR-106）**：策略路径不再锁 5 把注册表，观测写入加
  快速开关，无回调时移动入库；`submit_auto` vs `submit` 中位差距
  +19.9% → +11.6%。
- **完成通知等待者门控（CR-108）**：无等待者时每任务 2 次 `notify_all`
  全部消失。
- **等待轮询通知化（CR-109）**：`try_wait_for_completion` 从 10ms 盲轮询
  （300s = 3 万次全量扫队列）改为通知驱动谓词等待 + 50ms 兜底分片。
- **提交路径单次分配（CR-110）**：`submit` 的 promise+ready 双包装合并
  单次堆分配。
- **派发器锁分配消除（CR-112）**：三处 `unique_ptr<shared_lock>` 改
  `std::optional`，每次 dispatch 免一次堆分配。
- **lockfree 队列 hook 快速路径（CR-121）+ 批量便签（CR-122）**：常态推送
  零 shared_ptr 原子读（旧 libstdc++ 走内部锁池）；批量路径 thread_local
  便签免反复分配。
- **GPU 参数缓存 O(1) 侵入式 LRU（CR-151）**：替换 O(n) 扫描；更新已有
  key 不再误逐热点。
- **测量口径更正与不改语义决定（CR-102/CR-135）**：旧基准为 O2 TU 链 O0
  库的混合优化口径，评审"基线 65-70%"系该产物（同参 Release 真实差距约
  +20% 内）；retention 高水位摊销被精确上界契约测试否决，扫描深度插桩
  证明恒为 1，语义不变。

**定时器事件驱动改造（PR #207，CR-135 后续）**：

- **1kHz 轮询 → 条件变量等待**：schedule/periodic/reschedule/stop 持锁
  bump `schedule_epoch_` 并 notify，调度线程精确睡到堆顶 deadline（堆空
  100ms 兜底上界）。等待 CPU 从恒定 0.7-1.0% 单核降至 0.024%（约 34 倍，
  收益主体是能耗）。显式 `wait_until(system_clock)` 规避 GCC ≥ 10 的
  `wait_for(duration)`/`wait_until(steady_clock)` 均映射到
  `pthread_cond_clockwait`、gcc-11 libtsan 不拦截（PR101978）的误报缺口；
  CI TSAN job（g++-11，即约束工具链）补入 `test_timer_handle` 作为权威
  门禁。取消语义不变：`request_cancel` 仍不唤醒（stale-entry lazy-deletion
  契约）。
- **periodic 网格锚定**：`next = 上一 deadline + P`（错过不追补）取代
  `now + P`——旧锚定把唤醒过冲逐周期累积成漂移，旧 1ms 轮询的网格量化
  恰好掩盖了它。periodic 抖动 avg 569-642µs → -47~+55µs（改善 10-27 倍）。

### 构建与打包（评审 Phase 3，8 项，PR #205）

- **覆盖率插桩真正生效（CR-080）**：库插桩改 `executor_apply_coverage_to_target()`
  函数式显式调用（旧版 include 期 `if(TARGET executor)` 恒假，库从未产出
  覆盖数据、报告静默失真）；插桩库的消费方经 INTERFACE 链接选项继承
  `--coverage`，独立测试目标不再缺 gcov 运行时。
- **版本单一来源（CR-085）**：6 个打包脚本默认值改从 `project(VERSION)`
  提取；新增生成的 `executor/version.hpp`（构建树 BUILD_INTERFACE 与
  install 双路可达）。
- **Sanitizer 配置治理（CR-083）**：Sanitizers.cmake 无条件 include；TSAN
  独立于 `EXECUTOR_ENABLE_SANITIZERS` 总开关（CI Release+TSAN job 路径
  保留）；ASAN/TSAN 显式同开在配置期 FATAL_ERROR。
- **安装树完整性（CR-084）**：`executor/util/` 纳入安装；GPU 后端头 6 处
  源码树相对包含（安装树必断链）改 `<executor/...>`；新增
  `install_headers_smoke` ctest（临时 prefix 安装 + 全部已安装头探针编译）。
- **Windows/共享库守卫（CR-081/082/086）**：共享库守卫扩到
  `MSVC OR (WIN32 AND NOT MINGW)`（覆盖 clang 工具链）；package_windows.ps1
  撤销多余 `Split-Path`（包内文档静默缺失）；build_windows.ps1 开关透传
  （旧版硬编码 OFF 无视入参）。
- **ODR 违例结构性消除（NN-01）**：ThreadPool 测试钩子无条件参与类布局，
  `EXECUTOR_THREAD_POOL_TEST_HOOKS` 不再影响布局——混合宏 TU ↔ 库的
  ASAN stack-buffer-overflow 根因消除。

### 文档

- 新增 `docs/CODE_REVIEW_2026-09-30.md`：全量代码评审与四阶段修复执行
  记录——每项缺陷的复现证据、修复方案与复验结论；含三项评审前提被测试
  推翻（CR-051/CR-104/CR-102）与一项被测量口径更正（CR-106）的记录。

---

## [0.5.2] - 2026-09-28

0.5.2 以 dependency-driven scheduling 为主线：`submit_after` 的依赖等待从
"任务立即入队、wrapper 在 worker 上等待条件变量"演进为调度侧唤醒——依赖
未就绪的任务驻留任务图节点、不占用 worker，依赖终态定向级联出队；补全
parked 超时（提交即起算）与 shutdown 终局结算语义，退役 `task_graph_cv_`
及其惊群唤醒，新增监控 Queued 补记与诊断接口。既有公开提交 API 兼容；
闭包墓地诊断接口 `closure_graveyard_size()` 为唯一新增公开方法。

### 新增

- **dependency-driven scheduling 第三阶段（PR-3，v0.5.2 主线收口，设计见
  `docs/design/dependency_driven_scheduling.md`）**：
  - **`task_graph_cv_` 退役**：依赖图任务运行时不再有任何依赖检查与
    条件变量等待（PR-1 起 parked 任务仅在依赖全部成功后入队，终态不可逆），
    4 处 `notify_all` 惊群唤醒全部删除——每 terminal 唤醒从 O(等待者)
    降为 O(1) 定向入队。
  - **监控 Queued 补记**：parked 任务出队入执行器时补记 `Queued`
    生命周期，观测链完整为 DependencyBlocked → Queued → Running。
  - **closure 墓地观测与终局清空**：新增 `closure_graveyard_size()`
    诊断接口；shutdown 终局清空墓地释放常驻引用。
  - **PA-6 验收基准**：新增 `tests/benchmark_task_graph_paths`
    （tracked 提交吞吐 1/2/4/8 生产者、parked fan-out 释放延迟、依赖链
    每跳成本、墓地规模）；PA-6 数据回填 performance_audit 台账，
    "分桶锁"备选确认不再需要。
- **dependency-driven scheduling 第二阶段（PR-2，v0.5.2 主线，设计见
  `docs/design/dependency_driven_scheduling.md`）**：
  - **D1 parked 超时**：queued soft timeout 自提交时刻起算，parked 期间
    经 facade 一次性定时器（时长取 `ExecutorConfig::task_timeout_ms`，
    0 = 不启用维持既有行为）触发，与池队列计时器经 phase CAS 仲裁恰好
    一个赢家；parked 获胜即失败结算并级联下游。定时器线程按需启动
    （全新 facade 无需先提交 delayed/periodic 预热）；输家/赢家闭包的
    promise/state 捕获转入 facade 墓地延迟析构，消除定时器线程与消费者
    使用异常对象之间的析构竞争（TSAN 实证）；`record_task_timeout`
    先于自身 future 结算（统计可见性与级联顺序同一不变式）。
  - **D2 shutdown 终局结算**：shutdown 返回前对全部仍 parked（依赖永不
    就绪）的节点统一失败结算，future 不悬空、admission/registry/in-flight
    全部释放；serial 派发被池丢弃时 TicketGuard 同步补图终态，parked
    下游即时级联。
  - **serial dispatch 结算顺序对齐**：submit_on 全部终态路径 drain bag 化
    （on_timeout/成功/协作取消/失败/context 拒绝），失败统计先于级联
    写入——与 tracked 路径同一观测不变式。
  - retention 交互与 admission 四路径（依赖失败/超时/取消/shutdown）
    恰好一次释放的验收测试。
- **dependency-driven scheduling 第一阶段（PR-1，v0.5.2 主线，设计见
  `docs/design/dependency_driven_scheduling.md`）**：`submit_after` /
  `submit_after_with_handle` 从"任务立即入队、wrapper 在 worker 上等
  条件变量"演进为"依赖未就绪不入队"——parked 载荷驻留任务图节点
  （提交时定格 wrapper/priority/执行器快照），依赖终态经
  `resolve_task_graph_dependents_locked` 级联：全部成功按原 priority
  调度侧出队，任一失败即时结算依赖异常（任务不占 worker）。
  worker 占用与宽依赖饿死/挂死窗口消除（新增免饿死回归测试，旧实现
  在该场景挂死）；`unmet_count` 驱动定向出队，替代对全部等待者的
  `notify_all` 惊群（条件变量本体按计划 PR-3 退役）。

### 修复与改进

- **facade 析构与孤池 worker 的 UAF 闭环（既有窗口，PR-1 时序使其暴露）**：
  `stop(false)` 将池交给 detached 终结线程后，manager 撤下句柄，后续
  `shutdown(true)`（含 `~Executor` 路径）此前会完全跳过等待，孤池 worker
  收尾（failure 面板/monitor/registry 写入）可与 facade/manager 析构并发
  （TSAN 实证 heap-use-after-free）。现改为 retired 保活 + 终局排空：
  executor 退休池存入 `retired_pools_`、manager 停机执行器存入
  `retired_async_executors_`，任何 `shutdown(true)` 先等退休池 worker 全部
  join 再放行析构；`stop(false)` 立即返回契约不变。
- **失败统计先于依赖级联**：tracked 任务异常路径中 `record_task_exception`
  提前到级联结算之前——dependent future 就绪时失败面板计数保证可见
  （满载下 8/20 观测窗口，修复后 0/40）。

### 文档

- **待办账实清理（v0.5.x 阶段 21 第一项）**：对 `docs/todolists/` 15 份计划文档
  逐项审计核实（约 326 个未勾项 → 186，且剩余均为真遗留、显式门控（⏸）或
  可重跑模板）。要点：主清单阶段 20 P1/P2 回填勾选（随 0.5.0 合入）并新增
  阶段 21（dependency-driven scheduling，承接 performance_audit PA-6 的
  调度侧唤醒深化）；lockfree_queue_optimization §6.2/§6.3 三处失实勾选勘误；
  gpu_todolist 约 70 项回填勾选，§3.5 "pinned memory / optimizer" 两处已勾项
  经审计证伪加勘误注记（对应 PA-30/PA-31）；android/client/mira 三份反馈计划
  约 26 项漏勾回填、已决待决项回填，外部门控项（big.LITTLE 真机、heyaki
  M6/M7、Mira 回填）显式标注；发布前人工核对项迁入 `docs/RELEASE_CHECKLIST.md`；
  android 两项已定决策回写 `docs/design/android_port.md` 待决项；修复
  `website/.vitepress/config.mjs` 中英导航版本标签滞后（v0.4.0 → v0.5.0）。

### 修复与改进

- **#194 benchmark_thread_pool_hotpath harness 数据竞争修复**：Phase 2 延迟采样的
  任务闭包原先并发 `push_back` 同一 per-producer `std::vector`（同一 producer 的
  任务可被任意 worker 执行），TSAN 构建下确定性报 data race 并因 vector 内部状态
  损坏挂死（30s ctest 超时，重跑仍不终止）。现改为每样本一个预分配槽位，由
  (producer, sample index) 唯一寻址：任务闭包只写自己独占的槽，采样路径零共享
  容器写入；槽位在 `shutdown(true)` join 全部 worker 后单线程读取。修复后 TSAN
  构建对该 benchmark 0 warning；Phase 1 吞吐压测的插桩减速（~20x，实测 220-276s）
  通过 TSAN 构建下单独放宽 ctest 超时到 600s 收敛（库代码无涉）。
- **#187 残余窗口封口（批量预留误取消）**：v0.5.0 的倒序预留 + BatchWriting
  逃逸把误取消窗口缩到极窄，但在 2-vCPU CI runner 的 gcc-11 Release TSAN 下，
  写入循环中段仍可被 scan-ahead 消费者在生产者线程被抢占期间按 64-yield 预算
  误判停滞（`test_batch_integration` 间歇失败）。消费者侧取消预算现按认领批量
  规模缩放（`reservation_wait_yields_ × min(claimed, 1024)`，claimed 即
  `enqueue_pos_` 单次 CAS 认领的槽数）：健康批量生产者不再可能被误杀；真停滞
  恢复延迟按批量线性、有界（最坏 64×1024 次 yield，亚秒级），停滞契约测试
  （小批量 + hook 阻塞）不受影响。

---

## [0.5.0] - 2026-09-13

0.5.0 以任务生命周期语义与跨平台落地为主线：facade 新增任务级协作取消与可取消、
可重排的定时句柄，串行执行上下文与总量有界 admission 转正；Android 适配一期完成
CPU-only 交叉编译与 ARM64 验证；2026-09 性能审查的 P1（线程池提交热路径）与
P2（无锁组件兑现）两阶段重构合入，提交吞吐与 RT jitter 显著改善；停机/生命周期
竞态（P-001~P-008）与 TSAN 可见的数据竞争面收敛。既有主要公开调用方式保持兼容；
`ExecutorSnapshot` schema 2 → 3（纯新增字段）。

任务协作取消与定时句柄（客户端反馈台账 P1-2/P1-3 收敛，设计见
`docs/design/task_cancellation_and_timers.md`）：facade 新增任务级协作取消与可取消、
可重排的定时句柄；取消是协作请求而非抢占，取消计数进入独立生命周期字段而非
failure 体系。

串行执行上下文与总量有界 admission（Mira 台账 EXE-20260830-001/002/003 收敛，设计见
`docs/design/serial_execution_context.md`、`docs/design/bounded_admission.md`）：
`submit_on`/`submit_on_with_handle` 的 facade wrapper 重构为非阻塞共享状态；
`max_in_flight_tasks` 为默认异步提交提供跨 scheduler 与本地队列的总量上限。

### 新增

- **任务级协作取消（C1）**：新增 `include/executor/task_cancellation.hpp`
  （`TaskCancelled` / `TaskCancellationReason` / `TaskCancellationResponse` /
  `CancellationStatus`）。`submit_cancellable` / `submit_cancellable_priority` /
  `submit_cancellable_after` 把 `StopToken` 注入为 callable 首参数；
  `request_task_cancel(const TaskHandle&)` 提供排队取消与运行中协作请求，
  幂等且不写 failure 事件；`submit_with_handle` / `submit_after_with_handle`
  天然获得排队取消能力；取消 registry 有界（默认 65536，
  `set_cancellation_registry_capacity()` 可调），容量耗尽明确拒绝。
- **定时句柄（T1）**：新增 `include/executor/timer.hpp`（`TimerHandle` /
  `ScopedTimerHandle` / `TimerStatus` / `TimerOperationResult`）。
  `submit_delayed_with_handle` / `submit_delayed_cancellable_with_handle` /
  `submit_periodic_with_handle` / `submit_periodic_cancellable_with_handle`
  提供取消、重排与状态查询；内部定时器改为 registry + generation heap，
  变更 1ms 内可见的 steady 时钟分片等待（5ms 延迟任务的平均到期误差从约 5.5ms
  降至约 0.9ms；不用 condition_variable 定时等待，规避 gcc-11 libtsan 对
  pthread_cond_clockwait 未拦截导致的 double-lock 误报），
  stale entry 有界压缩；终态 record 只保留有界元数据。
- **定时器互操作指南（S1）**：新增 `docs/external_event_loop_interop.md` 与可编译
  示例 `examples/event_loop_interop.cpp`（托管事件循环、strand 延续盲区纪律、
  PhaseGate 批次收尾）；中英文网站同步上线指南页。
- **监控扩展**：`ExecutorSnapshot` schema 2 → 3，新增 `cancellation`
  （`CancellationStatus`）与 `timers`（`TimerStatusSummary`）独立字段与快照文本行；
  `Executor::get_cancellation_status()` / `get_timer_status_summary()` 查询入口。
- **教程与测试**：新增教程 `examples/tutorial/13_cancellation_and_timers.cpp`、
  `tests/test_task_cancellation.cpp`、`tests/test_timer_handle.cpp`、文档一致性测试
  `tests/test_api_doc_cancellation_fields.cpp`，以及整库
  `EXECUTOR_STOP_TOKEN_FORCE_FALLBACK` 强制实例化的
  `tests/test_task_cancellation_fallback.cpp`。
- **comm 指引（G1 轻量项）**：中英文"如何选择通信组件"指南新增
  "什么时候允许裸回调"一节，明确裸 `std::function` 回调的适用边界。
- **串行执行上下文（S2）**：新增 `include/executor/serial_execution_context.hpp`
  （`SerialExecutionContext`，`post`/`reserve`/`post_reserved`/`abandon`/`shutdown`，
  FIFO ticket 语义）。`Executor::submit_on` / `submit_on_with_handle` 在其上提供
  严格按提交顺序结算的 `std::future<T>` / `TimerSubmission<T>`；facade wrapper 为
  非阻塞共享状态实现（派发/结算分离），多 worker 下不再互相饥饿（两 worker ×
  10,000 突发约 1s 内全部结算），外部事件循环互操作见
  `docs/external_event_loop_interop.md`。
- **总量有界 admission（A1）**：`ExecutorConfig::max_in_flight_tasks`（默认 `0` =
  不启用、零热路径开销）为 facade 默认异步提交（普通 / priority / tracked /
  cancellable / batch / `submit_on*`）提供总在途上限；达到上限时提交不抛出，
  future 立即以 `CapacityExhaustedException` 就绪，`FailureKind::CapacityExhausted`
  事件与 `capacity_exhausted_count` 计数可观察；运行期 `set_max_in_flight_tasks()` /
  `get_in_flight_submissions()` / `get_max_in_flight_tasks()` 可调可查。

### 性能

- **无锁组件兑现与 RT 优先级反转消除（性能审查 PA-5/7/8，2026-09 收敛计划
  阶段 P2）**：`ObjectPool` 改 tagged Treiber 索引 freelist（`(tag:32 |
  index:32)` 单字 head，ABA 由 tag 封闭；消费者 `release_bulk` 整链单 CAS
  splice；双重释放检测改为 per-node state CAS，语义不变）——
  `LockFreeTaskExecutor` 提交路径与 RT 线程逐任务 `release` 不再共享互斥锁，
  RT 路径优先级反转面消除；`LockFreeQueue` 消费侧 MPMC 化（前沿连续
  Published run 单 CAS 认领），`LockFreeWorkerQueue` 的 pop/steal/size 去
  `consume_mx_`（steal 与 pop 同为 FIFO 最老端，恰好一次交付），`size()`
  改无锁近似值；LockFree worker 空闲从 1µs-sleep 永久轮询（约 10⁶
  syscall/s/核）改为「PAUSE → yield → 10µs-sleep 缓冲带 → futex 驻停」，
  驻停编码进 32 位 `wake_seq_`（bit0=驻停位 + 唤醒计数，生产者 push 后
  无条件 `fetch_add(2)`、返回值带驻停位才 `notify_one`，忙碌路径零
  syscall），空闲 executor 进程 CPU 实测 0.0ms/500ms。实现期修复两个
  深挖出的并发陷阱（条件唤醒标志被 StoreLoad 重排打穿致挂死；CAS 循环
  带陈旧期望值重试致偶发线程饿死 20 秒+）。基准（同机同条件 3 次取中位）：
  mpsc 吞吐 1P +86%、8P +289%、16P +482%、32P +399%；RT 执行器提交延迟
  -21%；RT 1ms 周期 jitter p50 78.7→16.8µs；已知取舍：2-4 生产者 mpsc
  吞吐 -23%~-24%（Treiber 单链低生产者数下的缓存行往返，详见计划文档
  P2 小节归因实验与后续条目）。新增 `test_object_pool_lockfree_stress`
  （多生产者锤击 + RT 延迟分布 + 空闲 CPU 守护 + 驻停唤醒守护）。设计
  推导与基准全表见
  `docs/todolists/performance_audit_2026-09_plan.md` 阶段 P2 小节。
- **线程池提交热路径重构（性能审查 PA-1/2/3/4、PA-9/13/17，2026-09 收敛计划
  阶段 P1）**：worker 空闲驻停从「全局 `mutex_` + `condition_` 重谓词（谓词内做
  steal/dequeue，含堆分配与排序）」改为 32 位驻停代次计数 + C++20
  `std::atomic::wait`（Linux futex 直达）；提交路径单任务 `notify_one` +
  worker 接力唤醒（并行度指数恢复，惊群消除），批次 `notify_all`；Task 全链路
  （enqueue → dequeue → dispatch → 本地队列 → pop）改 `unique_ptr` 所有权/字段级
  移动，消除逐跳 std::function/string/vector 复制；`dispatch_batch` 消除每次
  派发的 4-6 次堆分配（成员便签 + 计数排序分段）；`local_queues_` 去除
  libstdc++ `atomic_load(shared_ptr*)` 库级自旋锁（改为读写锁保护下的裸指针
  快照）；steal victim 选择无分配化（`LoadBalancer::highest_load_victim`）；
  `should_exit` 原子空集快速路径。停机正确性同步加固：worker 的 stop_ 退出加
  「退出守门」（持 dispatcher 锁复核代次），排除任务处于搬运途中被永久滞留的
  窗口。基准（14 核桌面，gcc 13 Release，3 次取中位）：单生产者提交吞吐
  125.5k → 565.4k tasks/s（4.5x），e2e 3.5x，多生产者（2-32 线程）2.2-4.7x
  且扩展曲线平坦化，直连 ThreadPool 的单生产者吞吐 13.5x、唤醒 p99（4T+）
  3-5x 改善。新增 `benchmark_thread_pool_hotpath`（多生产者争用吞吐 +
  wake-to-exec 延迟分布）。设计说明（32 位 futex 前置条件、代次采样时机、
  接力唤醒与退出守门的推导）见
  `docs/todolists/performance_audit_2026-09_plan.md` 阶段 P1 小节。

### 修复与改进

- **P-001/P-002（停机竞态）**：`LockFreeTaskExecutor` 与 `RealtimeThreadExecutor`
  以单原子准入门闩消除停机与提交交错的生命周期竞态（PR #180）；worker 线程内
  自停止语义与 `stop_and_join()` 返回值契约同步澄清。
- **P-003（LockFree 池容量）**：对象池按取整后的环容量统一分配——请求容量 5
  （环取整为 8）时可用槽位 7，池与 `push_tasks_batch` 上限一致，非 2 的幂配置
  不再让 `failed_pushes` / `queue_full_rejections` 失真。
- **P-004（进程内存锁租约）**：`util::ProcessMemoryLockLease` 引用计数管理
  `mlockall`/`munlockall`——**行为变化**：最后一个持有租约的实时执行器停止时
  才解除进程锁；与进程内其他 `mlockall` 使用方共存更安全。
- **P-008（Windows 处理器组）**：`cpu_affinity` 支持超过 64 逻辑 CPU 的
  处理器组编号（`g*64+序号`），跨组配置明确拒绝（PR #181）。
- **TSAN 收尾（issue #185–#188）**：`LockFreeQueue::push_batch_exact` 的批量预留
  会被消费者"停滞生产者恢复"启发式误取消（TSAN 构建下空环确定性失败）——预留改
  倒序使前沿槽可取消窗口缩至 O(1)，`cancel_reservation` 增加 BatchWriting 逃逸，
  停滞恢复契约保持不变；`~TaskMonitor` 析构先取 `mutex_` 再成员析构，与最后一个
  持锁读者建立 happens-before（消除 shutdown/析构路径 TSAN 报告与潜在 UAF）；
  `test_lockfree_mpsc` 测试自身同步域修复；benchmark 测试 `RUN_SERIAL` +
  sanitizer 构建下延迟断言豁免；TSAN CI 子集补 `test_lockfree_mpsc` /
  `test_batch_integration` / `test_executor_manager`。
- **P-006/P-007（可观察性）**：`ThreadPool::get_status()` 空闲线程数饱和语义
  与 resizer 竞态修复；GPU `validate_memory_range` 拒绝外部缓冲区，无锁队列
  `size()`/`empty()` 近似语义钉住回归测试。
- **测试与 CI**：admission/serial 独立测试 NDEBUG 安全化；慢 CI runner 上
  计数与 tick 轮询有界化；forced-fallback 目标从全源码清单构建。

### 变更

- shutdown 清理未到期 delayed 任务：future 异常由
  `std::runtime_error("Timer stopped...")` + `SubmitRejected` 事件改为
  `TaskCancelled(Shutdown)`，不再记录 failure 事件；可观察性转移到定时计数。
- `ExecutorSnapshot::schema_version` 2 → 3（纯新增字段）；解析快照文本的下游
  工具需按新 schema 更新（迁移说明见 `docs/MIGRATION.md`）。

### Android 适配一期

Android 适配一期：核心库可在 NDK 工具链下以 CPU-only 配置交叉编译为静态库/共享库，
并纳入官方模拟器与真实 ARM64 runner 的验证流程。Android 上的线程优先级、CPU 亲和性、
`mlockall` 与 timer slack 均保持 best-effort，不承诺硬实时；CUDA/OpenCL 不进入一期。

#### 新增

- **Android CPU-only 构建支持**：新增 `if(ANDROID)` CMake 平台分支，bionic 下不再错误
  链接 `librt`，也不导出 `libatomic`；Android 构建默认关闭 GPU/CUDA，用户仍可显式覆盖。
- **便携 StopToken/JThread 兼容层**：新增 `include/executor/stop_token.hpp`。桌面平台
  `executor::StopToken` 是 `std::stop_token` 别名，保持既有 override 源码与 ABI 兼容；
  Android libc++ 未启用 jthread 时使用自有 `StopSource` / `StopToken` / `detail::JThread`。
- **Android 线程与 affinity 默认值**：bionic 下使用 `sched_setaffinity` /
  `sched_getaffinity`；默认线程池上限为 4，自动 affinity 来自 cgroup 允许 cpuset；
  短周期实时线程不再自动申请 `SCHED_FIFO`。
- **Android 构建与设备脚本**：新增 `scripts/build_android.sh`、
  `scripts/run_android_tests.sh`、`scripts/capture_android_device_info.sh`，以及
  `tests/android_smoke.cpp` 等无 GTest standalone 测试。
- **Android CI**：新增 NDK r26c / r28b 交叉编译 workflow；新增手动触发的
  `arm64-concurrency` workflow，覆盖 4 核、单核 pinned、ASan/UBSan 和可配置 MPSC soak。
- **Android 打包与平台文档**：新增 `docs/PACKAGE_ANDROID.md`，覆盖 NDK CMake、AGP、
  `c++_shared` 打包、JNI shutdown 生命周期与 Prefab/AAR 模板；中英文网站首页、构建页
  与平台部署核对页同步补充 Android CPU-only 能力边界。

#### 修复与改进

- 修复 `test_multithread_mpsc` 在慢速 ARM64 模拟环境下消费者过早退出导致误报的测试逻辑。
- Android 下实时调优路径统一为 best-effort：priority / affinity / mlock / timer slack
  失败只写入状态字段，不改变任务接受结果。
- 为 Android 平台裁剪 NDK clang 不支持的 warning 选项，并守卫仅适用于 desktop Linux
  的 `/proc` 测试。
- **修复 P-260816-001**：`ExecutorManager::shutdown()` 不再在持有
  `default_async_mutex_` 时执行默认执行器的阻塞排空（`stop(wait_for_tasks)` /
  `wait_for_completion()` 含 worker join）。此前池内任务在排空期间再入
  `submit()` / 状态查询等持锁读路径会与 shutdown 互相等待形成自死锁；现在改为
  锁内置闩并快照执行器、锁外排空，置闩后读路径立即走拒绝分支，与 ThreadPool
  自身"先停止接收新任务，再等待已接受任务完成"的关停顺序对齐。并发 shutdown
  调用者通过新增的条件变量等待排空完成，保持"第二个调用者等第一个排空结束后
  才返回"的旧语义。新增回归测试 `tests/test_shutdown_drain_reentrancy.cpp`
  （旧实现在该测试下确定性死锁）。
- **修复 P-260816-002（H1）**：`Executor` 定时器线程启停竞态。旧实现先原子置位
  `timer_running_` 再创建线程并给 `timer_thread_` 赋值，并发 `shutdown` 会在成员尚未
  赋值时读取它（数据竞争 UB）并跳过 join；随后赋值出的 joinable 线程成员在析构时触发
  `std::terminate`，竞态窗口内提交的延迟任务 future 永久悬挂。现在：`timer_thread_` /
  `timer_state_` / 测试工厂由 `timer_thread_mutex_` 保护，赋值完成后才置位运行标志；
  每代线程持有独立的停止标志，停止只对本代置位（join 期间并发重启不会复活旧线程，
  join 必定返回）；join 在锁外执行。`submit_delayed` 在入队临界区内检查停止位，
  消除"入队后无人处理"的悬挂 future；`set_timer_thread_factory_for_test` 同步化。
  新增回归测试 `tests/test_timer_thread_lifecycle_race.cpp`（旧实现下延迟任务
  future 永久悬挂 / `std::terminate`，测试确定性命中）。

#### 验证

- 官方 Android 模拟器（API 30 x86_64，KVM）：6/6 standalone 测试通过。
- qemu-user + NDK bionic 静态 ARM64：6/6 测试通过。
- GitHub ARM64 runner（Neoverse-N2，4 核）：6/6、单核 pinned、ASan/UBSan、600 秒
  MPSC soak 均通过；结果见 `docs/performance/android_a3_validation.md`。
- big.LITTLE Android 真机验证已登记为发布前 gate，正式版本不得在未完成该项时宣称
  已在 big.LITTLE 设备验证。
- P-260816-001 修复验证：树内 Debug 构建 105/105 ctest 通过（除 benchmark 标签外）；
  关停/生命周期相关测试（`test_shutdown_drain_reentrancy`、`test_concurrent_stop_submit`、
  `test_thread_pool_self_shutdown` 等）在 ThreadSanitizer 下无警告；该回归测试已加入
  CI TSan 任务清单。
- P-260816-002 修复验证：树内 Debug 构建 106/106 ctest 通过（除 benchmark 标签外）；
  定时器相关测试（`test_timer_thread_lifecycle_race`、`test_periodic_failure_observability`、
  `test_realtime_timer_period_race`、`test_timer_period_guard`、`test_executor_facade`）在
  ThreadSanitizer 下无警告；该回归测试同样加入 CI TSan 任务清单。

---

## [0.4.0] - 2026-08-13

0.4.0 聚焦通信与并发执行路径的确定性边界：核心通信组件采用构造期固定存储和原子同步，新增进程内 Topic 扇出、LET 阶段通信、实时分配诊断及延迟分位数观测；任务图句柄保留和线程池真实扩缩容也获得明确的容量与并发语义。既有主要公开调用方式保持兼容，但“同步无锁”仅描述组件内部原子与固定存储，完整实时性仍须由调用方在目标环境验证。

### 新增

- **进程内 Topic / Subscription**：新增 `comm::Topic<T>` 与 move-only RAII `TopicSubscription<T>`，将订阅后的事件扇出到每个订阅者的独立有界 FIFO；发布结果报告匹配、成功和拒绝订阅数，逐订阅者保留独立 drop policy、统计、回调和关闭唤醒语义。该原语明确不提供网络传输、重放、可靠确认或硬实时保证。
- **LET 阶段通信契约**：`PhaseGate`、`DoubleBuffer` 与 `LatestMailbox` 新增可选的 phase-bound LET 模式。绑定后，发布仅发生在当前相位，读取仅暴露上一完成相位的数据；相位切换会拒绝活跃读写，避免跨周期读取或写入。
- **实时内存分配诊断**：新增 `comm::RealtimeAllocationGuard`、`RealtimeAllocationViolationPolicy` 和线程局部统计，可记录受保护实时路径中的分配次数、字节数、组件与阶段；Linux 构建可通过 `EXECUTOR_ENABLE_REALTIME_ALLOCATION_GUARD` 启用，`RealtimeThreadConfig::enable_allocation_guard` 控制周期回调的 opt-in 诊断。
- **通信延迟分位数**：`CommStats` 增加固定大小延迟直方图及近似 `p50_latency`、`p99_latency`，同时保留累计、平均和最大延迟统计。
- **有界任务图句柄保留**：`ExecutorConfig::task_graph_retention_capacity` 和对应运行时设置 API 控制终态 `TaskHandle` 的保留上限。被淘汰的句柄会明确拒绝为过期；仍被活跃依赖链引用的节点不会提前淘汰。
- **线程池真实扩缩容**：`ThreadPool::resize()` 与 `ThreadPoolResizer` 现创建或移除真实 worker，且仅接受初始化配置的线程数范围；缩容前迁移本地队列任务并 join 被移除 worker，返回时状态稳定。

### 修复与改进

- **通信同步核心无锁化**：`MpscChannel` / `RealtimeChannel` 改为构造期预分配的有界 MPSC 节点池，
  `LatestMailbox` / 未绑定 `DoubleBuffer` 改为固定 reader-pin 快照槽，`PhaseGate` / `Sequencer`
  改为原子状态核心；新增同步原子 lock-free 查询与构造期平台校验。该保证不覆盖 payload、callback、
  时钟、缺页或 OS 调度，`Topic` fan-out 仍属于 mutex 与动态分配支持的非实时控制面。
- **线程池扩缩容并发安全**：本地 worker 队列改以原子发布的 `shared_ptr` 快照访问，调度、窃取与 resize 通过读写锁协调，避免队列替换期间的悬空访问和 UAF；shutdown 与 resize 的 join 路径也已串行化。
- **任务调度边界**：移除 `TaskDispatcher` 的旧引用构造路径；空本地队列快照不会从调度器取走任务或发生越界访问。
- **实时契约实现边界**：LET 绑定要求固定双缓冲容量和不抛异常的复制语义；每个相位只允许一次发布，缺失上一相位数据或相位转换中读取会返回明确的通信错误。
- **兼容性**：调整 C++20 实现以兼容 GCC 10（项目仍建议使用 GCC 11 或更高版本）。

### 文档与测试

- 更新中英文 README、API、通信设计文档、教程站点与 sitemap，补充 LET 状态/相位、通信选择、延迟观测和失败可观测性示例说明。
- 新增 Topic fan-out/独立背压/并发生命周期、通信实时内存、LET `PhaseGate`、邮箱与双缓冲、任务图保留/过期语义的测试；扩展线程池扩缩容、调度 fallback 与并发 UAF 回归测试，并约束扩缩容压力用例的工作负载。
- 新增面向使用者的 `executor-integration` 渐进式接入指南，以及面向维护者的能力索引与维护参考。

---

## [0.3.1] - 2026-08-06

0.3.1 是统一 `Executor` Facade、完整生命周期监控与按意图自动路由的功能版本。除实时进程内存锁配置项外，它保留各执行模型真实的完成、接收和生命周期语义，而不将它们统一伪装为 `future`。

### 新增

- **任务意图与 CPU/GPU 双路径**：新增 `TaskOptions`、`TaskBuilder`、`ExecutionIntent`、`FallbackPolicy`、`cpu_gpu_task()` 和 `CpuGpuTask`。普通 `submit_auto(lambda)` 默认选择异步线程池；CPU/GPU 双路径仅在 GPU 可提交时使用 GPU，`AllowCpu` 才允许显式回退。
- **可解释路由与能力发现**：新增 `RoutingDecision`、routing callback、最近路由决策缓冲及 `get_executor_capabilities()`。路由说明与 failure event 分离：前者解释选择，后者报告实际拒绝或执行失败。
- **有界 dispatch**：新增 `DispatchResult` 和 `dispatch_auto()`。`LowLatency` 只投递到用户指定、运行中的无锁执行器；`RealtimeQueue` 只投递到指定、运行中的实时队列。返回值只表示队列接收，不表示任务完成。
- **无锁统一管理**：`ExecutorManager` 现注册、启动、停止并枚举 `LockFreeTaskExecutor`，跨异步、GPU、实时、Blocking I/O 和无锁后端保证名称唯一；关闭时先从无锁注册表摘除并停止。
- **Blocking I/O 统一控制面**：新增 `BlockingWorkerSpec`、`WorkerHandle` 和 `start_worker()`，封装注册、启动、状态查询及 stop/wakeup/join，同时保留 `IBlockingIoWorker` 的 stop token、启动超时和退出原因契约。
- **完整生命周期 Monitor**：新增 `ExecutorSnapshot`、`ExecutorLifecycleState`、`Executor::get_snapshot()` 和稳定文本导出，统一汇总生命周期、默认异步、Realtime、Blocking I/O、GPU、失败状态、最近失败事件、任务统计及聚合计数；snapshot 明确 `schema_version`、序号、采集时间、`partial` 和一致性说明。
- **故障现场诊断**：等待完成或 shutdown 超时、初始化/注册/启动失败路径可通过 snapshot callback 获取完整现场；诊断异常与业务执行、future、worker 和 shutdown 隔离。
- **有界在途任务诊断**：线程池和任务图支持 `Pending`、`Queued`、`Running`、`DependencyBlocked` 等生命周期状态，提供容量、采样率、状态计数、最老任务年龄和有限任务条目；容量溢出会计数并标记诊断不完整，不保存 callable、payload 或异常对象。
- **一致性校验与性能基线**：Manager 增加轻量 `state_epoch`，snapshot 采集前后最多重试两次，持续变化时标记 `epoch_changed`；idle initialized async 场景完成采集与文本格式化基线，epoch 校验不使用全局大锁。

### 测试与文档

- 新增自动路由阶段测试，覆盖默认路由、CPU 回退/拒绝、路由 callback 隔离、路由缓冲语义、无锁队列满、实时未启动/有界接收、Blocking worker 生命周期和能力枚举。
- 新增生命周期快照测试，覆盖未初始化不触发懒初始化、全部后端汇总、等待/关闭故障现场、in-flight 容量溢出、并发 shutdown，以及持续注册变化下的 epoch 有界重试和 partial 标记。
- `API.md`、`MIGRATION.md`、中英文 README 和 Blocking I/O 教程补充 API 选择表、结果语义、迁移路径及自动路由边界。
- `API.md`、生命周期 Monitor 设计文档和实施计划同步 snapshot 字段、best-effort/partial 语义、有限在途诊断、state epoch 和稳定文本导出说明。
- 新增 `examples/lifecycle_snapshot.cpp`，可在 CPU-only 构建中演示任务积压、任务失败、稳定文本导出和 shutdown 后的生命周期快照；该示例作为 CTest smoke test 运行。
- 新增生命周期快照性能基线，记录 idle initialized async 场景的采集/格式化耗时、格式化分配次数和输出字节数。

### 破坏性变更

- **实时进程内存锁配置**：`RealtimeThreadConfig::enable_memory_lock` 更名为 `enable_process_memory_lock`，并改为默认关闭，以明确其 Linux `mlockall` 的进程级语义。需要该能力的调用方必须改用新字段并显式设置为 `true`，同时检查 `RealtimeExecutorStatus::process_memory_lock_applied` 与 `process_memory_lock_errno`。

### 兼容性

- 除上述配置项外，`submit()`、`submit_gpu()`、实时和 Blocking I/O 的既有入口保持可用；生命周期 snapshot 为新增只读 API，既有单项状态和统计 API 无需迁移。
- legacy 四参数 CPU/GPU `submit_auto(TaskCharacteristics, name, kernel, config)` 在 `0.3.x` 保持既有“GPU 未就绪即失败、无隐式 CPU 回退”的行为，暂不添加编译期弃用标记。
- 带返回值的 CPU/GPU 自动任务、`ExecutionReport<T>` 和 legacy overload 的弃用/移除仅在后续允许破坏性变更的主版本评估。

---

## [0.3.0] - 2026-07-27

0.3.0 是面向跨线程通信、任务依赖编排和长期阻塞 I/O 生命周期管理的向后兼容功能版本。0.2.3 的公开 API 保持可用；新代码可逐步采用 `executor::comm`、任务图 facade 和 `BlockingIoExecutor`。

### 新增

- **通信与并发 facade**：新增安装头文件 `executor/comm.hpp` 及 `executor::comm` 命名空间，提供统一结果、错误码、统计与事件回调；公开 `MpscChannel<T>`、`SpscChannel<T>`、`LatestMailbox<T>`、`RealtimeChannel<T>`、`DoubleBuffer<T>` / `Snapshot<T>`、`PhaseGate` 和 `Sequencer`，覆盖有界消息流、最新值、实时周期 drain、一致快照和启动/顺序协调。
- **通信背压与可观测性**：channel 支持容量、超时、关闭和丢弃策略；通信组件可查询 `CommStats`，并可通过 `set_event_callback()` 获取低频事件。实时 channel 明确为有界、非等待 facade，周期内可设置 drain 预算。
- **任务图 facade**：`Executor` 新增 `TaskHandle`、`TaskSubmission<T>`、`submit_with_handle()`、`submit_after()`、`submit_after_with_handle()` 和 `when_all()`，用于在同一 `Executor` 实例内表达任务依赖与汇合；依赖失败、无效/跨实例 handle 与环路会以异常结果和 `SubmitRejected` 暴露。
- **阻塞 I/O worker**：新增 `IBlockingIoWorker`、`IBlockingIoExecutor`、`BlockingIoConfig` 与 `BlockingIoExecutorStatus`，并通过 `Executor` / `ExecutorManager` 提供注册、启动、停止和状态查询。worker 以 `run(std::stop_token)` + `wakeup()` 协作停止，适用于调用方持有的长期可中断 I/O 循环。
- **教程与用户网站**：新增中英文 VitePress 使用手册、完整教程示例和 GitHub Pages 部署/校验流程，覆盖任务提交、依赖、通信、实时控制、GPU、可观测性、部署和故障排查；新增阻塞 I/O worker 指南与教程示例。

### 修复

- **线程池和负载均衡并发安全**：修复 worker 队列丢失唤醒、完成排空、监控停止/异常生命周期、初始化失败回滚，以及动态扩缩容时 `LoadBalancer` 访问 worker 容器的数据竞争。
- **安全停止与生命周期**：worker 内调用 `stop()` 时安全转交 join；CUDA 并发停止串行化并修复重启后的 stopping state / waiter 注册；OpenCL 启动 CAS、建线程失败回滚及 stop/cleanup 与公开操作的生命周期互斥得到加固。
- **无锁执行器正确性**：修复 MPSC 槽位预留与发布导致的 head-of-line 阻塞、精确批量预留取消空洞、取消预留前的让步，以及 `LockFreeTaskExecutor::start()` 建线程失败后的回滚。
- **输入与诊断边界**：线程池拒绝空任务；`LockFreeQueue::backoff_multiplier` 增加边界校验；OpenCL kernel 异常写入 `last_error_message`；`submit_gpu()` 找不到执行器时记录 facade failure；明确 `dropped_task_count` 语义。
- **构建告警隔离**：CUDA/OpenCL 供应商头改为 `SYSTEM` include，严格告警仅应用于 executor 库目标，并修复 C++20 / 编译器告警问题。

### 测试 / CI

- **并发回归覆盖**：新增通信 facade、任务图、阻塞 I/O、线程池扩缩容、worker 自停止、CUDA 并发停止、OpenCL 生命周期和 MPSC 并发测试。
- **持续集成加固**：TSAN 与无锁 CI 覆盖每次变更；coverage / 无锁工作流聚焦功能测试；修复线程池、实时和 CUDA 环境相关的 flaky 测试，并让全部教程示例参与构建检查。

### 文档

- **迁移与边界说明**：`MIGRATION.md` 新增 0.2.3 → 0.3.0 的通信、任务图与阻塞 I/O 迁移建议，明确这些 API 均为兼容扩展；API 文档补充通信 facade、任务图、I/O worker、实时丢弃计数与集成边界。
- **性能记录**：更新批量提交 benchmark 基线数据；性能收益仍依赖任务规模、硬件、线程数与构建配置，应以本地测试结果为准。

### 兼容性

- **无破坏性变更**：0.3.0 保持 0.2.3 公开 API 兼容。通信 facade 不替代调用方的协议、设备重连、数据语义或安全策略；实时通信 facade 的内部实现不构成硬实时或无锁保证，存在此类要求时应使用经验证的专用实现。

---

## [0.2.3] - 2026-07-08

0.2.3 是面向 `Executor` facade 完整度与运行时失败可观察性的向后兼容版本。已有 0.2.2 代码可以继续编译使用；新代码建议优先使用 `_ex`、failure callback、facade 实时推送和可诊断等待 API。

### 新增

- **统一失败事件模型**：新增 `FailureKind`、`ExecutorFailureEvent`、`ExecutorFailureStatus` 与 `ExecutorFailureCallback`，覆盖任务异常、提交拒绝、任务超时、实时丢任务、GPU 失败、等待超时和调优回退等事件类型。
- **Facade 失败观察入口**：`Executor` 新增 `set_failure_callback()`、`get_failure_status()`、`get_recent_failures()`、`clear_recent_failures()` 和 `set_recent_failure_capacity()`；未设置 callback 时，失败仍会累计到状态计数和最近事件缓冲。
- **可诊断 Result API**：新增 `ExecutorResult`、`ExecutorErrorCode` 与 `executor_error_code_to_string()`；`initialize_ex()`、`register_realtime_task_ex()`、`start_realtime_task_ex()`、`register_gpu_executor_ex()` 可返回稳定错误码与说明消息，旧 `bool` API 保持兼容并委托到 `_ex`。
- **等待与生命周期状态**：新增 `CompletionStatus`、`WaitResult`、`wait_for_completion_for()`、`wait_for_completion_ex()`、`try_wait_for_completion()`、`is_idle()` 和 `get_completion_status()`，等待超时时可查看 active / queued / pending 任务快照。
- **周期任务状态查询**：新增 `PeriodicTaskStatus`、`get_periodic_task_status()` 与 `get_all_periodic_task_status()`，记录周期任务执行次数、失败次数、连续失败次数、最后错误与下一次执行时间。
- **实时 facade 推送**：新增 `Executor::push_realtime_task()` 与 `try_push_realtime_task()`，用户无需获取底层 `IRealtimeExecutor*` 即可推送实时任务；失败通过返回值、failure event 和状态计数同时可见。
- **实时拒绝原因计数**：`RealtimeExecutorStatus` 新增 `rejected_not_running_count`、`rejected_empty_task_count`、`pool_exhausted_count`、`queue_full_count`，将 `dropped_task_count` 的主要原因拆开观测。
- **失败可观察示例**：新增/更新 `examples/failure_observability.cpp`、`examples/periodic_monitoring.cpp` 与 `examples/realtime_can.cpp`，展示 `_ex` 初始化、failure callback、wait result、周期状态和实时 facade 推送。

### 修复

- **普通异步任务异常可见**：通过 `Executor::submit()` / `submit_priority()` / `submit_batch()` 提交的用户任务即使调用方没有立即 `future.get()`，也会记录 `TaskException` failure event，并让底层失败统计保持可见。
- **fire-and-forget 批量任务异常可见**：`submit_batch_no_future()` 的用户任务异常进入 failure event / status counter，避免无 future 场景下静默丢失异常。
- **提交拒绝可见**：未初始化、shutdown 后提交、空 batch、执行器不可用等提交失败路径记录 `SubmitRejected`，有 future 的路径会设置异常。
- **延迟与周期任务失败可见**：`submit_delayed()` 到期提交失败会设置 promise 异常并记录 failure event；`submit_periodic()` 的周期回调异常会更新周期任务状态并触发 failure event，默认继续调度。
- **等待超时可诊断**：`wait_for_completion()` 保持兼容签名，但超时不再无声返回；`wait_for_completion_ex()` 返回超时状态快照并累计 `wait_timeout_count`，shutdown 等待超时也会留下诊断事件。
- **Callback 异常隔离**：failure callback 自身抛出的异常不会杀死 worker、定时器线程或后台执行路径。
- **实时推送失败归因**：facade 推送在实时 executor 不存在、未运行、空任务、队列满或对象池耗尽时统一返回 `false` 并记录对应 failure event。

### 文档

- **README / README_zh**：同步 `Executor` facade 的失败可观察性、可诊断 `_ex` API、实时推送入口与 `wait_for_completion_ex()` 示例。
- **API.md**：补充 `ExecutorResult`、failure status、periodic status、wait result、实时背压字段和软超时 future 异常语义。
- **MIGRATION.md**：新增“从 0.2.2 升级到 0.2.3”，明确无破坏性变更，并给出推荐迁移入口。
- **Facade 可观察性计划**：`docs/todolists/facade_observability_update_plan.md` 标记阶段 1-6 完成，保留通信 facade 阶段作为后续工作。
- **Deb 打包指南**：发布命令、版本检查清单和 CUDA 完整包说明更新到 `0.2.3`。

### 测试 / CI

- **失败可观察性测试**：新增/补强 `test_executor_failure_observability`、`test_periodic_failure_observability`、`test_thread_pool_timeout_future`，覆盖任务异常、批量无 future、周期任务失败和软超时 future 异常。
- **可诊断 API 测试**：新增 `test_executor_result_diagnostics`，覆盖初始化、实时注册/启动、GPU 注册等 `_ex` 错误码。
- **等待超时测试**：新增/补强 `test_wait_completion_result`、`test_wait_for_completion_timeout_observable`，验证完成、超时、未初始化状态和 failure counter。
- **实时 facade 推送测试**：新增 `test_realtime_facade_push`，覆盖成功推送、不存在 executor、未运行、空任务、停止后推送和队列满失败路径。
- **通信 facade 规划测试**：保留 `tests/harness/test_comm_facade_usage.cpp` 的 disabled 用例，作为后续 `executor::comm` facade API 的需求锚点；0.2.3 不发布该 API。

### 兼容性

- **无破坏性变更**：0.2.3 保持 0.2.2 公开 API 兼容；新增 API 均为扩展。旧 `initialize()`、`register_realtime_task()`、`start_realtime_task()`、`register_gpu_executor()`、`wait_for_completion()` 和底层 `push_task()` 继续可用。

---

## [0.2.2] - 2026-06-18

### 修复

- **v0.2.1 紧跟的 CI 修复**：见下方 `### 测试 / CI` 段。
- **无锁基础设施稳定性**（无 PR）：修复 benchmark 超时、`shutdown(true)` 任务挂起、`dispatch_batch` 任务丢失、周期任务取消竞态、无锁队列容量检测与内存可见性问题。
- **ObjectPool ABA 关键修复**（无 PR）：将 `ObjectPool` free list 从无锁 CAS 改为 mutex 保护，彻底消除 ABA 导致的 SEGFAULT；影响 `LockFreeTaskExecutor`、`RealtimeThreadExecutor` 等使用 `ObjectPool<Task>` 的执行器，`acquire()` / `release()` 接口保持兼容。
- **ObjectPool 入参与释放防护** [#3]：拒绝 `capacity=0` 构造，避免无效对象池配置。
- **ObjectPool release 防护**（无 PR）：新增 double-free / foreign pointer guard，防止重复释放或外部指针污染对象池。
- **ObjectPool::release O(1)** [#41]：优化释放路径，保留正确性防护并降低释放开销。
- **ThreadPool WorkerLocalQueue** [#1] [#6]：修复 `empty()` 判断逻辑与 `steal()` 竞态。
- **ThreadPool 状态统计** [#2]：修复 `get_status().idle_threads` 的 `size_t` 下溢。
- **ThreadPool 并发关闭** [#11]：修复 `stop()` / `shutdown()` 并发调用导致 double-join UB。
- **ThreadPool resize / dispatch** [#14]：worker 无效时任务重新入队，避免 resize 期间丢任务。
- **ThreadPool try_steal_task** [#15]：对 `local_queues_` 使用 `shared_lock`，修复 resize 并发访问竞态。
- **ThreadPool resize UAF**（无 PR）：使用 `shared_lock` 防护 resize 期间的队列生命周期。
- **LoadBalancer 数据竞争** [#12]：将 `strategy_` 改为 atomic。
- **LockFreeQueue 数据竞争** [#4] [#13]：`size()` 使用 acquire ordering，`stats_enabled_` 改为 atomic。
- **LockFree batch 异常安全** [#31]：`push_tasks_batch` 在对象池耗尽与部分入队场景下保持资源回收正确。
- **LockFreeTaskExecutor 构造泄漏** [#33]：使用 `unique_ptr` 替换裸指针，修复构造失败路径泄漏。
- **LockFreeTaskExecutor 异常可见性** [#44]：任务异常可被统计与观察，避免后台吞掉故障。
- **GPU submit_kernel_after** [#7] [#28]：`submit_kernel_after` 不阻塞 GPU worker，并修复依赖任务 UAF。
- **CudaExecutor wait_for_completion UAF** [#39]：修复等待完成期间的生命周期问题。
- **CudaExecutor submit 不死锁** [#43]：修复提交路径中可能出现的死锁。
- **Realtime 周期任务预算** [#40]：周期任务超预算时正确记录与处理。
- **simple_cycle_loop skip-late** [#42]：周期循环对过晚周期执行跳过策略，降低积压。
- **set_thread_priority nice** [#45]：Linux 下真正应用 nice 值，修复优先级配置未生效问题。
- **Windows 编译调整**（无 PR）：修复 Windows 平台编译兼容性。

### 新增

- **LockFreeTaskExecutor**（无 PR）：新增 MPSC 无锁任务执行器，提供 `start()`、`stop()`、`push_task()`、`is_running()`、`pending_count()`、`processed_count()` 与队列统计接口，适用于高频日志、实时事件与多线程任务聚合。
- **批量任务提交 API**（无 PR）：新增 `submit_batch()` 与 `submit_batch_no_future()`，单线程 500-2000 任务场景可获得 **5-16x** 加速。
- **LockFreeTaskExecutor 批量提交**（无 PR）：新增 `push_tasks_batch()`，支持尽力批量入队与实际入队数量回传。
- **智能调度接口**（无 PR）：新增智能调度与自适应调度能力，为后续 facade 默认优化提供基础。
- **实时 push_task 背压可见性** [#32]：新增 `push_task_ex()` 与 `dropped_task_count` / `failed_pushes` 等状态字段；`push_task()` 保持 void 兼容，背压丢任务可被观测。
- **软任务超时** [#24]：新增 `task_timeout_ms` 软超时语义，执行前 `elapsed >= timeout` 时跳过并计入 `timeout_count`；C++ 无安全线程终止机制，执行中的任务不被强制中断。

### 优化

- **无锁 MPSC 基础设施**（无 PR）：从 MPSC 队列、无锁任务执行器、批量提交一路演进到序列号 MPSC 队列、False Sharing 消除、CAS 重试策略优化、性能监控优化与 worker local queue 无锁化。
- **无锁工作线程队列**（无 PR）：`WorkerLocalQueue` 改造为无锁实现，提交吞吐量 **441,500 → 488,698 tasks/s（+10.7%）**，端到端吞吐量 **433,083 → 442,009 tasks/s（+2.1%）**。
- **Linux 实时性加固** [#16]：`RealtimeThreadExecutor` 增加 `mlockall`、`timer_slack`、线程命名等加固，1ms 周期 jitter p99 从 61 µs 压至约 15-20 µs。
- **Default-Optimal Facade (P019)** [#19] [#20] [#21] [#22]：
  - `enable_memory_lock` / `timer_slack_ns` 从 opt-in 改为 opt-out，默认开启实时性优化。
  - `min_threads` / `max_threads = 0` 时自动探测 `hardware_concurrency()`，`work_stealing` 默认开启。
  - 线程池 `cpu_affinity` 空时自动分配 [0..hw-1]，实时线程按周期自适应优先级。
  - 实时线程 `cpu_affinity` 空时自动绑核；多实时线程使用 round-robin 自动亲和性。
  - 1ms 周期 jitter p99 从 54.64 µs 降至 **1.77-6.64 µs**，降低 **89-97%**。
- **ThreadPool soft timeout** [#24]：执行前跳过超时任务并计数，避免误导用户认为执行中任务会被强杀。
- **LockFree spin+yield** [#26]：用 spin+yield 替换 100µs busy-sleep，降低无锁执行器等待延迟。
- **Realtime Windows timer 数据竞争** [#27]：`timer_period_ms_` 改为 atomic。
- **Realtime 多线程亲和性** [#29]：多个实时线程自动 round-robin 分配 CPU affinity。
- **GPU 性能优化**（无 PR）：补充 GPU 性能优化与性能测试报告。

### 文档

- **ObjectPool ABA 设计说明**（无 PR）：新增 ABA 修复设计文档，说明从 CAS free list 切换到 mutex 的正确性取舍。
- **API / README / CHANGELOG / MIGRATION 同步** [#17]：同步公开 API、默认值、迁移说明与发布记录。
- **README 拆分** [#18]：拆分英文 `README.md` 与中文 `README_zh.md`。
- **P019 facade 文档同步** [#23]：同步默认即最优 facade 哲学、实时性默认值与性能描述。
- **批量提交与软超时语义** [#25]：补充 `push_tasks_batch` 与 `task_timeout_ms` soft timeout 说明。
- **LockFreeQueue empty / size 语义** [#30]：说明 `empty()` 与 `size()` 在并发场景下的近似语义。
- **API 背压字段** [#46]：补充 `push_task_ex()`、`dropped_task_count`、`failed_pushes`、`queue_capacity` 等背压字段说明。

### 测试 / CI

- **v0.2.1 紧跟的 CI 修复**（无 PR）：连续修复 5 个 CI 问题，稳定 0.2.1 后续发布分支。
- **benchmark_batch_* 超时修复**（无 PR）：修复批量提交 benchmark 测试超时。
- **benchmark_lockfree_task_executor timeout**（无 PR）：CTest timeout 从 30s 调整为 120s，避免高吞吐压测误判超时。
- **无锁队列与批量提交测试清理**（无 PR）：移除无用测试文件并补强批量、并发、工作窃取相关测试。
- **benchmark latency 阈值调整**（无 PR）：放宽 `latency_single_task` P99 限制到 100µs，降低 CI 环境噪声误报。
- **Code Coverage mlockall 跳过**（无 PR）：覆盖率任务中跳过 `mlockall`，避免 OOM。
- **CUDA 测试头文件包含**（无 PR）：为 `test_unified_memory` 与 `test_gpu_dep_async` 补充 CUDA headers。

### 构建

- **Windows 编译调整**（无 PR）：修复 Windows 平台构建问题。
- **CMake 4.x CUDA Toolkit**（无 PR）：从 `CUDAToolkit` 推导 `CUDA_INCLUDE_DIRS`，兼容 CMake 4.x。
- **CUDA / no-CUDA 安装策略**（无 PR）：CUDA executor 运行时通过 `dlopen libcuda` 动态加载；deb 发布使用带 CUDA 的完整构建，用户机器无 CUDA 时运行时自动降级。

### 性能基准

- **任务提交吞吐量**：`benchmark_baseline` 从 v0.2.0 的 456,703 tasks/s 保持同档并在 commit path 达到约 488K+，约 **+7%+**。
- **MPSC 工作窃取场景**：提交吞吐 **441,500 → 488,698 tasks/s（+10.7%）**；端到端 **433,083 → 442,009 tasks/s（+2.1%）**。
- **实时线程 1ms 周期 jitter**：p99 **61.30 µs → 1.77-6.64 µs（-89% ~ -97%）**；avg **54.47 µs → 1-2 µs（约 -95%）**。
- **LockFreeTaskExecutor SPSC**：10K 任务提交平均 **97.29 ns**，p50 **29 ns**，p99 **1013 ns**，吞吐 **8,242,895 ops/s**。
- **LockFreeTaskExecutor 端到端**：100K 任务端到端吞吐 **5,942,007 ops/s**。
- **批量提交**：单线程 500-2000 任务场景 **5-16x** 加速。

---

## [0.2.1] - 2026-03-09

### 新增

- **OpenCL 执行器**：实现 `OpenCLExecutor`，支持跨平台异构计算（Intel/AMD/NVIDIA GPU）
- **OpenCL 动态加载**：运行时加载 OpenCL 库，无静态链接，OpenCL 不可用时安全降级
- **GPU 设备查询 API**：新增 `enumerate_cuda_devices()`、`enumerate_opencl_devices()`、`enumerate_all_devices()`、`get_recommended_backend()` 函数，用户可查询系统可用 GPU 设备及推荐后端
- **设备信息增强**：`GpuDeviceInfo` 新增 `vendor` 字段，标识 GPU 厂商（NVIDIA/AMD/Intel）
- **统一内存支持**：CUDA 执行器支持统一内存（Unified Memory），新增 `allocate_unified_memory()`、`free_unified_memory()`、`prefetch_memory()` 方法；配置选项 `enable_unified_memory`；CPU 与 GPU 可共享内存无需显式传输
- **构建与示例**：`EXECUTOR_ENABLE_OPENCL` 选项；示例 `gpu_opencl`、`gpu_device_query`、`gpu_unified_memory`

### 文档

- **OpenCL 环境搭建指南**：[docs/setup/opencl_setup.md](docs/setup/opencl_setup.md)，包含 Linux/Windows 环境配置、常见问题排查

详细设计见 [docs/design/gpu_executor.md](docs/design/gpu_executor.md)。

---

## [0.2.0] - 2026-01-29

### 新增

- **GPU 执行器（CUDA）**：`IGpuExecutor` 接口，CUDA 执行器实现，与 ExecutorManager/Executor Facade 集成
- **GPU 任务与配置**：`register_gpu_executor`、`submit_gpu`、`get_gpu_executor`、`get_gpu_executor_status`、`get_gpu_executor_names`；`GpuExecutorConfig`、`GpuTaskConfig`、`GpuDeviceInfo`、`GpuExecutorStatus`
- **CUDA 动态加载**：运行时加载 CUDA 库，无静态链接，CUDA 不可用时安全降级
- **GPU 内存与流**：设备内存分配/释放、主机↔设备/设备↔设备拷贝（含异步）、流创建/销毁/同步、流回调
- **多 GPU 设备**：按设备 ID 注册多个执行器；设备间 P2P 拷贝为**实验性**，未在多 GPU 实机充分测试
- **GPU 内存池与监控**：可选内存池（`GpuMemoryManager`）、kernel 与内存统计、异常处理与错误码转换
- **GPU 任务队列**：优先级、批量提交、任务依赖（`submit_kernel_after`）
- **构建与示例**：`EXECUTOR_ENABLE_GPU`、`EXECUTOR_ENABLE_CUDA` 选项；示例 `gpu_basic`、`gpu_multi_device`

### 其他

- **CI**：C/C++ 工作流重构，依赖升级至 v4
- **文档与测试**：实时线程周期精度记录与外部接入说明；定时器优化；消除测试中数据竞态

详细设计见 [docs/design/gpu_executor.md](docs/design/gpu_executor.md)。

---

## [0.1.1] - 2026-01-25

### 优化

- **锁竞争优化**：为 `PriorityScheduler` 的每个优先级队列使用独立锁，减少锁竞争，端到端吞吐量提升 5.3%，延迟 p99 降低 18%
- **内存分配优化**：将 `PriorityScheduler` 从 `shared_ptr<Task>` 改为 `unique_ptr<Task>`，减少内存分配开销和控制块开销
- **批量分发优化**：实现真正的批量任务分发，批量 dequeue/push/负载更新，减少锁操作次数，端到端吞吐量提升 2.9%
- **工作窃取优化**：实现基于负载的智能窃取策略，优先从高负载线程窃取任务，端到端吞吐量提升 5.9%，延迟 p99 降低 44.4%，提交吞吐量提升 7.3%
- **延迟任务处理优化**：使用 `priority_queue` 替代 `vector` + `remove_if`，按执行时间排序，提高延迟任务处理效率
- **任务 ID 生成优化**：使用原子计数器替代时间戳实现，任务 ID 生成性能提升 80-90%，端到端吞吐量提升 7.3%，延迟 p99 降低 44%

### 性能提升

相比 v0.1.0：
- 端到端吞吐量提升 **13.0%**（461,576 → 521,390 tasks/s）
- 延迟 p99 降低 **55%**（0.22μs → 0.10μs）
- 提交吞吐量略有波动，整体保持稳定

详细优化记录和性能测试结果参见 [docs/optimization/PERFORMANCE_OPTIMIZATION.md](docs/optimization/PERFORMANCE_OPTIMIZATION.md)。

---

## [0.1.0] - 2025-01-24

### 新增

- **Executor Facade**：统一 API `Executor::instance()` / 实例化模式，`initialize`、`shutdown`、`wait_for_completion`
- **任务提交**：`submit`、`submit_priority`、`submit_delayed`、`submit_periodic`、`cancel_task`
- **实时任务**：`register_realtime_task`、`start_realtime_task`、`stop_realtime_task`、`get_realtime_executor`、`get_realtime_task_list`
- **监控**：`enable_monitoring`、`get_async_executor_status`、`get_realtime_executor_status`、`get_task_statistics`、`get_all_task_statistics`
- **执行器管理**：`ExecutorManager` 单例/实例化，默认异步执行器 + 实时执行器注册表，RAII 生命周期
- **线程池**：动态扩缩容、优先级调度、工作窃取、负载均衡、任务分发
- **专用实时线程**：`RealtimeThreadExecutor`，周期回调、线程优先级、CPU 亲和性，可选 `ICycleManager` 集成
- **配置**：`ExecutorConfig`、`ThreadPoolConfig`、`RealtimeThreadConfig`
- **构建与安装**：CMake 3.16+，静态/动态库选项，`find_package(executor)` 支持（`executorConfig.cmake`、`executorConfigVersion.cmake`）
- **测试与示例**：单元/集成/性能/压力测试，`basic_submit`、`realtime_can`、`multi_project`、`monitor_example`

### 依赖与平台

- C++20，仅标准库 + `pthread`（Linux），无第三方必需依赖
- Linux/windows 下已验证；

---

## 迁移指南

当前为首次发布，无历史版本可迁移。若未来有破坏性变更，将在此补充迁移说明。

参见 [docs/API.md](docs/API.md) 与 [docs/design/executor.md](docs/design/executor.md)。
