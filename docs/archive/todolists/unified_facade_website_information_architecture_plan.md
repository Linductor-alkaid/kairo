# 统一 Facade 网站信息架构调整计划

本文档定义 `0.3.1` 统一 Facade 与自动路由完成后，使用手册应如何调整入口、阅读顺序和专题定位。它是[统一 Facade 与自动路由设计](../design/unified_facade_and_auto_routing.md)的文档交付计划，不改变 C++ API，也不替代 `docs/API.md` 的完整签名说明。

## 背景与问题

旧手册按执行器和功能分区组织：线程池、实时、GPU、Blocking I/O、无锁和通信在导航中相对并列。该组织适合已经知道所需执行模型的专家，却把后端选择的复杂性提前暴露给普通开发者。

`0.3.1` 已提供按意图的统一入口：

- 普通有限工作可通过 `submit_auto(lambda)` 使用默认异步路径；
- CPU/GPU 双实现可通过 `cpu_gpu_task()` + `submit_auto()` 交由可解释路由处理；
- 有界投递使用 `dispatch_auto()`，明确返回接收结果；
- 长期 Blocking I/O 使用 `start_worker()`，明确返回生命周期 handle。

因此网站的默认叙事应从“先选择执行器”调整为“先描述业务工作，再按明确语义下钻”。这不是把所有后端包装成同一模型；相反，网站必须让读者尽早理解 completion、admission 和 lifecycle 是三种不同结果。

## 目标与非目标

### 目标

1. 普通开发者完成构建后，可从 `submit_auto(lambda)` 开始，不必先理解实时、GPU、无锁或 Blocking I/O 执行器。
2. 以业务约束而非类名引导选型：是否要返回结果、是否有独立 CPU/GPU 实现、是否需要有界接收、是否是长期可中断循环。
3. 专家用户仍能快速找到显式后端 API、容量、部署和性能细节，不把专业控制隐藏或弱化。
4. 清楚区分三种结果模型：future completion、bounded admission、worker lifecycle。
5. 中英文站点使用相同的信息架构和事实来源；在英文版本未完成前，翻译状态必须明确标记。

### 非目标

- 不声称 `submit_auto()` 能自动证明 callable 的实时安全、线程安全、GPU 内存所有权或 I/O 可中断性。
- 不将 `dispatch_auto().accepted` 写成任务完成、实时周期执行或持久化成功。
- 不将 `WorkerHandle` 写成一次任务的 completion handle。
- 不删除既有显式 API、GPU、实时、无锁或 Blocking I/O 专题。
- 不在快速开始中教授所有 scheduler 阈值、队列容量和平台权限细节。

## 读者路径

| 读者/问题 | 默认入口 | 返回模型 | 何时进入专题 |
| --- | --- | --- | --- |
| 普通有限 CPU 工作 | `submit_auto(lambda)` | `std::future<T>` 完成或异常 | 需要 priority、delay、periodic、batch 或 dependency |
| 独立 CPU/GPU 实现 | `cpu_gpu_task()` + `submit_auto()` | `std::future<void>` 完成或异常 | 需要注册、诊断或调节 GPU 后端 |
| 已验证的 MPSC 低延迟路径 | `dispatch_auto(LowLatency)` | `DispatchResult::accepted` | 需要无锁容量、对象池、吞吐或关闭细节 |
| 周期实时队列 | `dispatch_auto(RealtimeQueue)` | `DispatchResult::accepted` | 需要周期预算、drop/backpressure 或部署权限 |
| 长期可中断 I/O | `start_worker(BlockingWorkerSpec)` | `WorkerHandle` 启动和生命周期 | 需要 wakeup、stop token、协议和部署细节 |

所有路径均应链接到 `RoutingDecision`、failure event、状态计数或 capability snapshot 的相应观察入口。页面应先解释调用方能确认什么，再解释实现使用什么后端。

## 信息架构调整

### 新手主线

