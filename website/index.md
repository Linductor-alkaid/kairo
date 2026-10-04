---
layout: home
hero:
  name: Executor
  text: 面向 C++20 应用的进程内并发执行基础设施
  tagline: 统一 Facade 管理普通异步任务、低延迟队列、周期实时线程、长期 Blocking I/O 和可选 GPU 工作；从 submit_auto() 开始。
  actions:
    - theme: brand
      text: 十分钟开始使用
      link: /zh/quick-start/build
    - theme: alt
      text: 了解 Executor
      link: /zh/getting-started/what-is-kairo
features:
  - title: 先完成一个任务
    details: 从 submit_auto() 和 future.get() 开始，不要求先理解线程池、GPU 或实时调度器。
  - title: 约束明确后再下钻
    details: 只有遇到明确的周期、容量、I/O 或数据传递约束时，才进入有界投递、长期 worker、实时、通信或 GPU 等专用路径。
  - title: 示例可验证
    details: 页面核心代码对应仓库中的教程示例，并由根 CMake 工程持续编译和 smoke test。
---

## 一眼看懂

```cpp
auto& executor = executor::Executor::instance();
auto answer = executor.submit_auto([] { return 42; });
std::cout << answer.get() << '\n';
executor.shutdown();
```

`get()` 同时获取结果和重新抛出任务中的异常。完整代码与预期输出见[第一个任务](/zh/quick-start/first-task)。

## 能力边界

Executor 不是协程运行时、分布式消息系统或硬实时操作系统；它不能安全地强制终止任意正在运行的 C++ 函数，`submit_periodic()` 只是普通线程池上的软周期任务。完整边界（含 0.4.0 起的同步无锁保证）见 [Executor 是什么](/zh/getting-started/what-is-kairo)。

## 从这里继续

- 第一次使用：从[构建与安装](/zh/quick-start/build)到[第一个任务](/zh/quick-start/first-task)。
- 按业务约束选型：[选择提交接口](/zh/guides/choosing-submit-api)，再理解[执行模型与路由边界](/zh/guides/execution-models-and-routing)。
- 改造现有项目：[从线程代码迁移](/zh/guides/migrating-existing-threads)，并检查[并发架构反模式](/zh/guides/concurrency-antipatterns)。
- 验证服务端模型：[数据导入案例](/zh/tutorial/service-data-import)讲清部分失败、幂等与请求排空。
- 准备接入服务：[生产接入检查清单](/zh/guides/production-readiness)。
- 深入原理：[高级与原理](/zh/advanced/)；按需了解实时线程、通信、GPU 与底层执行路径。
- 使用 AI 集成 Kairo 时，先阅读渐进式 [Kairo 集成 skill](https://github.com/Linductor-alkaid/kairo/blob/master/docs/skill/kairo-integration/SKILL.md)；其中也说明了如何让下游项目中的 AI 获取该 skill。

## 发布信息

| 项目 | 当前支持 |
| --- | --- |
| 平台 | Linux、Windows；Android CPU-only（NDK） |
| 语言 | C++20 |
| 构建系统 | CMake 3.16+ |
| 版本 | `v0.5.3` |
| 持续集成 | [GitHub Actions](https://github.com/Linductor-alkaid/executor/actions/workflows/c-cpp.yml) |
| 许可证 | [MIT](https://github.com/Linductor-alkaid/kairo/blob/master/LICENSE) |

<div class="version-note">本手册对应 `v0.5.3`；后续 `master` 开发能力需在发布 tag 后才构成稳定版承诺。</div>
