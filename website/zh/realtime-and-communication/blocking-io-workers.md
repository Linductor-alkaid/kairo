---
title: 阻塞 I/O worker
description: 通过 Kairo Facade 管理长期阻塞 worker 的所有权、唤醒与 join。
---

# 阻塞 I/O worker

## 何时使用这条路径

当一个组件拥有长期循环、循环可以阻塞且必须有序停止时，使用 `BlockingIoExecutor`。它不是任务队列、实时控制循环、协议 adapter 或设备接入框架。

有限且可排队的工作使用线程池；固定周期控制使用专用实时线程。worker 的协议、输入、输出、重试策略和安全行为仍由使用该库的项目负责。

## Worker 契约

实现 `IBlockingIoWorker::run(stop_token)` 和 `wakeup()`：

- `run()` 可以等待，但 stop 请求可见后必须返回。
- `wakeup()` 必须解除当前等待，可重复调用且不得抛异常。
- stop token 本身不能中断任意外部等待；若等待原语不能直接唤醒，使用有限 timeout，并在每次返回后检查 token。

注册后 Facade 拥有 worker：它创建专属线程，在停止时调用 `wakeup()`，并在释放 worker 前完成 join。

## 可运行的 mock worker

该教程只用 condition variable 演示生命周期，不引入协议或硬件依赖：

<<< @/../examples/tutorial/12_blocking_io_worker.cpp{1-78}

```bash
./build/examples/tutorial/tutorial_12_blocking_io_worker
```

```text
blocking worker started=yes, stopped=yes, wakeups=1
```

## 生命周期与状态

1. 设置非空的 `BlockingIoConfig::thread_name`，将 `std::unique_ptr<IBlockingIoWorker>` 放入 `BlockingWorkerSpec`，并传给 `start_worker()`。
2. 需要启动诊断时读取 `WorkerHandle::start_result()`；`WorkerHandle::started()` 是其便捷成功判断。显式的注册/启动 API 仍保留，便于渐进迁移。
3. 通过 `WorkerHandle::status()`（或 `get_blocking_io_worker_status(name)`）读取状态。`ready` 只表示 executor 线程设置完成，不代表协议、设备或第一条输入已就绪。
4. 调用 `WorkerHandle::request_stop()` 可唤醒阻塞 worker 但不 join；调用 `WorkerHandle::stop()` 则请求停止、唤醒并 join。重复调用安全。

`Executor::shutdown()` 对所有已注册 I/O worker 采用同样的 stop/wake/join 规则，包括 `shutdown(false)`。不要 detach worker，也不要在 shutdown 后保留它的引用。

## Kairo 不负责的部分

本库刻意不决定消息所有权、队列策略、数据新鲜度、重连、设备安全动作或部署调优。这些由实现 worker 的应用自行定义和验证。

完整签名与状态字段见 [API 参考](https://github.com/Linductor-alkaid/kairo/blob/master/docs/API.md#45-阻塞-io-worker-api)。如果工作改为有固定周期预算的控制，请回到[专用实时控制循环](/zh/realtime-and-communication/realtime-control)。
