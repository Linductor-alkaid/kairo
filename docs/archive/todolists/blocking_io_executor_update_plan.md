# Blocking I/O Executor 实施与使用手册更新计划

本文档把 [阻塞 I/O 执行器扩展设计](../design/blocking_io_executor.md) 拆分为可提交、
可验证的实现计划。目标是在不改变现有 async、实时、GPU 和 `executor::comm` 语义的前提下，
新增用于长期、可中断阻塞循环的 `BlockingIoExecutor`。具体协议和设备接入不属于本计划。

本计划中的网站工作必须跟随公开 API、编译示例和测试完成后落地；设计稿不是公共 API，不能先在
使用手册中承诺尚未实现的类型或实时性质。

---

## 当前基线

- [x] `ThreadPoolExecutor` 管理有限的异步任务；不适合永久占用 worker 的 transport loop。
- [x] `RealtimeThreadExecutor` 管理固定周期、短回调和有界任务消费；阻塞 I/O 会计入周期预算。
- [x] `ExecutorManager` 已统一管理默认 async、RT、GPU 的注册与 shutdown。
- [x] `executor::comm` 已提供 `MpscChannel`、`LatestMailbox`、`RealtimeChannel`、`DoubleBuffer`、
  `PhaseGate` 和本地 `CommStats`/`CommEventCallback`。
- [x] 现有通信事件默认独立于 `ExecutorFailureStatus`。
- [x] 网站已有中英文“实时与通信”专题、导航、编译并 smoke-test 的教程机制，以及内容维护规则。
- [x] 已具备针对专属、可阻塞、可唤醒 worker 的 executor 类型、状态和 facade API；协议与设备接入仍由调用方实现。

## 实施原则与不可变约束

1. **接口分离**：I/O executor 不继承 `IAsyncExecutor` 或 `IRealtimeExecutor`，不提供通用 `push_task()`。
2. **停止必须可中断**：`stop_token` 只表达请求；每个 worker 必须提供 `wakeup()` 来解除阻塞等待。
3. **普通调度默认值**：I/O worker 默认不申请 `SCHED_FIFO`、不自动绑核、不自动 memory lock。
4. **控制/I/O 解耦**：接收、校验、解码和发布在 I/O worker；控制在 RT loop；重计算在 async/GPU。
5. **通信语义不混淆**：状态、命令、诊断和配置各自选择 mailbox/channel/snapshot；不以“队列最快”为选择依据。
6. **错误分域**：启动和注册失败进入 `ExecutorResult`；运行期 transport/协议/背压进入 I/O 或通信诊断面。
7. **不 detach**：未 join 的 worker 不得脱离 Executor 生命周期，防止 transport 与数据面 use-after-free。
8. **文档以事实为源**：公开示例必须编译；中英文页面使用相同源码和命令；完整签名仍以 `docs/API.md` 为准。

---

## 阶段 I0：接口、配置和状态骨架

### 任务

- [x] 新增公开头 `include/executor/blocking_io.hpp`。
- [x] 定义 `BlockingIoConfig`：线程名、可选 affinity、可选 memory lock、启动 ready 超时。
- [x] 定义 `IBlockingIoWorker`：
  - [x] `run(std::stop_token)`。
  - [x] `wakeup() noexcept`。
  - [x] 由执行器以一次性 ready signal 确认线程属性完成后进入 worker。
- [x] 定义 `IBlockingIoExecutor`：`start()`、`request_stop()`、`stop()`、`get_name()`、`get_status()`。
- [x] 定义 `BlockingIoExecutorStatus` 和 stop reason：运行/ready/stop 请求、线程属性结果、wakeup 计数和最后错误。
- [x] 在 `include/executor/types.hpp` 中增加稳定状态字段；协议或设备特有统计不进入核心库。
- [x] 编写 config 校验函数：空名、负 timeout、空 worker 均以 `InvalidConfig` 拒绝。

### 验收

