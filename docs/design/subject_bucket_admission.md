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
- `set(subject, 0)` 解除后：已接纳任务正常释放到孤儿计数；重新配置产生
  新代计数（从 0 起）。
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
