# 分主体在途份额 Admission 设计（A0 冻结，2026-10-28）

输入：Mira 台账 EXE-20261028-001（上游 issue #222）。#179 的总量有界
admission（`docs/design/bounded_admission.md`，D-1–D-5）提供了 facade 级
单值在途上限，但没有分桶维度：多个主体共享一个 Executor 时，一个主体可以
吃光全部全局在途额度，其他主体即使自身份额未被触碰也被全局拒绝。本设计在
同一接纳点上叠加**可选分主体桶维度**：每个已配置主体独立计数在途提交并以
份额上限约束，与全局上限同时生效。Executor 只负责按 key 记账与拒绝；份额
值的来源与动态调整由调用方决定。

## 冻结决策

### D-1 配置面

仅运行期接口（`ExecutorConfig` 无初始映射；份额天然随主体/会话动态来去）：

- `set_subject_max_in_flight_tasks(subject, max)`：配置/调整主体份额。
  `max == 0` 表示**解除**该主体约束（erase 表项；在途任务随终态释放计数，
  此后该主体不再可查询——语义声明，不做静默）。重复配置保留既有计数
  （调小不驱逐已接纳任务，只约束后续提交）；解除后重新配置的键从 0 起计
  （新代计数）。
- `get_subject_max_in_flight_tasks(subject)`：未配置主体返回 `0`
  （"未知即宽容"，与全局 `0 = 未启用` 同习惯）。
- `get_subject_in_flight_submissions(subject)`：该主体当前在途计数；
  未配置返回 `0`。
- `get_subject_quota_status()`：全部已配置主体的快照
  （`SubjectQuotaStatus{subject, max_in_flight_tasks, in_flight_submissions}`，
  按 subject 字典序；弱一致读）。

键承载：`TaskOptions::subject`（`std::string`，空 = 不分桶）+
`TaskBuilder::subject()` setter。仅在接纳点（`try_admit_submission`）消费，
不进入内部 `Task` 结构（与 `affinity`/`resources` 同层先例一致）。

### D-2 计数与释放

- 注册表：`detail::SubjectQuotaRegistry`（mutex + unordered_map，键空间以
  已配置集合为界，不存在无界增长）。每键计数为
  `shared_ptr<atomic<int64_t>>`：接纳点在注册表锁外 `fetch_add`；
  `AdmissionReleaser` 以共享所有权 keepalive——键解除后仍在途的任务释放时
  安全落账到孤儿计数。
- 接纳协议（沿用总量单次 `fetch_add` + 超限回滚的形状）：
  1. 分桶查表先于任何计数；未带键的提交不触碰注册表锁（零额外开销不变）。
  2. 全局维度先行（`max_in_flight_tasks` 语义逐位不变）：超限即拒绝，
     走既有总量拒绝路径。
  3. 全局通过后评估分桶：超限则回滚分桶与全局计数并拒绝。
  两维独立启用：全局 `0 = 未启用` 时不影响分桶维度单独生效，反之亦然。
  允许计数瞬时过冲，但任一维度的接纳决策不超过各自上限。
- 释放载体：`AdmissionReleaser` 泛化为 `{全局计数指针, 分桶计数
  shared_ptr}`，单一 `atomic_bool` CAS 恰好一次同时释放两维（各自可为
  null），析构兜底池丢弃路径。**全部六个提交路径调用点的释放接线零改动**
  （`release_admission` lambda 形状不变）。
- 不变式继承 `bounded_admission.md` D-2：**接纳先于取消 registry**、
  **释放先于 future 结算**（"计数先于 future"）；终态集合 = 正常完成、
  任务异常、排队取消、执行前超时、提交拒绝（池停止/registry 耗尽等）、
  context shutdown 拒绝、池丢弃、shutdown 排空。任何失败路径按获取的
  逆序释放，无泄漏窗口。shutdown 无显式清零，计数经每任务终态释放归零
  （与全局维度一致）。
- 并发一致性契约（配额热更新 vs 并发接纳，PR #223 审查冻结）：接纳的
  **快照点 = `find()` 的注册表临界区**——一次接纳在同一临界区内原子地
  取得 `{份额上限, 计数器}` 快照，随后的 `fetch_add` 与判定在锁外按该
  快照完成，且每条提交只做一次快照、只做一次判定（无重评、无 ABA）。
  由此：
  - `set()` 在某次接纳的快照点之前完成 → 该接纳必须使用新份额（注册表
    互斥量的 happens-before 保证；`max` 与 `counter` 同临界区读取，不
    存在新配额配旧代计数器的撕裂配对）；
  - 快照点之后落地的份额更新**不回溯已开始的接纳**——允许其按旧份额
    完成（弱一致）。瞬时超额上界 = 快照早于更新的并发在途接纳数，随
    终态释放自愈，后续提交一律按新份额判定；
  - 计数器生命周期：快照经 `shared_ptr` 取得所有权，erase/重配不使任何
    在途 `fetch_add`/释放悬空（孤儿计数见 D-5 代际语义）。

