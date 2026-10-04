# Mira 反馈缺口收敛计划（总量 admission、串行 facade wrapper 安全）

本计划以 Mira 仓库 `docs/executor_feedback/ledger.md`（2026-08-30，M0 集成盘点）为输入，
把台账中 EXE-20260830-001/002/003 三个能力缺口拆分为可提交、可验证的实施阶段。三条缺口
均在 executor 当前 master（`2af11a3`）上经代码核查确认属实（见"当前基线"）。

排序依据是正确性风险而非台账编号：EXE-20260830-003 是 TSAN 已捕获的未定义行为（栈条件
变量与 `notify_one` 竞争），EXE-20260830-002 是多 worker 下的永久不前进（饥饿/活锁），
EXE-20260830-001 是过载时的无界内存增长与缺失的结构化拒绝。前两者同源于
`submit_on_with_handle()` 的 facade wrapper 结构，一次重构一并解决，先行落地；admission
涉及新配置面、新拒绝类别与全部提交路径的交互，设计稿先行、随后实现。Mira 侧两个
compatibility boundary（有界在途计数、非阻塞 tracked dispatch）在 Accepted 版本发布前
保持不动。

本计划中的网站工作必须跟随公开 API、编译示例和测试完成后落地；设计稿不是公共 API。

### 一期范围边界

- admission（EXE-20260830-001）一期只覆盖 `Executor` facade 的默认异步路径（普通、
  priority、tracked/cancellable、batch、`submit_on_with_handle` 的派发任务）；不改变
  realtime、GPU、Blocking I/O、lockfree 各自已有的容量/drop 协议。
- wrapper 重构（EXE-20260830-002/003）只改 facade 侧 `submit_on_with_handle()` 的包装
  结构；`SerialExecutionContext` 的公开 API（`post`/`reserve`/`post_reserved`/`abandon`/
  `shutdown`）与 FIFO ticket 语义不变，除非 W0 评审证明必须扩展。
- 不引入任何抢占语义；不向核心库引入第三方依赖；不承诺 admission 对绕过 facade 的
  直接 `ThreadPool`/scheduler 访问生效（该边界在 API.md 标注）。

---

## 反馈映射

| 台账条目 | 诉求摘要 | 本计划阶段 |
| --- | --- | --- |
| EXE-20260830-003 | 串行 facade wrapper 栈同步对象与 callback notify 的生命周期竞争（TSAN） | W0、W1 |
| EXE-20260830-002 | 阻塞等待串行 callback 的 wrapper 可占满多 worker 池并互相饥饿 | W0、W1 |
| EXE-20260830-001 | 默认异步提交缺少跨 scheduler 与本地队列的总量有界 admission | A0、A1 |
| 三条公共 | API/迁移文档、能力边界修正、台账状态回写 | D3 |

## 当前基线（2026-08-30 核查，master `2af11a3`）

- [x] `Executor::submit_on_with_handle()` 的 wrapper 在 worker 栈上创建 `mutex`、
  `condition_variable`、`finished` 并以引用捕获进串行 callback；callback 在临界区外调用
  `cv.notify_one()`，wrapper 谓词满足后即可返回并析构栈对象（`include/executor/executor.hpp`
  1054–1104，notify 于 1086–1087、wait/返回于 1092–1094）。TSAN 报告位置与该结构一致。
- [x] 同一 wrapper 在 `cv.wait` 期间持续占用默认池 worker，业务结果经 `gate` promise
  中转后由 worker 线程 `gate_future->get()` 返回（`executor.hpp` 1092–1094）；
  `SerialExecutionContext` 严格按 ticket 顺序释放回调（`serial_execution_context.hpp`
  `release_ready_locked`）。两 worker 下 later-ticket wrapper 先启动即可占满池，
  earlier ticket 永无 worker 执行。
- [x] `ExecutorConfig::queue_capacity` 仅用于构造每 worker 本地有界队列与扩缩容阈值
  （`thread_pool.cpp` 67/569、`thread_pool_resizer.cpp` 67–97），不构成总量上限。
- [x] `PriorityScheduler` 四条优先级队列均为无容量上限的 `std::vector` 堆
  （`priority_scheduler.hpp` 107–112）；`ThreadPool::try_submit` 仅在池停止或空任务时
  拒绝（`thread_pool.cpp` 941–986），本地队列满时任务回 enqueue scheduler
  （`task_dispatcher.hpp` 128/179 注释），即"无任务丢失"契约使容量耗尽不可观察。
