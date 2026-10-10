---
title: 声明任务的期限、优先级与资源
description: 使用 0.6.0 调度运行时为任务声明 QoS、deadline、affinity 与资源需求，并注入自定义调度器。
---

# 声明任务的期限、优先级与资源

## 目标

越过手动挑选队列槽位的阶段：为任务声明它需要什么（优先级类别、完成期限、CPU 偏好、GPU 资源），让调度运行时把这些声明转化为准入与路由决策。

## 推荐方案

用 `kairo::task(...)` 构建任务并链式声明调度需求，然后通过 `submit_auto()` 提交。排序层次固定为：`优先级 -> EDF -> FIFO`。

<<< @/../examples/tutorial/14_scheduling_runtime.cpp{1-11,13-31}

```bash
./build/examples/tutorial/tutorial_14_scheduling_runtime
```

预期输出：

```text
qos task result=42
expired deadline rejected=yes
feasible deadline result=7
custom scheduler routes=1
```

## 每项声明的含义

- `qos(QosClass::Interactive)` —— 排队优先级 preset（`BestEffort/Standard/Interactive/Critical` 映射 `LOW/NORMAL/HIGH/CRITICAL`），仅在未显式设置 `priority()` 时生效。它不是延迟或带宽保证；确定性周期仍需 RealtimeQueue 意图。
- `deadline(time_point)` —— 真实调度输入，不是超时。同一优先级内按最早 deadline 先行（EDF）排序。已严格过期的 deadline 在提交时以结构化原因 `RoutingReason::DeadlineExpired` 拒绝（0.6.1）；开始执行时已错过的任务仍会运行并记录 `FailureKind::DeadlineMissed`——取消始终是请求，不是中断。
- `affinity(AffinityHint{{0, 1}})` —— advisory。调度器把请求与各后端的绑核集合对比；不相交时任务仍被接受，但决策被标记为降级（0.6.1）：`status = AcceptedDegraded`、`reason = AffinityMismatch`，并携带 `AffinityMismatch` 诊断位与人读 detail。不会为单个任务重新绑定 OS 线程。
- `resources(ResourceRequirements{...})` —— 与能力快照的可行性核对：`gpu_device` 不符以 `BackendUnavailable` 拒绝，内存超过可用量以 `CapacityPressure` 拒绝（两者均携带 `ResourceInfeasible` 诊断位，0.6.1）。它不是资源预留；并发提交在执行期仍可能竞争。

## 注入自定义调度器

调度决策位于 `IScheduler`（`<kairo/scheduler.hpp>`）之后。`DefaultScheduler` 组合意图路由与上述模型约束；`executor.set_scheduler(std::make_unique<MyScheduler>())` 可替换它（须在首次提交前调用；传 `nullptr` 恢复默认）。调度器只产出决策——提交仍由 facade 按各后端自己的协议执行。

每条决策都携带可机读的结果（0.6.1）：`RoutingDecision::status`（`Accepted` / `AcceptedDegraded` / `Rejected`）是接受/拒绝的权威判据，`reason` 与 `diagnostics` 位掩码解释原因，`detail` 仅供人阅读。通过 `get_scheduling_metrics()` 无需侵入调度器内部即可读取整体健康度（接受/降级/拒绝计数、deadline 拒绝与错过、affinity 不匹配、resource 拒绝）。将 `wants_feedback()` 覆写为 true 的调度器还会经 `on_task_completed()` 收到逐任务完成测量（`queue_wait_ns`、`execution_duration_ns`、最终 backend、deadline miss、failure kind）；`DefaultScheduler` 不消费反馈。

## 输入、所有权与失败

所有声明都是 `TaskOptions` 上的普通值，不持有你的对象引用。过期 deadline 的拒绝通过任务 future 以包含 "deadline" 的 `std::runtime_error` 送达，同时 `get_failure_status().submit_rejected_count` 增加。错过的 deadline（开始执行时观测）增加 `deadline_missed_count` 而不是拒绝任务。

## 检查理解

1. 一股 CRITICAL 优先级任务流占满线程池。一个 deadline 非常临近的 NORMAL 任务还能按时执行吗？
   不能——排序先看优先级再看 EDF，deadline 不会跨越优先级类别。
2. `qos(QosClass::Critical)` 会让线程池任务变成实时任务吗？
   不会。它只是把任务放进 CRITICAL 队列槽位；实时周期需要专用实时线程（见实时教程）。
3. 两个任务都声明 100 MB GPU 内存，而当前恰好剩 100 MB。两者都能拿到吗？
   可行性核对基于快照，不是预留——两者都可能通过准入，之后在执行期竞争。

## 相关页面

- [控制命令优先](/zh/tutorial/priority) —— 手动优先级槽位
- [延迟重试与健康检查](/zh/tutorial/delayed-and-periodic) —— 定时句柄
- [何时使用 AdaptiveScheduler](/zh/guides/adaptive-scheduling) —— 0.7.0 开发快照：反馈驱动的 CPU/GPU 选择、降载与提升
- [版本与迁移](/zh/reference/version-and-migration) —— 0.6.0 变更
