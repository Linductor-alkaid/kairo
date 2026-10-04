---
title: 首发决策记录
---

# 首发决策记录

| 事项 | 决策 | 理由 |
| --- | --- | --- |
| 站点名称 | Executor 使用手册 | 与仓库名称一致，清楚表明教学用途。 |
| 受众承诺 | 十分钟内完成第一个可观察的异步任务 | MVP 的最短成功路径。 |
| 源码目录 | `website/` | 与 C++ 源码、示例和参考文档隔离。 |
| 工具链 | VitePress 1.6.3、Node.js 20 LTS、npm | 静态生成、内置搜索和低维护成本。 |
| URL | GitHub Pages 项目站点，`/kairo/`；暂不使用自定义域名 | 与当前仓库 `Linductor-alkaid/kairo` 对应，避免首发依赖 DNS 配置。 |
| 语言策略 | 根路径为中文首页；内容使用 `/zh/`，预留对称的 `/en/` | 根路径不做浏览器语言跳转，避免不可预测的深链行为。 |
| 版本策略 | `v0.6.0` 稳定版基线 | 页面以当前仓库 API 为准；后续未发布能力不标记为既有稳定版，发布后再增加历史版本站点。 |
| 图表策略 | 首发使用 Markdown 文本图 | 避免 Mermaid 额外依赖和构建风险。 |
| 代码事实源 | `examples/tutorial/`，首发采用 VitePress `<<< @` 引用 | 网页核心代码直接引用可编译的完整示例，避免双重事实源。 |
| 英文首发 | 不发布英文镜像 | 不展示空白翻译；中文页面完成后再按路由一一翻译。 |
| API 参考 | 首发链接现有 `docs/API.md` | 避免复制整份 API 文档形成双重维护；后续按模块拆页或自动生成。 |

## 内容迁移清单

| 现有内容 | 网站角色 | 首发处理 | 维护责任 |
| --- | --- | --- | --- |
| `README_zh.md` / `README.md` | 项目概览与入口 | 首页摘要，保留 README 作为仓库入口 | 项目维护者 |
| `docs/BUILD.md` | 构建事实源 | 快速开始重组并链接完整文档 | 构建维护者 |
| `docs/API.md` | API 事实源与参考 | 教程按场景引用；参考页直链 | API 维护者 |
| `docs/MIGRATION.md` / `CHANGELOG.md` | 版本与迁移 | 版本迁移页直链 | 发布维护者 |
| `docs/design/*.md` | 设计与原理事实源 | 高级与原理入口聚合 | 架构维护者 |
| `docs/performance/*.md` | 性能事实源 | GPU、批量和无锁专题引用 | 性能维护者 |
| `examples/` | 场景与平台专项事实源 | 保持原位，按专题链接 | 示例维护者 |
| `examples/tutorial/` | 教程代码事实源 | 进入网页代码引用与 CMake smoke test | 教程维护者 |

## 首批页面追踪

| 页面 | 事实源 | 完整示例 | 验收责任 |
| --- | --- | --- | --- |
| 首页 | `README_zh.md`、公开 Facade | `examples/tutorial/01_first_task.cpp` | 项目维护者 |
| Executor 是什么 | `docs/design/user_guide_website.md` | — | 架构维护者 |
| 安装与构建 | `docs/BUILD.md`、根 `CMakeLists.txt` | `01_first_task.cpp` | 构建维护者 |
| 第一个任务 | `include/executor/executor.hpp` | `01_first_task.cpp` | 教程维护者 |
| 返回值与异常 | `Executor::submit()`、future | `01_first_task.cpp` | 教程维护者 |
| 初始化与关闭 | `Executor::initialize_ex()`、`shutdown()` | `01_first_task.cpp` | API 维护者 |
| 版本与迁移 | `docs/MIGRATION.md`、`CHANGELOG.md` | — | 发布维护者 |

后续页面在加入导航前必须补充事实源、示例和验收责任。

## 教程主线模型

连续教程统一使用一个小型机器人数据流水线，业务对象固定为：`SensorFrame`（采集帧）、`ParsedFrame`（解析结果）、`Plan`（规划结果）、`ControlCommand`（控制命令）、`ControlConfig`（最新配置）和 `SystemState`（监控快照）。

1. 在后台解析 `SensorFrame`：`submit()`。
2. 让 `ControlCommand` 抢占普通分析：`submit_priority()`。
3. 设备暂不可用时重试，并执行健康检查：`submit_delayed()` / `submit_periodic()`。
4. 并行处理多帧并汇合 `Plan`：batch、`TaskHandle`、`when_all()`。
5. 将 `ControlConfig`、命令流和 `SystemState` 传给周期控制：通信 Facade。
6. 仅在需要严格周期时进入实时线程；仅在计算收益明确时进入 GPU。
