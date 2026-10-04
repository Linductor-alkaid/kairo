# 客户端反馈缺口收敛计划（协作取消、定时句柄、序列化上下文）

本计划以 heyaki 仓库 `docs/executor-feedback-ledger.md`（2026-08-29，M0–M5 盘点）为输入，
把台账中 P1 级 executor 能力缺口拆分为可提交、可验证的实施阶段，并按台账建议的收益/风险
顺序排序：先协作取消（P1-3），再定时句柄（P1-2），最后序列化上下文（P1-1，指南先行）。
P2-1/P2-2 按台账结论延后到 heyaki M6/M7 真实压测后重估；台账 P3（heyaki 侧接入待办）与
V-1（heyaki 自行整改）不属于本计划范围。

本计划中的网站工作必须跟随公开 API、编译示例和测试完成后落地；设计稿不是公共 API，不能
先在使用手册中承诺尚未实现的类型或语义。

### 一期范围边界

- C1/T1 一期只覆盖 `Executor` facade 的普通异步任务（含 priority）、任务图以及 facade
  定时器；不改变 realtime、lockfree、GPU、Blocking I/O 各自已有的停止/drop/worker 生命周期协议。
- T1 的 `TimerHandle` 只保证由 facade timer 调度到已选择的 executor；不保证与 asio strand 或
  其他外部序列化上下文绑定，因此不能宣称可直接替换 heyaki 中所有 `asio::steady_timer`。
- 外部上下文绑定属于后续 T2/S2 能力。T1 仅可迁移不依赖 strand 所有权的定时工作；需要同一
  strand 执行和销毁的工作继续使用应用侧 timer，直到 T2/S2 验收完成。

---

## 反馈映射

| 台账条目 | 诉求摘要 | 本计划阶段 |
| --- | --- | --- |
| P1-3 | 运行中任务的协作取消令牌，取消事件进入 status 体系 | C0、C1 |
| P1-2 | 可取消、可重排、可与对象生命周期绑定的 delayed/periodic 句柄 | C0、T1；外部 context 绑定由 T2/S2 门控 |
| P1-1 | 序列化上下文 / `submit_on` 使 strand 类派发纳入 admission 与监控 | S1（指南）、S2（API，门控） |
| P2-1 | 同上下文 signal/slot 原语，或明确文档指引何时允许裸回调 | G1（轻量文档项随 D1） |
| P2-2 | 带优先级/权重与 drop 策略的 channel 变体或公共骨架 | G1 |
| P3、V-1 | heyaki 侧待办与自行整改 | 不在本计划内 |

## 当前基线

- [x] `Executor::cancel_task(task_id)` 仅作用于周期任务，无通用任务取消（`executor.hpp` facade）。
- [x] `TaskOptions::deadline` 明确为路由/诊断用途，不中断已开始运行的任务（`task_options.hpp`）。
- [x] `executor::StopToken`/`StopSource`（含 Android fallback 与 `EXECUTOR_STOP_TOKEN_FORCE_FALLBACK`
  测试通道）已存在，但只接入 `IBlockingIoWorker::run(stop_token)`，未进入任务提交路径。
- [x] `submit_with_handle`/`submit_after_with_handle` 返回 `TaskSubmission{TaskHandle, future}`；
  `TaskHandle` 仅为字符串 id，无取消能力（`types.hpp`）。
- [x] `submit_delayed` 仅返回 `std::future`，无句柄、无取消、无重排；`submit_periodic` 返回
  task_id，可经 `cancel_task` 取消，具备 `PeriodicTaskStatus`。
- [x] `FailureKind` 现有七类，无取消相关类别；`CompletionStatus` 无取消计数。
- [x] executor 核心库无 asio 依赖；asio strand/io_context 互操作目前没有任何文档化指引。
- [x] `executor::comm` 已有 `MpscChannel`、`LatestMailbox`、`RealtimeChannel`、`SnapshotStore`、
  `Topic`、`DoubleBuffer`、`PhaseGate`；无同上下文 signal/slot，无多优先级/加权 channel 变体。
- [x] 网站为 VitePress 双语结构，教程编号至 `12_blocking_io_worker.cpp`，中英文页面共用
  编译示例事实源。

