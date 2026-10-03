---
title: 启动专用实时控制循环
description: 使用 Kairo Facade 注册、诊断启动、推送和停止一个专用周期线程。
---

# 启动专用实时控制循环

## 学习目标

从 CAN 或控制循环的固定周期需求出发，使用 `register_realtime_task()`、`start_realtime_task()`、`try_push_realtime_task()` 和状态查询完成最小的可诊断路径。

这是专家专题：普通有限工作继续使用 `submit_auto(lambda)`；只有已有固定周期、周期预算与有界背压语义时才注册专用实时线程。若只需向已启动的实时队列投递一次工作，可使用 `dispatch_auto(RealtimeQueue)`，但它同样只报告接收，不报告完成。

## 何时需要专用线程

`submit_periodic()` 适合健康检查、刷新和允许抖动的后台工作。控制循环若需要固定周期、周期预算、优先级或 CPU 亲和性，应使用一个专用实时线程；它仍受操作系统、权限和硬件约束，不是绝对时限保证。

长期阻塞等待不属于以上两条路径：不要把它放入 `cycle_callback`，应使用具有显式唤醒契约的[阻塞 I/O worker](/zh/realtime-and-communication/blocking-io-workers)。

## 推荐方案

教程示例在非特权环境中主动关闭内存锁和 timer slack 请求，以便验证基本路径：

<<< @/../examples/tutorial/07_realtime.cpp{1-45}

完整源码：[`examples/tutorial/07_realtime.cpp`](https://github.com/Linductor-alkaid/kairo/blob/master/examples/tutorial/07_realtime.cpp)。

```bash
./build/examples/tutorial/tutorial_07_realtime
```

## 预期输出

```text
realtime started=yes, command=queued, cycles=observed, command ran=yes
```

## 生命周期与队列

1. 填写最小 `RealtimeThreadConfig`：名称、周期和 `cycle_callback`。
2. 用 `_ex` 变体注册并启动；失败时读取 `ExecutorResult::error_code` 和 `message`。
3. 通过 `push_realtime_task()` 或 `try_push_realtime_task()` 投递常规控制工作；返回 `false` 表示未入队。
4. 用 `get_realtime_executor_status()` 和 `get_realtime_task_list()` 观察运行状态，完成后调用 `stop_realtime_task()`。

实时队列是有界入口：入队成功只表示将在后续周期处理，并不表示任务已完成。`max_tasks_per_cycle` 默认是 `64`；剩余工作会留给下一周期，以保护周期预算。周期回调超时后，运行时会跳过已错过的节拍并重新以“当前时间加一个周期”调相，避免追赶造成抖动风暴；通过 `cycle_timeout_count` 观察超时。紧急停止必须走应用自己的硬件或安全控制旁路，不能等待实时队列消费。

若调用方在统一控制面投递，必须声明名称和 intent：

```cpp
TaskOptions options;
options.intent = ExecutionIntent::RealtimeQueue;
options.preferred_executor = "control";
auto admission = executor.dispatch_auto(options, [] { apply_control(); });
```

`admission.accepted` 与 `try_push_realtime_task()` 的 `true` 含义相同：仅表示队列接收。未启动、队列满、对象池耗尽或关闭竞争会拒绝，绝不改投默认线程池。

`RealtimeQueue` 必须和 `preferred_executor` 同时填写；它只匹配同名、已启动的实时后端。完整的检查和拒绝分支见[自动路由如何匹配目标](/zh/guides/execution-models-and-routing)。

## 实时线程如何接收函数与输入

实时路径有两个不同入口，二者都只接收无参数、无返回值的 `void()` callable：

| 入口 | 何时调用 | 输入绑定方式 | 完成观察 |
| --- | --- | --- | --- |
| `config.cycle_callback` | 每个周期固定调用 | 注册前用 lambda 捕获长期状态 | `cycle_count`、超时和应用状态 |
| `try_push_realtime_task(name, task)` | 入队后由后续周期有限消费 | 推送时用 lambda 捕获该命令输入 | 返回值只表示是否入队，状态计数观察执行 |

```cpp
auto controller = std::make_shared<Controller>(config_snapshot);
config.cycle_callback = [controller] {
    controller->run_cycle();
};

ControlCommand command = read_command();
const bool queued = executor.try_push_realtime_task(
    "control", [controller, command] {
        controller->apply(command);
    });
```

这里没有 `try_push_realtime_task(name, fn, args...)` 重载，也没有每项 future；输入必须先绑定进可复制的 `std::function<void()>`。不要捕获提交线程栈上的引用，也不要在 callback 内分配大型对象、阻塞等待或取得普通 mutex：输入应在非实时线程准备好，再以小型值、稳定 handle 或预分配对象传入。

`cycle_callback` 捕获的对象必须活到 `stop_realtime_task()` 返回；动态推送任务捕获的对象必须活到任务被消费或执行器停止清理。使用 `shared_ptr` 只能解决生命周期，不能保证实时确定性；引用计数、析构位置和对象内部锁仍需在目标硬件测量。

## 配置与降级

默认配置会尽力申请实时优先级、CPU 亲和性和低 timer slack。进程级内存锁默认关闭：Linux `mlockall` 会锁定整个进程和后续映射，只有在评估完整进程内存预算后才应启用 `enable_process_memory_lock`。Linux 的 `SCHED_FIFO`、`mlockall`、容器 cpuset 和 Windows 调度能力可能受权限或平台限制；库会安全继续运行，但这不代表请求已生效。

部署时检查 `RealtimeExecutorStatus` 的 `priority_applied`、`cpu_affinity_applied`、`process_memory_lock_applied`、`process_memory_lock_errno` 和 `timer_slack_applied`，并结合 `cycle_timeout_count`、`dropped_task_count`、`queue_full_count` 与 `pool_exhausted_count` 设定告警。空 `cpu_affinity` 是自适应选择，显式配置则应由部署环境验证其有效性。

## 下一步阅读

[选择消息传递方式](/zh/realtime-and-communication/channels)选择普通数据流或实时周期内有限消费；需要可观察的配置和状态传递，请继续阅读下一章。
