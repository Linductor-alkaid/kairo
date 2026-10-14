# 优先级语义重构决策（2026-10-14）

## 背景

在 0.7.1 调度器开发过程中发现：`submit_priority` 路径虽然命名为普通提交 API，但实际上隐式依赖了与 `submit_auto` 共享的 PriorityScheduler 队列系统，这导致了以下问题：

1. **语义正交性破坏**：submit 系列（submit/submit_priority）定位为"不产出路由决策"的快速路径（见 scheduling_runtime.md §10.2），但 submit_priority 实际影响了调度顺序
2. **调度器抽象不完整**：优先级是调度策略维度（与 deadline/QoS/affinity/resource 正交），应由调度器管理，而非绕过调度器的独立 API
3. **架构演进不一致**：0.7.0 M3 已建立 `effective_priority_for()` 咨询机制，0.7.1 的 deadline/QoS 都通过 TaskBuilder 表达，唯独优先级保留独立 API

## 决策

**将优先级语义完全转移到调度器层，在 0.7.2 版本移除 `submit_priority` API。**

### 理由

1. **语义一致性**：优先级作为调度维度，应与 deadline/QoS/affinity/resource 在同一抽象层表达
2. **架构清洁性**：统一通过 IScheduler 接口管理所有调度策略，消除绕过调度器的特殊路径
3. **可迁移性**：TaskBuilder 已支持 priority 表达，迁移路径清晰且可自动化
4. **时机合理性**：
   - 0.7.x 聚焦调度模型演进，本重构正交于该主题但与之强耦合
   - 延后至 0.8.0 会让 0.7.x 系列内部调度语义不一致
   - 0.7.2 作为破坏性变更窗口（原规划为 additive + 修复，现调整为包含此唯一破坏性变更）

## 重构方案

### 迁移路径

```cpp
// 旧 API (0.7.1 及之前)
executor.submit_priority(TaskPriority::HIGH, []{ work(); });

// 新 API (0.7.2+)
executor.submit_auto(TaskBuilder([]{ work(); }).priority(TaskPriority::HIGH));
```

### 语义等价性

- 优先级 + EDF + FIFO 的调度逻辑完全相同
- `effective_priority_for()` 咨询机制保持不变
- PriorityScheduler 内部实现（四队列、aging）不变
- 性能无回归（submit_auto 裸 builder 已在验收线内）

### 实施时间线

- **0.7.2-rc1**：标记 `[[deprecated]]`，提供编译期警告，文档与示例全部迁移
- **0.7.2-rc2**：验证零警告（所有测试/示例通过）
- **0.7.2 正式版**：移除 API（保留 `submit_priority_scheduled` 内部实现供 facade 复用）

### 迁移支持

1. **自动化脚本**：`scripts/migrate_submit_priority.sh` 覆盖主要模式
2. **迁移指南**：`docs/MIGRATION.md` 0.7.2 节补充详细说明
3. **文档同步**：
   - `docs/API.md` §3.2 移除 submit_priority
   - website 教程页（priority.md）改写为 TaskBuilder 示例
   - 所有示例代码更新

## 影响面评估

- **测试代码**：256 处使用（可自动迁移）
- **示例代码**：4 处使用（手动更新）
- **用户代码**：通过 deprecated 警告周期（rc1 → rc2）提示迁移

## 验收标准

1. 256 处测试代码迁移完成，零 deprecated 警告
2. 配对 A/B 基准：迁移前后端到端吞吐 ±2% 内
3. 契约测试：优先级 + EDF 排序、aging、effective_priority_for 咨询逻辑不变
4. `scripts/check_docs_drift.sh` 通过
5. ASAN/TSAN 全绿

## 风险与缓解

| 风险 | 缓解措施 |
|------|---------|
| 用户代码破坏 | deprecated 警告周期；清晰的迁移指南；自动化脚本 |
| 性能回退 | 配对 A/B 基准验证；复用既有内部实现 |
| 语义漂移 | 契约测试覆盖调度逻辑；验收标准锁定行为 |
| 文档不同步 | check_docs_drift.sh 验证；website 教程全面改写 |

## 参考文档

- roadmap_v0.7.md §4.0：优先级语义转移计划
- scheduling_runtime.md §14：设计语义与实现要点
- executor.md（原始架构设计）§3：优先级调度器语义约定

## 批准

- **提议人**：架构评审（2026-10-14）
- **决策**：合理，纳入 0.7.2 开发计划
- **状态**：已批准，待实施