---

## 阶段 C0：取消与定时语义设计评审

### 任务

- [x] 新增设计稿 `docs/design/task_cancellation_and_timers.md`，已完成先行评审；第 11.2 节
  待决项已于 2026-08-29 全部冻结并回写设计稿，C1/T1 可以启动实现。
- [x] 定义 token 注入协议：提交路径识别可接收 `executor::StopToken` 的 callable，注入方式与
  `detail::JThread`、`IBlockingIoWorker::run(stop_token)` 一致；不接收 token 的任务只享受
  排队取消。（设计稿 §6.2：token 固定为 callable 首参数，由 executor 注入）
- [x] 明确 API 选择：优先提供显式 `submit_cancellable*`/`submit_*_with_handle` overload；若
  采用自动 token 检测，必须定义首/末参数位置、泛型 lambda 冲突时的优先级、返回类型推导和
  编译期拒绝规则，禁止悄然改变既有 `submit()` 调用的调用形式。
  （设计稿 §6.2 + §11.2 冻结项 4：显式 overload，无自动检测）
- [x] 设计共享 cancellation state：任务在 scheduler、本地队列、steal 和执行包装之间只携带
  可共享的 state/StopSource，不依赖 `Task::cancelled` 的按值复制；定义 handle 到 state 的
  并发索引、终态清理、保留上限和 cancel/开始执行/完成之间的线性化点。
  （设计稿 §4.1/§4.2/§9.1 + §11.2 冻结项 1/2）
- [x] 定义两类取消语义：排队中取消（任务不开跑，future 以规定方式满足）与运行中取消
  （协作请求，不抢占、不强制中断）。（设计稿 §5.1/§5.2）
- [x] 定义取消后的 future 满足方式（如 broken promise 语义或显式空值/异常），不留未定义等待。
  （设计稿 §6.1：统一 `TaskCancelled` 异常 + reason）
- [x] 取消事件归类（已定，2026-08-29）：采用独立 lifecycle 计数，不进入 failure 体系——
  取消是正常生命周期而非失败，不向 `FailureKind` 添加取消类别、不触发 failure callback；
  设计稿需定义计数字段归属（如 `CompletionStatus` 扩展或独立 `CancellationStatus`）与查询入口。
  （设计稿 §6.3：独立 `CancellationStatus` 快照与 `get_cancellation_status()`）
- [x] 定义 timer 句柄生命周期与所有权：cancel/reschedule、shutdown 交互、periodic task_id
  与新句柄的统一程度；普通 `TimerHandle` 为可复制控制句柄，析构不取消，新增的
  `ScopedTimerHandle`（或等价命名）才采用唯一所有权和析构请求取消，避免临时对象和 future-only
  用法被意外取消。（设计稿 §7.2/§7.3 + §11.2 冻结项 7）
- [x] 明确 `deadline` 与取消的关系保持正交：deadline 仍为 advisory，取消必须显式请求。
  （设计稿 §5.3）

### 验收

- [x] 设计稿不承诺任何抢占式中断；阻塞在无 wakeup 机制操作上的任务不被承诺打断。
  （设计稿 §1.1/§2.2）
- [x] Android fallback StopToken 语义与桌面 `std::stop_token` 行为一致的设计说明。
  （设计稿 §8 + §11.2 冻结项 5：一期只承诺 polling）
- [x] C1/T1 的公开 API 草案（签名、错误码、状态字段）随设计稿一并评审。
  （设计稿 §6/§7）

---

## 阶段 C1：任务级协作取消（P1-3）✅ 已完成

### 任务

- [x] 为每个纳管任务创建独立 StopSource；其 cancellation state 在句柄、registry 和各队列副本间
  共享传播，并按 C0 确定的显式 overload（或已审定的自动检测规则）向可接收 `StopToken` 的
  callable 注入。（`TaskCancellationState`：`include/executor/task_cancellation.hpp`，
  单原子 phase 提供线性化点；提交边界采用 facade wrapper + 终态 CAS 仲裁实现，
  见设计稿 §11.2 冻结项 2。）