1. 首页与“Executor 是什么”：从“多个执行器”改为“从一个业务任务开始；只有意图明确时才选专用路径”。
2. 快速开始“第一个任务”：主示例改为 `submit_auto(lambda)`，展示 future 与默认路由决策；保留 `submit()` 作为显式线程池控制入口。
3. 场景指南“如何选择提交接口”：以结果模型作为第一分支，再以业务约束选择 `submit_auto`、`dispatch_auto`、`start_worker` 或显式 API。
4. 增加“执行模型与路由边界”桥接页：解释意图、路由决策、能力快照，以及 future/admission/lifecycle 的不可混用边界。

### 专家下钻

- GPU 专题的自动调度页分为新 `CpuGpuTask` 路径和 legacy 四参数 `submit_auto` 路径，明确后者在 `0.3.x` 只做兼容维护。
- 实时与无锁专题顶部加入“仅在已有周期/有界背压/单消费者约束时进入”的提示，并链接回提交接口选型页。
- Blocking I/O 专题以 `start_worker()` / `WorkerHandle` 为推荐入口；显式 register/start/stop API 作为渐进迁移与诊断 escape hatch。
- 可观察性专题新增 routing decision 与 failure event 的职责对比；不要把允许 CPU fallback 计为用户任务失败。
- 版本迁移页将 `0.3.1` 明确为新手默认入口变化，但强调既有显式 API 无行为变化。

### 导航原则

- 顶层导航继续突出“快速开始”“场景指南”“循序教程”；“专题”是按需下钻区，不是新手的第一站。
- 快速开始和场景指南内避免直接把读者送入 `IRealtimeExecutor*`、`LockFreeTaskExecutor` 或 GPU executor 注册页。
- 每个专家页首段回答两件事：何时应该进入本页，以及何时应返回默认 Facade 路径。
- 专题之间使用“下一步”链接，不通过重复完整选型表制造多套规则。

## 页面改造清单

### 阶段 A：中文主线与桥接页

- [x] 更新 `website/zh/getting-started/what-is-executor.md`：默认一次性工作改为 `submit_auto`，重写五类默认路径表。
- [x] 更新 `website/zh/quick-start/first-task.md` 与其可运行教程示例：展示 `submit_auto(lambda)`、future 和 routing decision。
- [x] 更新 `website/zh/guides/choosing-submit-api.md`：先按 future/admission/lifecycle 分流，再补 priority、delay、batch、dependency 等细分。
- [x] 新增 `website/zh/guides/execution-models-and-routing.md`：作为默认 Facade 与专家专题之间的桥接页。
- [x] 更新 `website/.vitepress/config.mjs` 中文导航与 sidebar：将桥接页放入场景指南靠前位置。

### 阶段 B：中文专家专题重定位

- [x] 更新 `website/zh/gpu/automatic-scheduling.md` 和 GPU 索引：主写 `CpuGpuTask`，隔离 legacy overload 的兼容说明。
- [x] 更新 `website/zh/realtime-and-communication/realtime-control.md`、`capacity-and-alerting.md`：补 `RealtimeQueue` 有界接收与不等于完成的语义。
- [x] 更新 `website/zh/advanced/lockfree-and-performance.md`：补 `LowLatency` 显式 opt-in、单消费者和 admission 边界。
- [x] 更新 `website/zh/realtime-and-communication/blocking-io-workers.md`：以 `start_worker` 为首选（已完成），并检查索引页与教程链接。
- [x] 更新 `website/zh/reliability/failure-observability.md` 和 `website/zh/reference/version-and-migration.md`：路由/失败职责边界与 `0.3.1` 迁移策略。

### 阶段 C：英文同步与发布准备

- [x] 将阶段 A、B 的信息架构和事实同步到对应 `website/en/` 页面；不逐字翻译，但保持 API 语义与路径一致。
- [x] 更新英文导航的 release 标识至 `v0.3.1`，前提是版本已正式发布。
- [x] 在 `website/translation-status.md` 中更新每个改动页面的 published/needs-translation 状态；不得把未同步页面标为 Complete。
- [x] 检查网站内链、侧边栏顺序、代码片段来源和中英文切换目标。

