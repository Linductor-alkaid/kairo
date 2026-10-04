---
title: 高级与原理
description: 只有默认 Facade 无法满足资源隔离或底层控制需求时，才使用这些高级接口。
---

# 高级接口与实现原理

大多数应用从 `Executor` Facade 的 `submit_auto(lambda)` 开始。只有需要隔离资源、接入自定义周期源、直接控制实时/GPU 执行器或分析队列实现时，才需要这些公开的高级接口。

先读[源码结构与阅读路线](/zh/advanced/source-architecture)，了解模块、所有权和同步域，再按需要选择专题：

1. [何时使用高级接口](/zh/advanced/escape-hatches)：实例隔离、`ExecutorManager` 与直接执行器指针的责任边界。
2. [接入自定义周期源](/zh/advanced/custom-cycle-manager)：实现 `ICycleManager` 并管理其生命周期。
3. [任务如何穿过执行器](/zh/advanced/execution-paths)：普通与实时任务的状态转移、promise 兑现和完成不变量。
4. [无锁与性能实验](/zh/advanced/lockfree-and-performance)：`LockFreeTaskExecutor`、MPSC 槽位协议、对象池和退避。
5. [性能测量与回归检查](/zh/advanced/performance-measurement)：统一测量吞吐、尾延迟、jitter 和正确性的实验方法。

以下页面介绍当前实现，方便调试和性能分析；除 `include/kairo/` 下的公开接口外，不承诺 `src/` 内类型、数据结构或调度细节保持兼容。设计说明见 [`docs/design/executor.md`](https://github.com/Linductor-alkaid/kairo/blob/master/docs/design/executor.md)、[`docs/design/lockfree_user_api.md`](https://github.com/Linductor-alkaid/kairo/blob/master/docs/design/lockfree_user_api.md) 和 [`docs/API.md`](https://github.com/Linductor-alkaid/kairo/blob/master/docs/API.md)。