- [x] 新增取消入口（如 `Executor::request_task_cancel(const TaskHandle&)`，或扩展 TaskHandle），
  覆盖一期范围内的 async、priority、delayed、依赖图任务；与 periodic 保持语义文档统一，但
  不强行把 realtime/GPU/Blocking I/O 纳入同一取消协议。（delayed 经 `TimerHandle::cancel`
  覆盖；periodic 句柄化变体同协议。）
- [x] 排队中取消：任务不执行，future 按 C0 定义满足，计入取消计数。
- [x] 运行中取消：仅协作式置位 token；任务通过 `stop_requested()` 轮询或 stop callback 响应。
- [x] 状态可观测：取消计数以独立 lifecycle 字段进入 status 快照（不作为 failure 事件、
  不计入 `ExecutorFailureStatus`），可通过对应状态查询接口观察到。
  （`CancellationStatus` + `get_cancellation_status()` + `ExecutorSnapshot.cancellation`。）
- [x] Android fallback 路径同步支持，`EXECUTOR_STOP_TOKEN_FORCE_FALLBACK` 编译实例化验证。
  （`tests/test_task_cancellation_fallback.cpp` 整库 forced-fallback 编译，CTest 注册。）

### 验收

- [x] 取消语义边界文档化：取消是请求不是中断。（`docs/API.md` §3.9、README、网站页。）
- [x] future 在取消后的行为被测试锁定，不出现无定义的永久等待。
- [x] 与既有 queued soft timeout（`task_timeout_ms`）、依赖图、periodic 的交互均有定义行为。
- [x] 明确取消 API 的幂等性和旧行为兼容：成功取消不产生 failure event；重复/过期句柄返回
  明确的 `AlreadyCompleted`/`NotFound` 等结果（或文档化幂等成功），并决定是否保留旧
  `cancel_task` 对无效 periodic id 记录 `SubmitRejected` 的行为；若改变，必须补迁移说明。
  （决定：保留旧行为；见 `docs/MIGRATION.md` 与回归测试。）
- [x] 既有 `submit`/`submit_with_handle` 返回类型与行为不变（回归锁定）。

### 测试

- [x] 新增 `test_task_cancellation.cpp`：排队取消、运行中协作退出、重复取消、句柄过期、
  取消与执行并发。
- [x] 取消计数断言（独立 lifecycle 字段，非 `ExecutorFailureStatus`）；TSAN 覆盖取消置位
  与任务执行的竞争。（TSAN 全量跑通 `test_task_cancellation`，无报告。）
- [x] fallback 实例化的取消语义测试。

---

## 阶段 T1：可绑定生命周期的定时句柄（P1-2）✅ 已完成

### 任务

- [x] 新增公开 `TimerHandle`（delayed/periodic 统一）：`cancel()`、`reschedule()`/expires
  语义、状态查询；普通句柄析构不取消，RAII 取消使用单独的 `ScopedTimerHandle`（或等价类型）。
- [x] 新增 `submit_delayed_with_handle`（命名与 `submit_with_handle` 对齐）；periodic 侧按
  C0 结论提供句柄化变体，保持既有返回类型兼容。
- [x] 提供 RAII 绑定模式（`ScopedTimerHandle` 或文档化包装）：唯一拥有者销毁即请求取消，
  但不宣称这等同于“与外部 strand 同上下文销毁”；该绑定语义留给 T2/S2。
- [x] timer 任务可接收 `StopToken`（与 C1 组合），并可选指定执行 executor/priority；
  是否绑定序列化上下文留给 S2。（`*_cancellable_*` 变体注入 token；priority 绑定随
  到期派发走默认池，序列化上下文绑定明确留给 S2。）
- [x] 监控：pending/executed/cancelled 定时任务计数进入 status 快照。
- [x] shutdown 语义：timer thread 停止时所有 pending timer 明确取消并按 C0 定义满足 future。

（实现：`include/executor/timer.hpp` 的 `detail::TimerScheduler`（registry + generation
heap + 1ms 分片等待 + stale 压缩）；终态 record 只保留有界元数据。）

### 验收

- [x] cancel/reschedule 与到期执行的竞争下无 use-after-free、无双执行。
- [x] 既有 `submit_delayed`/`submit_periodic` 行为与返回类型不变。
- [x] 句柄与普通会话对象共存销毁的示例成立，且核心库不引入 asio 依赖；需要 strand 所有权
  的 asio 场景必须明确标注为 T2/S2 前不可迁移。（见 S1 指南 §5 与 MIGRATION。）