### D-3 拒绝分类

异常类型与 `FailureKind` 不变（不新造拒绝面，沿用 #179 D-3 三通道）：

- 拒绝**不抛出**；分桶拒绝的 future 以 `CapacityExhaustedException` 就绪，
  消息为 `Subject in-flight quota exhausted (key=…);
  subject_max_in_flight=…`（与总量消息可区分）。接纳点与 future 结算
  **共用同一异常对象**（`AdmissionDecision::rejection_exception`）。
- `ExecutorFailureStatus` 尾部追加 `subject_quota_exhausted_count`：分桶
  拒绝时与 `capacity_exhausted_count` **同时**递增（前者是后者的子集）；
  总量拒绝只递增后者。
- `ExecutorFailureEvent` 尾部追加 `std::string subject`：分桶拒绝携带键，
  空 = 总量拒绝。failure 回调与 recent-failures 据此程序化区分两维。

### D-4 覆盖路径（一期）

**带键**（`options.subject` 进入接纳点）：
`submit_auto(TaskBuilder)`、`submit_after(TaskBuilder)`、
`submit_after_with_handle(TaskBuilder)`（含单/多依赖重载）。

**不带键（仅全局兜底，键为空串）**：总量 admission 的其余覆盖路径——裸
`submit` 系、`submit_with_handle`、`submit_cancellable*`、`submit_on*`、
`submit_batch*`、`submit_auto(F&&, Args...)`、`submit_batch_priority`。
一期不为其增设携带 subject 的重载（文档声明，不做静默）。

`submit_delayed*` / `submit_periodic*`（timer 派发）、GPU（含
`CpuGpuTask`，其 options 不提供 subject）、realtime、Blocking I/O、
lockfree 本就不在总量 admission 覆盖范围，同样不带键。直接使用
`ThreadPool` / `PriorityScheduler` 的调用方不受保护。

### D-5 其他边界

- 未配置主体 = 仅全局约束（"未知即宽容"）；键空间以已配置集合为界。
- **代际语义（PR #223 审查冻结）**：
  - *活键原位缩放*：对已配置键 `set(N→M)` 保留既有计数器（**同一代**），
    调小不驱逐已接纳任务，只约束后续提交（后续接纳按新份额快照判定）。
  - *解除*：`set(key, 0)` 即刻 erase——**后续提交不再受该主体约束**；
    已接纳任务仍恰好一次释放，落账到孤儿计数（`shared_ptr` keepalive，
    最后一个 releaser 析构后回收）；解除期间该键查询返回 0（不可查询）。
  - *重建 = 新代*：解除后同名重配创建**新计数器、从 0 起计**，不继承
    旧代残量、不计入新代配额与查询。理由：给操作者一个确定性的"清除
    历史"杠杆（否则不可取消的在途残留会立刻吃光新份额）；继承语义需要
    解除后保留 tombstone 并做跨代释放路由，状态与开销均不划算。瞬时
    共存上界 = 旧代残量（≤ 旧份额）+ 新代份额，随旧代任务终态归零。
  - *查询面*：`get_subject_in_flight_submissions()` /
    `get_subject_quota_status()` 只反映**当前代**计数；跨代在途总量不
    承诺可观测（旧代计数随终态消亡；保留其可观测需要永久 graveyard
    状态，收益不抵成本）。
  - *不引入 generation 标识*：现有查询面 + 本契约已覆盖可诊断性；需要
    稳定视图的调用方在重配前后各自快照即可。新增标识属投机 API。
- 不为 CRITICAL 优先级或任何提交路径提供份额旁路。
- shutdown 与并发 submit：分桶拒绝语义与总量一致（池停止拒绝走既有
  on_rejected 语义并释放两维计数）。
- 份额压力一期不作为 scale-up 信号，仅可经
  `get_subject_in_flight_submissions()` / `get_subject_quota_status()` 观测。
- 无任何无界 fallback 绕过：拒绝是唯一过载出口。

## 验收（对齐 Mira 台账上游验收标准）

1. A/B 两主体各自份额内提交均被接纳，互不影响；A 超自身份额被拒
   （`CapacityExhaustedException` + `subject_quota_exhausted_count` 递增 +
   事件携带 `subject`）且 B 仍可接纳。