## 内容规则与示例策略

1. 代码片段优先引用 `examples/tutorial/` 中可编译的完整示例；若新手示例改用 `submit_auto`，先更新示例和 smoke test，再更新网页。
2. 快速开始只展示普通 future 路径；`dispatch_auto` 与 `start_worker` 只在选择页和专门场景中首次出现。
3. 每次首次出现 `DispatchResult` 时都写明“accepted 不等于 completed”。每次首次出现 `WorkerHandle` 时都写明“它表示 worker 生命周期”。
4. `get_executor_capabilities()` 一律称为建议性快照，提醒实际投递仍可能被并发 stop、队列满或对象池耗尽拒绝。
5. 允许 CPU fallback 使用 `RoutingDecision::fell_back` / `FallbackPolicy` 解释，不将其描述成任务异常；`NoFallback` 和 `RequireRequestedBackend` 的拒绝应链接到 future/failure event 观察方式。
6. 保持网页设计约束：内容更新不引入新的页面视觉组件；如需新增视觉结构，先遵循 `.agents/DESIGN.md`。

## 验收标准

> 2026-09-22 账实核对：第 2–5 项页面侧证据已就位并回填；第 1、6 项分别为
> 外部用户效果验收（⏸ 门控）与 CI 持续门禁，不作一次性勾账。

- [ ] ⏸ 门控（外部用户）：新用户从首页到第一个任务无需阅读任何专用执行器页面，即可理解并运行 `submit_auto(lambda)`。
  （结构证据：website/index.md hero 与 zh/quick-start/first-task.md 主示例均为 submit_auto）
- [x] 选择页能让读者在一分钟内区分 future completion、bounded admission 和 worker lifecycle，并链接到对应观察方式。
  （zh/guides/choosing-submit-api.md 开篇三种结果模型表格 + 下钻条件，2026-09-22 回填）
- [x] 实时、无锁、GPU 和 Blocking I/O 页面均在首屏说明适用前提及不应使用的情况。
  （realtime-control.md、lockfree-and-performance.md、blocking-io-workers.md 首屏
  均有"何时使用/不应使用"声明，2026-09-22 回填）
- [x] legacy CPU/GPU overload 的文档不再被误读为新代码默认路径。
  （zh/gpu/automatic-scheduling.md 设"兼容路径"节并指明新代码用 cpu_gpu_task()，
  2026-09-22 回填）
- [x] 中英文相同路由页的 API 语义、代码来源、版本标签和导航顺序一致，或翻译状态明确声明缺口。
  （website/translation-status.md 全主题组 Complete 并定义 fallback；版本标签
  已于 2026-09-22 统一为 v0.5.0）
- [ ] 网站构建、链接检查及教程 smoke test 通过；文档改动不引入不可运行的独立代码副本。
  （CI docs.yml 每次 PR 强制 docs:check + docs:build + ctest -L tutorial，持续性门禁）

## 风险与决策点

- `submit_auto(lambda)` 作为默认入口会提高入门一致性，但用户仍需要知道何时显式选择 `submit()`（例如已有线程池语义、专家调试或兼容代码）。页面必须保留这个 escape hatch。
- 不能把“自动路由”宣传为性能自动优化。普通 `Auto` 不会偷偷选择无锁或实时后端，GPU 只接受显式双路径任务。
- 英文站点当前以 `v0.3.0` 为稳定基线。若 `0.3.1` 尚未发布，可先完成中文信息架构并将英文标记为待同步；不得将 master API 写成稳定发布承诺。
- 信息架构重构应分批发布：先改变入口与选择页，再调整专题，最后统一英文和版本标识，避免读者在中途遇到互相矛盾的导航。
