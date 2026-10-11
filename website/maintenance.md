---
title: 内容维护
description: Executor 使用手册的事实源、核对节奏、反馈入口与内容成熟度说明。
sidebar: false
aside: false
---

# 内容维护

本手册以“用户能完成任务并处理失败”为完成标准，而不是以公开接口是否被提到为标准。当前内容基线为 `v0.7.0` 与后续 `master` 开发；页面中的待发布能力不能视为已有稳定版本承诺。

<div class="maintenance-hero">
  <p class="maintenance-eyebrow">CURRENT BASELINE</p>
  <p class="maintenance-version">v0.7.0 + master 开发</p>
  <p>稳定 tag 发布、公开 API 行为变化或教程示例修改时，必须重新核对对应责任域。</p>
</div>

## 内容如何保持可信

<div class="maintenance-grid">
  <section class="maintenance-card maintenance-card-coral">
    <span>01</span>
    <h3>教程代码可执行</h3>
    <p>主线片段来自 <code>examples/tutorial/</code>，由 CMake 编译和 smoke tests 持续检查。</p>
  </section>
  <section class="maintenance-card maintenance-card-cream">
    <span>02</span>
    <h3>行为回到事实源</h3>
    <p>签名与语义以公开头文件、测试和 API 文档为准；网站负责解释用户决策，不复制第二份参考手册。</p>
  </section>
  <section class="maintenance-card maintenance-card-forest">
    <span>03</span>
    <h3>发布时重新核对</h3>
    <p>版本、构建、迁移、性能和平台差异随发布清单同步，不能沿用未经复测的结论。</p>
  </section>
</div>

## 责任域与事实源

| 责任域 | 覆盖页面 | 首要事实源 | 何时必须复核 |
| --- | --- | --- | --- |
| 快速开始与普通任务 | 首页、`zh/quick-start/`、`zh/tutorial/` | `include/executor/executor.hpp`、`examples/tutorial/01`–`06` | Facade、future、等待或生命周期变化 |
| 可靠性 | `zh/reliability/` | failure/status 类型、相关测试与教程 `06` | 事件类型、计数器、监控语义变化 |
| 实时与通信 | `zh/realtime-and-communication/` | realtime 与 blocking-I/O Facade、`include/executor/comm/`、教程 `07`–`08`、`12` | 周期、worker 生命周期、背压、drop 或平台行为变化 |
| GPU | `zh/gpu/` | GPU public API、教程 `09`、硬件报告 | 后端、降级、设备或性能结论变化 |
| 高级与参考 | `zh/advanced/`、`zh/reference/` | 公开头文件、`docs/API.md`、设计与测试 | 稳定边界、内部执行路径或迁移策略变化 |
| 站点交付 | 主题、导航、404、部署 | `website/`、docs workflow、站点检查脚本 | 路由、base URL、依赖或 Pages 流程变化 |

## 一次内容修改的完成标准

- 从用户问题开始，说明为什么选、何时不选和失败如何观察。
- 代码来自已编译示例，或有独立的片段验证路径。
- 对优先级、超时、实时性和性能不作超出实现与测量的承诺。
- 新入口同时出现在侧边栏、相关上一层页面和站内搜索可达内容中。
- 运行网站构建与链接检查；涉及示例时运行对应 CTest。
- 更新页面版本口径，并在需要时执行[发布文档同步清单](https://github.com/Linductor-alkaid/kairo/blob/master/docs/RELEASE_CHECKLIST.md)。

## 内容成熟度

当前站点已经具备完整主题覆盖和可构建框架，但“出现了页面”不等于“用户已经能深入掌握”。后续迭代优先补齐：需求判断、完整业务演进、错误场景、容量与关闭策略、可复现性能结论，以及平台和硬件差异。

<div class="maintenance-callout">
  <div>
    <strong>发现讲不清、复现不了或行为已变化？</strong>
    <p>请附上页面 URL、使用版本、最小复现、实际行为与期望行为。若问题来自真实接入场景，也请说明任务规模、线程模型和关闭方式。</p>
  </div>
  <a href="https://github.com/Linductor-alkaid/kairo/issues/new/choose">提交文档问题</a>
</div>