- [x] 公共头可独立包含，且不引入协议、设备或硬件 SDK 依赖。
- [x] 所有状态字段有初始化值和线程安全读取规则。
- [x] API 明确声明：没有有限等待或可唤醒路径的 worker 不满足契约。

### 测试

- [x] 新增 `test_blocking_io_types.cpp`：默认值、状态快照和公开头可独立编译。
- [x] 编译测试：消费者只依赖公开头，不需要链接实际 transport。

---

## 阶段 I1：单 worker 执行器与严格停止语义

### 任务

- [x] 新增实现文件 `src/executor/blocking_io_executor.hpp/.cpp`。
- [x] 内部使用 `std::jthread`，由 executor 唯一拥有 worker 和线程。
- [x] 启动路径：设置运行状态 -> 创建线程 -> 在线程内配置名称/affinity -> ready -> 执行 `run(stop_token)`。
- [x] 启动 ready 超时会 request-stop、wakeup、join、保存 `StartFailed` 原因并回滚运行状态。
- [x] `request_stop()` 按固定顺序执行：标记 stop -> `request_stop()` -> `worker.wakeup()`。
- [x] `stop()` 对已 join worker 幂等，串行化 stop，并在不持有 registry 锁时 join。
- [x] 捕获 `run()` 未处理异常；区分“请求后正常返回”“未请求的提前返回”“worker 异常”。
- [x] 不实现 join timeout 后 detach。
- [x] 仅在用户显式配置时执行 affinity 或 memory lock；记录请求是否真正生效。

### 验收

- [x] 没有阻塞 I/O 时，start/stop/析构均完成资源收敛；ASAN 执行路径通过。
- [x] 受控阻塞 worker 收到 wakeup 后可退出且 `stop()` 返回。
- [x] 多个线程同时调用 stop、重复启动、从 worker 自身回调请求停止都有定义行为，不产生双 join 或 `std::terminate`。
- [x] `std::jthread` 只存在于实现层，对调用方不泄漏线程所有权。

### 测试

- [x] 新增 `test_blocking_io_executor.cpp`。
- [x] 覆盖线程创建失败回滚、ready 超时、worker 启动异常、非预期返回、运行期异常。
- [x] 用 condition variable mock 实现可控阻塞 worker，验证 `wakeup()` 与 join。
- [x] 验证 stop 幂等、并发 stop、析构清理、自停止和状态计数。
- [ ] TSAN 测试生命周期状态读取与并发 stop；ASAN/LSAN 覆盖异常启动和停止路径。
  当前环境已完成 TSAN 编译与 ASAN 执行；TSAN runtime memory mapping 和 LSAN ptrace 扫描受宿主环境限制，留给 CI/目标机执行。
  （2026-09-22 注记：CI TSAN job（c-cpp.yml）目标清单目前未含 test_blocking_io_executor，
  存在缺口——补入或记录排除原因前本项无法关闭）

---

## 阶段 I2：ExecutorManager 与 Facade 集成

### 任务

- [x] 在 `ExecutorManager` 增加 I/O executor registry、访问锁、注册/查询/列举 API。
- [x] 在 `Executor` facade 增加：
  - [x] `register_blocking_io_worker[_ex]()`。
  - [x] `start_blocking_io_worker[_ex]()`。
  - [x] `stop_blocking_io_worker()`。
  - [x] `get_blocking_io_worker_status()` 与名称列表查询。
- [x] 注册时检查 RT、GPU、I/O 名称冲突；第一版在 facade 做跨 registry 检查。
- [x] 复用 `ExecutorResult` / `ExecutorErrorCode`：`InvalidConfig`、`DuplicateName`、`NotFound`、`StartFailed`。
- [x] 只对注册/启动等 executor 生命周期失败记录 facade failure；worker 运行期异常不记为 task exception。
- [x] 更新 `ExecutorManager::shutdown()`：先移出 I/O executor 所有权并释放 registry 锁，再 request-stop/wakeup/join。
- [x] 保持 `Executor::shutdown()` 先停止 timer thread 的当前语义；I/O join 对 `shutdown(false)` 仍是强制的生命周期收敛。
- [x] 审查单例 `atexit` 与实例模式析构路径；二者复用 `ExecutorManager::shutdown()`，实例析构已有 I/O worker 测试，单例 atexit 由既有 Facade 回归覆盖。