### 测试

- [x] 新增 `test_timer_handle.cpp`：cancel/reschedule 竞争、RAII 析构取消、shutdown 收敛、
  计数断言。（TSAN 全量跑通，无报告。）
- [x] `benchmark_timer_precision` 回归，确认大量句柄下 timer thread 单线程瓶颈可接受。
  （5ms delayed 基线平均到期误差约 5.5ms（10ms 轮询），新实现约 0.9ms。）
- [x] 新增编译/烟测教程 `examples/tutorial/13_cancellation_and_timers.cpp`，演示协作取消与
  定时句柄，注册 CTest。

---

## 阶段 S1：外部事件循环互操作指南（P1-1 第一步，仅文档）✅ 已完成

### 任务

- [x] 新增 `docs/` 指南《外部事件循环互操作》（asio strand/io_context 为主要案例）：
  - [x] 现行合规模式：io_context 作为 blocking worker 托管、`PhaseGate` 收尾（heyaki
    `AsioWorker` 路线）。
  - [x] 明确 post 级派发的不可见盲区：`asio::post(strand, ...)` 不进入 admission/统计/失败
    事件的现状与边界。
  - [x] 在盲区内的推荐纪律与替代模式，直到 S2（若落地）提供纳管 API。
- [x] 明确 executor 核心库不依赖 asio；指南只描述互操作。
- [x] README 与 `docs/API.md` 链接该指南，修正能力边界的过度宣称。

（指南：`docs/external_event_loop_interop.md`；可编译伴随示例
`examples/event_loop_interop.cpp`（MiniEventLoop 等价复现 strand 语义，无第三方
SDK 依赖，注册 CTest smoke `event_loop_interop_example`）。）

### 验收

- [x] 指南不承诺任何未实现 API；所有建议模式可编译复现（示例进 `examples/`）。
- [x] 覆盖台账 P1-1 描述的场景：strand 延续派发、io_context 托管收尾。

---

## 阶段 S2：序列化执行上下文 API（P1-1 第二步，门控于 S1）✅ 已完成

### 任务

- [x] 依据 S1 使用反馈决定 API 形态：executor 托管的 `SerialExecutionContext`（专用串行
  worker）与/或外部 strand adapter 的 `submit_on(context, task)` 纳管。
- [x] 纳管目标对齐台账 P1-1：post 级派发进入 admission、统计与失败事件体系。
- [x] 设计稿先行（`docs/design/serial_execution_context.md`），评审通过后实现。
- [x] 条件项不触发：S1 使用反馈支持纳管收益，已选择并实现 `SerialExecutionContext` API。

### 验收

- [x] 不向核心库引入 asio 依赖；包装外部上下文不改变其线程/序列化语义。
- [x] 以台账中 heyaki node/relay strand 派发场景作为验收参照用例（FIFO context smoke）。

### 测试

- [x] 设计定稿后补全：context 派发顺序、shutdown 收敛、监控可见性、跨 context 取消组合。

---

## 阶段 T2：外部上下文绑定定时器（与 S2 联动，条件项）

> 2026-09-22 定性：⏸ 门控（前置条件未满足）——S2 实际交付的是 executor 托管的
> `SerialExecutionContext`（include/executor/serial_execution_context.hpp），
> 未提供外部 strand adapter；heyaki asio strand timer 参照用例未启动。

- [ ] ⏸ 门控（S2 adapter）：仅在 S2 证明存在稳定的外部 context adapter 后启动；定义 `TimerHandle` 在指定 context
  上到期、取消、重排和销毁的执行位置。
- [ ] ⏸ 门控（heyaki 用例）：以 heyaki 的 asio strand timer 为参照用例，证明对象状态访问和 timer 销毁保持同一
  序列化上下文；未通过前不得在迁移文档中建议替换该类 timer。

---

## 阶段 G1：P2 能力重估门（P2-1/P2-2，明确延后）

> 2026-09-22 定性：⏸ 门控（下游进度）——由 heyaki M6/M7 外部进度触发，
> 与主清单阶段 18 同口径。