2. A+B 合计超全局上限时按既有总量语义拒绝（事件 `subject` 为空、消息为
   总量文本、只递增 `capacity_exhausted_count`）。
3. 分桶计数遵守"恰好一次接纳、恰好一次释放"：正常完成、任务异常、排队
   取消、执行前超时、池丢弃各终态后该主体在途计数归零且可再接纳。
4. future 就绪前分桶计数已释放（观测不变式）。
5. 份额热更新：调小不驱逐已接纳任务；`set(subject, 0)` 解除后不再约束。
6. 未配置主体仅受全局约束；全局未启用时已配置主体的份额仍独立生效。
7. shutdown 期间分桶拒绝语义与总量一致；`shutdown(true)` 排空后分桶计数
   归零。
8. `max_in_flight_tasks == 0` 且无任何份额配置时：全部既有测试行为不变，
   提交热路径零额外开销（不带键路径不触碰注册表锁）。
9. 配额热更新：`set()` 完成后**开始**的提交必须按新份额判定（严格可见）；
   与更新并发交错的已开始接纳允许按旧快照完成（弱一致），全部终态后该
   主体计数归零、无泄漏、无重复释放。
10. 代际语义：解除即刻解除后续提交的约束；解除后同名重建为新代、从 0
    起计；共存窗口内查询只反映当代计数，旧代任务终态释放不影响新代
    计数，全部终态后归零。

## 修订（2026-10-28，下游反馈）

以下修订来自下游对已交付能力真实使用形态的反馈（issue #222 评论），不改变
D-1～D-5 已冻结的计数、释放与拒绝语义，仅扩展入口面、补文档约定。

### R-1 裸形态带键入口

下游全部 `submit_auto` 调用均为裸 lambda 形态（`submit_auto([&]{...})`），
D-4 的带键路径（仅 TaskBuilder 形态）对其不生效，且**静默**——不抛错、
不计数、查询恒 0。修订：

- 新增公开重载 `submit_auto(SubjectKey, F&&, Args&&...)`，用法
  `ex.submit_auto(kairo::subject("key"), fn)`。与裸形态语义逐位一致
  （同一接纳点、同一路由决策与推断注解），仅把键送入接纳点；实现上
  `submit_with_rejection_observer` 增内部 subject 参数（公开 `submit()`
  恒传空串），两个裸形态共享 `submit_auto_impl`。
- 消歧安全：`SubjectKey` 构造**刻意 explicit**——若允许 `const char*`
  隐式转换，`submit_auto("key", fn)` 会因泛型重载对首参的精确匹配而
  静默错绑（键被当成 callable）；explicit 使该误用编译失败，强制经
  `subject()` 工厂表达。非模板具体首参在 C++ 重载偏序化中对
  `(SubjectKey, callable)` 调用精确胜出泛型模板。
- 诊断面：`SubjectQuotaStatus` 追加 `admitted_total`（当前代累计接纳数，
  与 in_flight 同一 shared 块、单次分配）。接入方验证配方：份额已配置但
  `admitted_total` 停在 0 = 键未流过接纳点。键化接纳路径一条 relaxed
  `fetch_add`，未带键路径零开销不变。
- 仍不带键（声明，不做静默）：公开 `submit()`、`submit_after` 裸形态、
  `submit_cancellable*`、`submit_on*`、`submit_batch*`；是否扩展属后续
  修订（下游未请求）。

### R-2 键的敏感性约定

分桶键被**逐字复制**进三处可观测面：拒绝异常消息
（`Subject in-flight quota exhausted (key=…)`）、`ExecutorFailureEvent::subject`
与 `ExecutorFailureStatus`/`SubjectQuotaStatus` 快照，均会流向日志、可能
被宿主呈现到界面。约定：

- 键必须是**不可识别的派生标识**（内部编号、哈希、代号），不得使用账号、
  租户名、外部 ID 等带身份含义的原文；
- 键空间以已配置集合为界、不存在无界增长；键随主体生命周期增删的清理
  责任在调用方（`set(key, 0)` 移除配置项）。

### R-3 跨代低报的显式警示

D-5 代际语义的一个后果提升为 API 层显式注意事项：解除后同名重建的短暂
窗口内，`get_subject_in_flight_submissions()` **低报**真实在途，差额 =
旧代残量（上界 = 解除前的份额），方向恰好与配额判定的安全侧相反。处置：
API.md §3.12 给出显式 ⚠ 注意事项与监控口径（超额上界 + 自愈时间，弱一致
契约 D-2 保证自愈）；不引入 retired 计数暴露——跨代可观测需要永久
graveyard 状态，收益不抵成本（维持 D-5 裁定，下游已按该口径设定监控）。