### 验收

- [x] I/O worker 可以通过 facade 得到 RT/GPU 一致的命名、注册、启动、状态和停止体验。
- [x] shutdown 不在 I/O registry 锁内调用外部 worker 代码或 join，避免锁级反转。
- [x] 既有 async/RT/GPU API 与 Facade 回归测试保持行为兼容。

### 测试

- [x] 新增 facade/manager 注册、重名、未找到、重复启动、状态查询测试。
- [x] 新增 I/O 注册失败和运行期 worker 异常的 facade 诊断隔离断言。
- [x] 添加 timer、RT、I/O、async 同时存在的 mixed shutdown 测试；GPU 仍由可选后端测试覆盖。
- [ ] 在 registry 操作和 stop 并发时运行 TSAN。
  （2026-09-22 注记：与 I1 的 TSAN 项同属 CI 缺口——c-cpp.yml TSAN job 目标清单
  未含 test_blocking_io_executor，补入或记录排除原因后关闭）

---


## 阶段 D1：源码、API 与迁移文档

本阶段仅覆盖 `executor` 核心库已实现的 I/O worker API、迁移说明和独立示例；不包含外部 transport adapter、应用集成或设备部署内容。

### 任务

- [x] 更新 `docs/API.md`：接口签名、`BlockingIoConfig`、状态字段、错误码、启动/停止契约、普通调度默认值。
- [x] 更新 `docs/MIGRATION.md`：从裸 `std::thread`/`std::jthread`、线程池常驻 lambda、RT callback 内阻塞 poll 迁移到 I/O worker。
- [x] 更新 `README.md` 与 `README_zh.md` 的能力边界和入口链接；不把它宣传为硬实时 transport。
- [x] 保持 [阻塞 I/O 执行器扩展设计](../design/blocking_io_executor.md) 为决策依据，并标注核心库与应用集成边界。
- [x] 为每个公开状态与异常路径补充 API 注释和可搜索术语：worker、wakeup、bounded wait、stop reason。

### 验收

- [x] 签名、默认值、错误码与测试一致；不存在仍要求读者猜测的 shutdown 语义。
- [x] 迁移文档明确哪些旧模式可保留，哪些模式会占用线程池或破坏 RT 周期。

---

## 阶段 D2：使用手册和网站更新计划

网站采用当前 VitePress 双语结构。内容更新遵循现有设计约束：保持文档型、任务导向、无营销式
页面重构；使用现有主题、导航与版式，不新增装饰性页面或视觉组件。每个公开示例都必须来自编译
并 smoke-test 的源码，不能在中英文页面维护两份手工复制的 C++ 代码。

### 页面与导航

- [x] 新增中文页面 `website/zh/realtime-and-communication/blocking-io-workers.md`，说明使用边界与库级生命周期契约。
- [x] 新增英语对应页面 `website/en/realtime-and-communication/blocking-io-workers.md`，使用同一示例、命令与 API 名称。
- [x] 更新中英文 `realtime-and-communication/index.md` 与两个 locale sidebar，加入 I/O worker 页面入口。
- [x] 更新 `website/translation-status.md` 与中英文维护页的事实源，标注双语页面与教程 `12`。

### 教程与可运行事实源

- [x] 新增编译/烟测示例 `examples/tutorial/12_blocking_io_worker.cpp`，使用受控条件变量 mock worker。
  - [x] mock worker 在受控条件变量上阻塞。
  - [x] 演示 facade 注册、启动、读取状态、request stop/wakeup/join。
  - [x] 验证状态计数和 clean shutdown，不以 sleep 作为正确性的唯一依据。