- [ ] ⏸ 门控（heyaki M6/M7）：触发条件：heyaki M6/M7 消息与文件传输真实压测完成后，按台账 P2-1/P2-2 重估：
  同上下文 signal/slot（带统计的 observer 原语）、多优先级/加权/双限额 channel 变体或
  公共骨架。
- [x] 延后期间仅落地轻量文档项：comm 使用指引中明确"何时允许裸 `std::function` 回调"
  （台账 P2-1 的次选建议），随 D1 交付。（中英文"如何选择通信组件"指南新增
  "什么时候允许裸回调"一节。）
- [ ] ⏸ 门控（heyaki M6/M7）：重估本身仍由 heyaki M6/M7 外部进度触发，未开始。
- [ ] ⏸ 门控（随重估）：重估结论（做/不做/再延后）回写台账与本计划，避免过早抽象。

---

## 阶段 D1：API 与迁移文档（随 C1/T1 公开 API 同步）✅ 已完成

### 任务

- [x] `docs/API.md`：取消协议（token 注入、排队/运行中语义、future 满足）、`TimerHandle`
  完整签名与状态字段、S1 指南入口。（§3.8 定时句柄、§3.9 任务协作取消、§6 监控计数、
  schema 3 说明。）
- [x] `docs/MIGRATION.md`：只说明不依赖外部 strand 所有权的自建定时如何迁移到
  `TimerHandle`，并列出 T2/S2 前不得迁移的 asio timer；说明从私有 deadline 取消迁移到
  `StopToken` 协作取消的适用边界。
- [x] README.md / README_zh.md 能力边界更新（取消与定时的新能力、不承诺抢占中断）。
- [x] 文档一致性测试沿用 `test_api_doc_*` 模式，锁定文档宣称与实际字段一致。
  （`tests/test_api_doc_cancellation_fields.cpp`。）

### 验收

- [x] 签名、默认值、错误码与测试一致；取消与定时的语义边界无歧义。

---

## 阶段 D2：使用手册与网站更新（跟随公开 API 与编译示例之后）✅ 已完成

### 任务

- [x] 中英文页面新增"取消与定时"主题（归入现有 realtime-and-communication 专题或按信息
  架构评审归属），双语同源。（`/realtime-and-communication/cancellation-and-timers`。）
- [x] 以 VitePress `<<< @` 嵌入 `examples/tutorial/13_cancellation_and_timers.cpp` 编译示例。
- [x] 更新两个 locale sidebar、专题 index 与 `website/translation-status.md`。
- [x] S1 互操作指南如上网站，同样双语同源、不引入第三方 SDK 依赖。
  （`/guides/event-loop-interop`，示例为无 SDK 依赖的 MiniEventLoop。）
- [x] 运行网站构建与链接检查（base `/executor/`），确认新路由、语言切换与交叉链接有效。
  （`npm run docs:check` 105 文件通过；`docs:build` 双语路由与交叉链接验证。）

### 验收

- [x] 用户能判断"排队超时、deadline 路由提示、显式取消请求"三者的区别与各自的承诺。
  （页面首个表格即三者对比。）
- [x] 用户不会从手册推导出"取消会抢占阻塞调用"或未承诺的 timer 精度。
  （"不承诺的事"小节显式列出。）

---

## 建议合并顺序

1. C0：设计稿与 API 草案评审（独立文档提交）。
2. S1：互操作指南，与 C0 并行并尽早合入，用真实边界输入 S2/T2 决策。
3. C1：协作取消与测试，先证明语义正确。
4. T1：通用定时句柄、教程 13 与 timer 精度回归，不包含外部 strand 绑定。
5. D1：API/迁移/README 文档随 C1/T1 同步（可同 PR 或紧随）。
6. S2：按 S1 结论门控，设计稿评审后再实现。
7. T2：仅在 S2 提供稳定 context adapter 时实现外部上下文绑定 timer。
8. D2：网站双语更新最后落地。

每阶段以独立可回滚提交合入。D2 不得先于 C1/T1 的公开 API 与测试发布；S2 不得先于 S1
指南合并；T2 不得先于 S2 context adapter 验收；G1 由 heyaki 外部进度触发，不阻塞本计划主线。

