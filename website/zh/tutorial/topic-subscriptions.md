---
title: 用 Topic 扇出相机帧
description: 为机器人规划和预览归档建立彼此独立的相机帧交付契约。
---

# 用 Topic 扇出相机帧

机器人流水线现在有一台相机和两个下游工作。导航规划必须按顺序检查进入其有界队列的每一帧；预览归档故意较慢，只需要为操作员界面保留最新的一帧。它不能拖慢规划，也不能让相机 producer 等待。

这和前面有限计算、通过 `future` 取结果的任务教程不同：这是一个持续数据边。本章可运行程序是 [`examples/topic_subscriptions.cpp`](https://github.com/Linductor-alkaid/kairo/blob/master/examples/topic_subscriptions.cpp)。

## 先写清楚交付契约

| 角色 | 需要什么 | 容量与过载选择 |
| --- | --- | --- |
| 相机采集 | 发布每一个新捕获的帧 | 记录发布结果，不假设必达 |
| 导航规划 | 用于障碍物规划的有序帧 | FIFO 容量 4，`RejectNewest` |
| 预览归档 | 只要最新的待处理预览帧 | 容量 1，`KeepLatest` |

单个 `MpscChannel<CameraFrame>` 不满足需求：规划和归档会竞争消息，每帧只能到达其中一个角色。让相机阻塞到归档追上同样不对，因为这会把非关键的显示需求变成相机延迟。`Topic<CameraFrame>` 为每个角色提供独立的有界 FIFO。

## 建立两条独立路径

示例的第一部分创建由相机 owner 持有的 Topic，以及两个 RAII subscription 句柄。

```cpp
Topic<CameraFrame> camera_frames("camera_frames");

TopicSubscriptionOptions planner_options;
planner_options.capacity = 4;
planner_options.drop_policy = DropPolicy::RejectNewest;
planner_options.name = "planner";
auto planner_frames = camera_frames.subscribe(planner_options);

TopicSubscriptionOptions preview_options;
preview_options.capacity = 1;
preview_options.drop_policy = DropPolicy::KeepLatest;
preview_options.name = "preview_archive";
auto preview_frames = camera_frames.subscribe(preview_options);
```

`planner_frames` 随规划角色存活，`preview_frames` 随归档角色存活。Topic 不重放历史帧，因此要先创建必需 subscription，再开启采集。规划的 `RejectNewest` 会暴露过载；预览的 `KeepLatest` 会替换过期预览工作，且不会影响规划的投递。

## 发布一小段相机突发流

示例连续发布两帧，中间不消费预览队列。这样无需依赖线程时序，也能观察到过载策略。

```cpp
for (const CameraFrame frame : std::array<CameraFrame, 2>{{{101, 2}, {102, 1}}}) {
    print_publish_result(frame, camera_frames.publish(frame));
}
```

每个结果统计本次 publish 快照中的 `matched_subscribers`、`delivered_subscribers` 和 `rejected_subscribers`。两次发布都会报告两个成功投递：`KeepLatest` 通过覆盖 101 来接收 102；只有规划队列已满时，`RejectNewest` 才会报告拒绝。结果为 false 也不能意味着盲目重试，因为重试可能向已经接收帧的 subscription 重复投递。

## 按每个角色的价值规则消费

相机停止后，示例关闭 Topic 并 drain 两个角色的队列。

```cpp
// The capture owner stops first; accepted frames remain available to drain.
camera_frames.close();

std::cout << "planner plans:";
CameraFrame frame;
while (planner_frames.try_receive(frame)) {
    std::cout << ' ' << frame.sequence;
}

std::cout << "\npreview archives:";
while (preview_frames.try_receive(frame)) {
    std::cout << ' ' << frame.sequence;
}
```

运行方式：

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release \
  -DKAIRO_BUILD_EXAMPLES=ON \
  -DKAIRO_ENABLE_GPU=OFF
cmake --build build --target topic_subscriptions
./build/examples/topic_subscriptions
```

预期输出：

```text
capture frame=101 matched=2 delivered=2 rejected=0
capture frame=102 matched=2 delivered=2 rejected=0
planner plans: 101 102
preview archives: 102
planner dropped=0, preview overwritten=1
```

规划按 FIFO 顺序看到两帧；归档只看到 102，因为一项容量的队列把过期的 101 替换掉。最后的统计说明这是有意覆盖，而不是无法解释的丢帧。

## 由 producer 负责关闭

相机采集 owner 在停止产生后关闭 `camera_frames`。关闭会唤醒等待接收者，但已成功接收的帧仍会先被 drain。subscription 的析构或 `close()` 会注销该角色；并发 publish 与取消订阅具有生命周期安全性，但正在执行的 publish 快照仍可能向正在移除的 subscription 投递或拒绝。不得在另一个线程调用同一 subscription 句柄成员函数时销毁该句柄。

在长期运行的机器人中，先停止相机采集，再关闭 Topic，让规划和归档在各自预算内 drain，最后 join 它们的线程。Topic close 不能替代停止外部相机 driver。

## 部署前明确边界

`Topic<T>` 是进程内 best-effort fan-out 控制面原语。订阅 registry 和每次 publish 快照都使用 mutex 与动态分配；复制和 fan-out 时间会随订阅数增长。它不提供重放、持久化、确认、网络传输、重连、跨订阅者原子投递或硬实时保证。

对于大型不可变图像，可使用显式共享所有权，例如 `Topic<std::shared_ptr<const Image>>`；这会避免复制图像字节，但不会让 Topic 变成实时原语。固定周期控制边需要单独的预分配组件和明确的 consumer 预算。

## 需求变化时更换设计

| 新需求 | 更合适的方向 |
| --- | --- |
| 只有规划需要每帧 | 使用一个有界 `MpscChannel` |
| 监控器只需要当前相机标定 | 使用 `LatestMailbox` 或 `DoubleBuffer` |
| 预览必须可靠持久保存每张图 | 交给具有持久化和确认的有界归档流水线 |
| 控制周期必须在固定预算内消费数据 | 使用专用实时路径，不使用 Topic |

下一步，[完整机器人数据流水线](/zh/tutorial/complete-robot-pipeline)会把这条相机边与配置、控制命令、快照、启动依赖和整机关闭顺序组合起来。