- [x] 在 `examples/tutorial/CMakeLists.txt` 注册示例与 CTest smoke test；命名和现有 tutorial 编号保持一致。
- [x] 网站页面用 VitePress `<<< @` 嵌入已编译示例的最小连续行段，并链接完整源码；不引入第三方 transport SDK。
- [x] 示例保留在实时与通信专题页，不打乱现有业务教程索引。

### 既有页面的交叉更新

- [x] 更新中英文 `realtime-control.md`、`guides/migrating-existing-threads.md` 与 `guides/concurrency-antipatterns.md`：说明永久阻塞循环不属于 `cycle_callback` 或线程池任务，并链接 I/O worker 页面。
- [x] 站点页面链接 `docs/API.md` 的 I/O worker 章节，而不重复完整签名表。

### 网站验证

- [x] 运行网站构建和链接检查，确认 base `/executor/` 下中英文新路由、sidebar、语言切换和交叉链接有效。
  （docs.yml 每次 CI 运行 docs:check + docs:build，0.5.0 已发布，2026-09-22 回填）
- [ ] 检查窄屏与宽屏下的长 API 名、表格和 code block 不溢出；不改变既有主题或页面信息架构。
  （2026-09-22 迁移：人工视觉核对项移入 docs/RELEASE_CHECKLIST.md 发布核对流程）
- [x] 确认每个 `<<< @` 指向存在的源码和稳定行段；示例修改时同步复查页面片段。
- [x] 执行新教程的 CTest smoke test 与 Blocking I/O runtime 测试。
- [ ] 在 release 文档检查中确认版本范围、API、迁移材料、教程、中文页、英文页和 translation status 同步。
  （2026-09-22 迁移：发布核对项移入 docs/RELEASE_CHECKLIST.md，随每次发布重跑）

### 手册完成标准

- [x] 用户能判断“这是固定周期控制、有限异步工作，还是专属可阻塞 I/O”。
  （blocking-io-workers.md 开篇三种模式判别，2026-09-22 回填）
- [x] 用户知道 `stop_token` 不能独自中断底层 I/O，并能选择 wakeup fd 或有限 timeout。
  （同页"stop token 本身不能中断任意外部等待……使用有限 timeout"，2026-09-22 回填）
- [x] 用户能把状态、命令和诊断分别接到正确的通信组件，并能观测 age、drop、error 和 shutdown。
  （本页未覆盖部分由 comm 专题页 channels/state-and-phases/observability 覆盖，
  2026-09-22 回填）
- [x] 用户不会从手册推导出未承诺的硬实时、无锁或自动重连保证。
  （同页"本库刻意不决定……重连、设备安全动作"，2026-09-22 回填）

---

## 建议合并顺序

1. I0：公开类型和配置，先评审 API 边界。
2. I1：独立执行器与严格停止单测，先证明可 join。
3. I2：Manager/Facade 集成及 shutdown 并发安全。
4. D1：API、迁移、README 与设计状态同步。
5. D2：独立 mock 教程、双语网站、导航和链接检查。

每阶段应以独立可回滚提交合入。D2 不得先于 I2 的公开 API 与 I1 的停止契约测试发布；文档不得把核心库的 worker 生命周期接口表述为外部 protocol adapter 或应用部署方案。

## 风险与待决项

- [x] 明确 I/O worker 的 ready 仅表示线程属性和 `run()` 入口已建立；transport 就绪、首帧接收和业务状态由使用方定义。
  （blocking-io-workers.md"ready 只表示 executor 线程设置完成"，2026-09-22 回填）
- [x] 评估全局 name registry 是否作为本计划的一部分落地；若延后，必须有跨 registry 冲突测试。
  （已按"延后 + 冲突测试"收敛：test_blocking_io_executor.cpp 跨执行器重名返回
  DuplicateName；facade 侧检查见 executor.cpp，2026-09-22 回填）
- [x] 保持 wakeup 平台无关：具体 fd、eventfd、pipe 或系统 API 由 worker 实现，不进入核心库公开 API。
  （blocking_io.hpp 纯虚 `virtual void wakeup() noexcept = 0`，无平台原语泄漏，
  2026-09-22 回填）