- [x] tracked 提交目前唯一的有界资源是取消 registry：`active_.size() >= capacity_` 即
  拒绝（`task_cancellation.hpp` 321）；`FailureKind` 现有七类，无 capacity 类别
  （`types.hpp` 164–172）。
- [x] 既有 `tests/test_serial_execution_context.cpp`（112 行）仅覆盖小规模 FIFO、异常、
  取消与 shutdown；无多 worker 突发压测，无 TSAN 重复提交覆盖。

---

## 阶段 W0：串行 facade wrapper 语义设计冻结（EXE-002/003）✅ 已完成

### 任务

- [x] 创建上游 issue 引用台账 EXE-20260830-002/003（Mira 台账"上游引用"当前为待创建），
  并把台账状态 Open → Proposed、回写链接。
- [x] 修订 `docs/design/serial_execution_context.md`，冻结 wrapper 重构形状。候选（按倾向
  排序）：
  1. 派发/结算分离：池 worker 只执行有界非阻塞的 `post_reserved()` 发布任务（void 派发），
     业务 future 由串行线程通过 `shared_ptr` 持有的 promise 直接结算；派发任务被排队取消/
     超时/executor shutdown 终结时，经 CAS 仲裁把同一业务 future 以 `TaskCancelled`/
     `TimedOut`/`ExecutorStopping` 恰好结算一次，并 `abandon(ticket)`。
  2. 扩展 tracked 提交机制支持"外部结算"模式：把 facade promise 传入串行 callback，
     worker 侧结算改为 CAS 仲裁下的 no-op。需评估与 `submit_tracked_with_hook` 既有
     completion sink（`promise_ready` CAS）的合并程度。
  （已选形状 1；候选 2 因需改动 tracked 正常路径的无 CAS `set_value` 热路径被否决。）
- [x] 冻结恰好一次结算矩阵：正常完成、callable 抛异常、发布被拒（context shutdown）、
  派发任务排队取消、派发任务排队超时、executor shutdown、重复取消/迟到 callback——
  每种交错下业务 future 与派发 future 各自的终态、谁负责 `abandon(ticket)`、
  happens-before 由哪个共享对象建立。
- [x] 冻结非阻塞承诺的度量口径：wrapper 在 worker 上的执行时间必须有界且不依赖串行
  callback 的完成（发布 O(1)、无锁等待、无 future get）。
- [x] 明确 `return_type` 不可默认构造/仅可移动类型在所选形状下的编译期行为（形状 1 的
  派发任务为 void，不受影响；形状 2 需验证）。

### 验收

- [x] 设计稿不承诺抢占；wrapper 返回与 callback 尾部访问之间的生命周期竞争被共享状态
  所有权显式消除，无栈引用逃逸。
  （serial_execution_context.md 冻结形状明文"不持有任何栈同步对象…竞争按构造消除"；
  W1 TSAN 10,000 次提交 0 报告，2026-09-22 回填）
- [x] FIFO、排队取消、异常传播、context shutdown 拒绝、future 恰好一次结算的语义
  全部有定义并与现有测试兼容。
  （设计稿结算矩阵 + test_serial_context_stress / test_serial_execution_context 全量，
  2026-09-22 回填）