## 收益与性能门槛

> 2026-09-22 账实核对：T1 精度门槛已记录并回填；C0 基线数量与 C1 内存/吞吐数字
> 两项度量缺口显式保留（需 heyaki 配合或本仓库补测），避免误读为"收益门槛已满足"。

- [ ] C0 记录 heyaki 当前私有取消点、facade timer 和必须保留在 asio strand 的 timer 基线数量；
  C1/T1/T2 各阶段验收只统计实际可迁移项，不以类型已发布代替业务收益。
  （2026-09-22 注记：全库无该基线数量记录，数据源在 heyaki 仓库，需下游配合）
- [ ] C1 合入前给出 cancellation state/registry 的单任务内存增量、并发取消吞吐和提交延迟回归；
  明确 registry 容量及终态保留策略，容量耗尽必须可观察且不得无界增长。
  （2026-09-22 注记拆分：registry 容量与终态保留策略已落地——task_cancellation.hpp
  kDefaultCapacity=65536、容量耗尽可观察、有界保留；剩余缺口为"单任务内存增量、
  并发取消吞吐"实测数字，可在本仓库补测后回填）
- [x] T1 使用 `benchmark_timer_precision` 记录句柄数量、取消/重排吞吐和到期抖动；超过既定预算时
  先保留兼容 API 并停止推广，不在同阶段无门槛扩展 timer thread 架构。
  （已记录基线对比：5ms delayed 平均到期误差旧实现约 5.5ms（10ms 轮询），registry +
  generation heap + 1ms 分片等待后约 0.9ms；单 timer thread 架构未扩展。）
- [ ] ⏸ 门控（heyaki 实测）：S2/T2 以 heyaki node/relay 参照用例统计纳入 admission/监控的 post 派发比例，以及可安全
  替换的 strand timer 数量；若收益不足以覆盖 adapter 复杂度，允许关闭 T2。

## 风险与待决项

> 2026-08-29 状态：以下前 8 项已由 C0 冻结决策解决（见设计稿 §11.2）并在 C1/T1
> 实现；其余为 S2/T2/G1 门控项，维持待决。

- [x] 排队取消后 future 的满足方式（C0 决）。（设计稿 §11.2 冻结：取消方满足 future，
  TaskCancelled 就绪）
- [x] 取消计数的字段归属（`CompletionStatus` 扩展还是独立 `CancellationStatus`）与是否需要
  单任务级取消历史查询（C0 定稿；归类为独立 lifecycle 计数已定）。
- [x] `submit_delayed` 返回类型兼容性：倾向新增 `submit_delayed_with_handle` 而非改签名
  （C0 确认）。
- [x] periodic 既有 task_id 与 `TimerHandle` 的统一程度，避免两套取消入口长期并存。
- [x] 共享 cancellation state 跨 scheduler/local queue/steal 副本的一致性、registry 有界保留和
  cancel/开始执行/完成的线性化点（C0 必须定稿，未定不得启动 C1）。
  （单一 phase CAS 仲裁，见 executor.hpp submit_tracked 注释）
- [x] token callable 的 overload resolution、参数位置、泛型 lambda 歧义和返回类型推导规则；
  默认选择显式 cancellable API 以降低源代码兼容风险。
- [x] 普通 `TimerHandle` 与 `ScopedTimerHandle` 的复制/移动/临时对象语义，防止意外析构取消。
- [x] T1 不具备外部 strand 所有权；迁移文档和网站不得把生命周期绑定等同于执行上下文绑定。
- [x] S2 是否进核心库：存在"指南 + 应用侧 adapter 已足够"的合理结论，不强推 API。
  （已决策并落地 SerialExecutionContext，docs/API.md 有专节）
- [ ] ⏸ 门控（heyaki 进度）：G1 依赖 heyaki M6/M7 外部进度，无固定时间表。
- [x] timer thread 单线程在大量句柄下的精度与延迟影响，以 `benchmark_timer_precision`
  回归数据决定是否需要分片。
  （已实现 generation heap + 1ms 分片等待，0.9ms vs 旧 5.5ms 基线已记录，2026-09-22 回填）