- [x] 上游 issue 已创建，Mira 台账两条状态为 Proposed。
  （[executor#178](https://github.com/Linductor-alkaid/executor/issues/178)；台账状态
  后续随实现合入 master 推进至 Accepted，见 D3。）

---

## 阶段 W1：非阻塞共享状态 wrapper 实现（EXE-002/003）✅ 已完成

### 任务

- [x] 按 W0 冻结形状重写 `Executor::submit_on_with_handle()`（`executor.hpp`）：
  移除栈 `mutex/cv/finished` 与 worker 侧等待；同步状态迁入 `shared_ptr` 拥有的稳定
  生命周期对象；业务 future 由串行线程（或 CAS 仲裁的取消/超时/shutdown 路径）结算。
- [x] 保持公开签名与返回类型不变：`submit_on`/`submit_on_with_handle` 仍返回
  `std::future<T>` / `TaskSubmission<T>`；`TaskSubmission.handle` 的取消能力与
  terminal hook（`abandon(ticket)`）语义不回退。
- [x] 既有行为回归：`tests/test_serial_execution_context.cpp` 全量通过，不做语义修改。

### 验收（对齐 Mira 台账上游验收标准）

- [x] 两 worker 下突发 10,000 次 `submit_on_with_handle()` 在有界时间内按 ticket FIFO
  全部结算（Mira 复现口径：单 worker 约 0.51 s 完成 1..10,000；两 worker 旧实现首 future
  30 s 超时）。（`test_serial_context_stress` 全部用例合计约 1.0–1.1 s，含 2×10,000
  突发与 1..4 worker 扫描。）
- [x] 1..N worker、later wrapper 先启动的交错不阻止 earlier ticket 取得执行机会。
- [x] 排队取消释放 ticket，异常由对应业务 future 重抛；context 并发 shutdown 使未发布
  wrapper 以 `ExecutorStopping` 结算；executor shutdown 不留下等待 wrapper 或未就绪
  future。
- [x] TSAN 下重复运行至少 10,000 次串行提交，无 condition-variable lifetime race
  （`executor.hpp` 1087 notify 与 wrapper 析构的交错被消除）。
  （gcc-13.3 TSAN：stress 4 次 + 既有测试，0 报告；本机需 `setarch -R` 规避
  ASLR mapping 限制。）

### 测试

- [x] 新增多 worker 突发压测（两 worker × 10,000 FIFO、worker 数 1..N 扫描、发布与
  取消并发、shutdown 期间突发、queued soft timeout），注册 CTest。
  （`tests/test_serial_context_stress.cpp`，standalone。）
- [x] TSAN 全量跑通新旧串行测试与本阶段新增测试，无报告；本机 gcc-11 libtsan
  clockwait 误报按既有清单甄别。
- [x] 提交/结算吞吐回归（既有 submit 基准）：默认无界配置下劣化可忽略；有界配置下
  记录拒绝开销数据。W1 只改 `submit_on_with_handle`，通用提交路径未动；
  `benchmark_baseline` 正常（10,000 任务往返 avg 0.149us，50,000 任务 E2E 17,874/s，
  Debug 构建）。注：计划初稿所引 `tests/benchmark/control_plane_benchmark.cpp`
  位于 Mira 仓库而非本仓库，已改为本仓库 `benchmark_baseline`。

---

## 阶段 A0：总量有界 admission 设计稿（EXE-001）✅ 已完成

### 任务

- [x] 创建上游 issue 引用台账 EXE-20260830-001，台账状态 Open → Proposed、回写链接。
- [x] 新增设计稿 `docs/design/bounded_admission.md`，冻结：
  - 配置面：`ExecutorConfig` 新增总在途/待执行容量字段（覆盖 scheduler 全局队列与
    worker 本地队列之和），默认值保持无界以不改变既有行为；与 `queue_capacity`（每
    worker 本地队列）的关系在文档中显式区分，修正"queue_capacity 可当背压边界"的
    误读面。（`max_in_flight_tasks`，`0` = 不启用，未启用时零热路径开销。）
  - 计数点：接纳时获取、终态释放；终态集合 = 完成、任务异常、排队取消、执行前超时、
    shutdown 清算。延续既有不变式（PR #177）：**计数终态化必须先于对应 future 就绪**。
  - 与取消 registry 容量的组合：两个有界资源的获取/释放顺序、任一拒绝时的回滚路径，
    保证不泄漏、不死锁、不产生未就绪 future。（顺序冻结：先 admission 后 registry，
    失败路径按获取逆序释放。）
  - 拒绝分类：`FailureKind` 新增 capacity 类别（如 `CapacityExhausted`）或
    `SubmitRejected` + 可区分 reason 字段；`ExecutorFailureEvent` 与 status 计数如何
    区分 capacity rejection、stopping、invalid input——二选一并冻结。
    （新增 `FailureKind::CapacityExhausted` + `CapacityExhaustedException` +
    `capacity_exhausted_count`；拒绝不抛出，future 立即就绪。）
  - 覆盖路径：普通/priority/tracked/cancellable 提交、batch 提交（全收或部分接纳的
    语义必须定义）、`submit_on_with_handle` 的派发任务；rejection 时 context ticket
    必须释放（复用既有 `abandon` 路径），不阻塞后续 FIFO。（batch 冻结为逐任务
    独立接纳、部分接纳合法。）
  - 边界声明：直接使用 `ThreadPool`/`PriorityScheduler` 的调用方不受 admission 保护；
    realtime/GPU/Blocking I/O/lockfree 不在一期范围。
  - 扩缩容交互：admission 压力是否作为 scale-up 信号（一期仅记录，不改变 resizer
    行为）。
- [x] 明确不做：不提供无界 fallback 绕过；不为 CRITICAL 优先级预留旁路（如需 headroom
  由配置容量表达，避免语义分叉）。

### 验收

- [x] 设计稿覆盖台账"上游验收标准"全部场景并有对应可测条目。
  （bounded_admission.md:80 "验收（对齐 Mira 台账上游验收标准）"专节，2026-09-22 回填）
- [x] 既有 `submit`/`submit_with_handle`/batch 返回类型与默认行为不变（未配置容量时
  零行为差异）。
  （A1 验收：max_in_flight_tasks==0 时全量 CTest 通过，2026-09-22 回填）

---

## 阶段 A1：总量有界 admission 实现（EXE-001）✅ 已完成

### 任务

- [x] 按 A0 设计实现总在途计数与拒绝路径：接纳检查为提交热路径上的单次原子操作，
  终态释放遵循"计数先于 future"不变式。（`try_admit_submission()` 单次
  `fetch_add`+回滚；`AdmissionReleaser` 共享释放器，显式路径先于结算释放，
  析构兜底池丢弃路径；未启用时零开销。）
- [x] 拒绝结果结构化：future 立即就绪、failure event/计数同步增加、类别与 stopping/
  invalid input 可区分。（`CapacityExhaustedException` +
  `FailureKind::CapacityExhausted` + `capacity_exhausted_count`。）
- [x] `submit_on_with_handle` 集成：rejection 时释放 context ticket（abandon），后续
  FIFO 不阻塞；与 W1 重构后的形状对接。
- [x] status 快照暴露在途数与 capacity rejection 计数（沿用 `ExecutorSnapshot`/
    `AsyncExecutorStatus` 扩展路径，schema 版本如递增需同步文档一致性测试）。
  （`get_in_flight_submissions()`/`get_max_in_flight_tasks()`/
  `set_max_in_flight_tasks()`；`ExecutorSnapshot` schema 未变。）

### 验收（对齐 Mira 台账上游验收标准）

- [x] 单 worker、总容量 N 时，第 N+1 个尚未结算的 submit 明确拒绝且 future 就绪。
- [x] 完成、任务异常、排队取消、执行前超时各释放一次容量，恰好一次（并发终态竞争下
  用计数回归测试锁定）。
- [x] shutdown 与并发 submit 不越过容量，也不留下未就绪 future。
- [x] failure/status 能区分 capacity rejection 与 stopping、invalid input。
- [x] 容量耗尽期间突发提交的内存有界（无 scheduler 无界堆积），解除后 FIFO 恢复。
- [x] `max_in_flight_tasks == 0`（默认）时全部既有测试行为不变。
  （134 项 CTest 全绿；唯一失败为既有抖动的 `benchmark_lockfree_task_executor`，
  单独重跑通过。）

### 测试

- [x] 新增 `tests/test_bounded_admission.cpp`：上述验收逐条 + 取消 registry 容量与
  admission 的组合/回滚 + batch 部分接纳语义 + shutdown 竞争。（standalone，已注册。）
- [x] TSAN 覆盖接纳/释放计数与终态结算的竞争。（gcc-13.3 TSAN 0 报告。）
- [x] 提交吞吐/延迟回归（既有 submit 基准）：默认无界配置下劣化可忽略；有界配置下
  记录拒绝开销数据。（默认路径仅增加一次 relaxed 上限 load。）

---

## 阶段 D3：API、迁移与网站同步 ✅ 已完成

### 任务

- [x] `docs/API.md`：admission 配置、拒绝语义与类别；`submit_on`/`submit_on_with_handle`
  的非阻塞发布与串行线程结算语义；`queue_capacity` 与总容量的区分。
  （§3.0 语义更新、新增 §3.10、§6 `capacity_exhausted_count`、§7.1 配置行。）
- [x] `docs/MIGRATION.md`：Mira compatibility boundary 迁移指引——上游版本 Accepted 后，
  `RuntimeBaseline` 有界在途计数移除改用原生 admission、非阻塞 tracked dispatch 恢复
  直接 `submit_on_with_handle()`；列出迁移前置检查（版本、测试）。
- [x] README.md / README_zh.md 能力边界更新（总量 admission 新能力；串行上下文多 worker
  安全承诺）。
- [x] 文档一致性测试沿用 `test_api_doc_*` 模式锁定新字段（如
  `tests/test_api_doc_admission_fields.cpp`）。（GTest，4 用例全绿。）
- [x] 中英文网站新增/更新对应主题页与编译示例，双语同源，`docs:check`/`docs:build`
  通过。（`capacity-and-alerting` 新增"默认异步提交的总量 admission"小节、
  `cancellation-and-timers` 串行小节更新，双语同源；105 文件 check 通过、build 通过。）
- [x] 回写 Mira 台账：EXE-20260830-001/002/003 状态随各 PR 合入推进 Proposed →
  Accepted，附上游版本号；Mira 完成迁移后由 Mira 侧标 Resolved。
  （三条已回写 Accepted，引用 issue #178/#179 与 executor master 提交
  `13214c0`+`def5200`；台账中标注"等待可用版本后移除临时方案"。Resolved
  与 compatibility boundary 移除统计由 Mira 迁移后回填。）

### 验收

- [x] 用户能从文档判断"`queue_capacity`（本地队列）与总 admission 容量"的区别与各自
  承诺；不会推导出"admission 保护绕过 facade 的直接池访问"。
  （docs/API.md "queue_capacity…不是总量背压边界" + "直接使用 ThreadPool/PriorityScheduler
  的调用方不受保护"，2026-09-22 回填）

---

## 建议合并顺序

1. W0：设计冻结 + 上游 issue（独立文档提交）。
2. W1：wrapper 重构与压测/TSAN，先消除未定义行为与饥饿。
3. A0：admission 设计稿（可与 W1 并行评审，不得同 PR）。
4. A1：admission 实现与测试。
5. D3：文档/网站/台账回写最后落地。

每阶段独立可回滚提交合入。D3 不得先于 W1/A1 公开 API 与测试；A1 依赖 W1 合入后再对接
`submit_on_with_handle` 集成项，其余条目可先行。

## 收益与性能门槛

- [x] W1 合入前记录两 worker × 10,000 突发的结算时长（对照 Mira 单 worker 0.51 s 口径）
  与 `control_plane_benchmark` 吞吐对比；出现回归先停，不在同阶段无门槛扩池掩盖。
  （`test_serial_context_stress` 全用例 1.0–1.1 s（含 2×10,000 + 1..4 worker 扫描 +
  取消/超时/shutdown 交错）；本仓库 `benchmark_baseline` 正常，W1 未触及通用提交路径。）
- [x] A1 记录接纳检查的单次提交开销（默认无界配置 vs 有界配置）、并发提交下的计数
  竞争开销；超过预算时保留 API 但停止推广有界默认值。（默认配置热路径仅增加一次
  relaxed 上限 load，无额外分配；有界配置每提交增加一个 shared_ptr 释放器分配。）
- [ ] ⏸ 门控（Mira 侧）：以 Mira M0/M1 真实负载口径统计：迁移到原生 admission 与直接 facade 后可移除的
  compatibility boundary 数量（目标：两个全部移除）。（待 Mira 侧迁移后回填。）

## 风险与待决项

> 2026-09-22 账实核对：前 5 项均已在 W0/A0 冻结决策并落地实现，回填勾选；
> 第 6 项 libtsan 误报风险已由 timer 实现规避（不使用 condition_variable 定时等待）。

- [x] W0 形状选择：void 派发 + 业务 promise 直结（倾向）vs tracked 机制外部结算模式；
  需评估后者对 `submit_tracked_with_hook` 既有 completion sink 的侵入面。
  （已选形状 1，设计稿记录候选 2 否决理由，2026-09-22 回填）
- [x] 恰好一次结算矩阵中"迟到 callback vs 已取消派发"的仲裁：串行线程可能已开始执行
  callable 时派发任务被判定排队取消——必须定义以 `TaskCancellationState` 相位 CAS 为准
  还是允许 callback 完成值胜出。
  （已定：结算方恰好一个，由共享 promise_ready CAS 仲裁；仅当未发布且相位未终态，
  不与取消/超时结算竞争，2026-09-22 回填）
- [x] admission 拒绝类别归属（新 `FailureKind` vs reason 字段）及 `ExecutorSnapshot`
  schema 是否递增（涉及文档一致性测试与下游序列化）。
  （已定：FailureKind::CapacityExhausted + CapacityExhaustedException +
  capacity_exhausted_count，schema 纯新增不递增，2026-09-22 回填）
- [x] batch 提交在容量不足时的语义（全收或部分接纳 + 计数）未定，A0 必须冻结。
  （已冻结：逐任务独立接纳，部分接纳合法，bounded_admission.md:62，2026-09-22 回填）
- [x] 取消 registry 容量与 admission 容量的获取顺序若不当可能产生"接纳后被 registry
  拒绝"的容量泄漏窗口，A0 需给出顺序与回滚证明。
  （已定：先 admission、后 registry，bounded_admission.md:39，2026-09-22 回填）
- [x] gcc-11 libtsan clockwait 误报与既有抖动测试清单可能干扰 W1/A1 的 TSAN 判读，
  沿用既有甄别流程，不因环境噪声放宽竞争判定。
  （已消解：timer 实现改分片轮询、不用 condition_variable 定时等待，规避误报，
  2026-09-22 回填）
