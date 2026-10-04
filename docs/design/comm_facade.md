# 通信与并发辅助 Facade 设计


> [!NOTE]
> 历史设计快照（0.4.0）：记录设计时的决策与权衡。项目已更名为 kairo，文中 `executor::`/`EXECUTOR_*` 等命名已对应更替为 `kairo::`/`KAIRO_*`。

本文档是阶段 7 的设计稿，目标是把常见跨线程通信模式提升到 `executor::comm`
层，让用户默认走类型安全、生命周期清晰、可观察的抽象，而不是直接组合
`mutex`、`atomic`、`condition_variable`、共享可变对象和底层无锁队列。

关联计划：[通信与并发辅助 Facade 更新计划](../archive/todolists/comm_facade_update_plan.md)。

---

## 背景与问题

多线程共享状态时，常见错误并不来自“没有工具”，而是来自工具组合太低层：

- 写线程和读线程谁先发生不确定，用户容易消费旧状态或半更新状态。
- 控制线程、采集线程、规划线程、通信线程之间有严格步骤顺序，但代码里只剩零散的锁和条件变量。
- `notify_one()` 漏掉、wait predicate 写错、ABA、relaxed/acquire/release 用错都会变成偶发问题。
- 队列满、覆盖旧值、实时周期内来不及消费时，如果没有统一状态，问题会表现为“系统偶尔不响应”。
- 实时线程不应被普通锁、堆分配、无限等待拖住。

项目已有的基础能力：

- `executor::util::LockFreeQueue<T>`：MPSC 无锁队列，要求 `T` trivially copyable，已有容量、失败 push、峰值等统计。
- `RealtimeThreadExecutor`：已有周期执行、每周期任务预算、实时 push 背压计数。
- `Executor` facade：已有 failure event、recent failure、wait result、周期任务状态等可观察性入口。
- `TaskDependencyManager`：已有依赖图、ready 检查、完成标记和 cycle 检测，但还没有高层任务时序 API。

阶段 7 不再把这些基础类型直接暴露给普通用户，而是提供更明确的通信语义。

---

## 设计目标

1. **默认写不出 data race**：API 不暴露可并发写读的裸引用；跨线程数据通过值、不可变快照或受控写入窗口传递。
2. **时序显式化**：线程间“先初始化后工作”“先采集后规划”“第 N 相位后再执行”通过 `PhaseGate` / `Sequencer` 表达。
3. **背压不静默**：队列满、覆盖、过期、关闭后提交都必须通过返回值和统计可见。
4. **实时路径边界清晰**：实时线程优先使用非等待接口；分别声明 data-race-free、系统级同步无锁、固定次数尝试、内部存储无分配和端到端硬实时边界。
5. **可诊断但低开销**：每个组件提供本地 `stats()`；可选接入 `Executor` 的诊断回调，但不把通信抖动混为任务失败。
6. **渐进实现**：先支持单消费者、有限容量和明确策略；多消费者广播作为独立的进程内原语补充，复杂 reactive graph 留到后续版本。

## 非目标

- 不替代完整 actor framework、Rx 框架或分布式消息系统。
- 不把进程内 `Topic<T>` 扩展为网络 broker：不提供 topic discovery、跨进程传输、持久化、重放、确认、重连或跨订阅者的事务性投递。
- 不承诺所有组件 lock-free：动态订阅的 `Topic<T>` 明确保留 mutex、动态 registry 和 fan-out 分配。
- 不把“内部同步原子 lock-free”等同于 payload、callback、分配、OS 调度或整条业务链路硬实时。
- 不强制所有类型都走同一内部实现。消息流使用预分配节点，非平凡快照类型使用 reader pin 固定槽。

## 当前语义边界

阶段 7 的组件是通信原语集合，不是统一的时间模型：

- 未绑定时，`PhaseGate` 表示阶段，`Sequencer` 表示可跳 ticket 的单调 publication watermark；两者都不把状态与数据快照绑定。
- `DoubleBuffer` 只保证一次发布得到完整值快照，不说明快照属于哪个逻辑相位。
- 未绑定时，`LatestMailbox` / `RealtimeChannel` 提供最新值或有界消息消费语义，但不提供端到端相位一致性。
- `MpscChannel` / `RealtimeChannel` 使用构造期预分配的 MPSC 节点和单逻辑消费者；`LatestMailbox` /
  普通 `DoubleBuffer` 使用四个固定 reader-pin 快照槽；`PhaseGate` / `Sequencer` 使用原子状态核心。
- 上述组件构造时要求内部同步原子在目标平台 lock-free。`is_synchronization_lock_free()` 只描述这层
  同步，不覆盖 `T`、时钟、字符串、callback、外部分配或 OS 调度，也不把有竞争的 CAS 宣称为 wait-free。
- 四槽快照的 `try_load()` 最多检查四个槽；`try_publish()` 是非等待、系统级 lock-free 入口，但
  publication CAS 可重试，不承诺单次调用有界或 wait-free。channel `try_*` 同样是非等待 lock-free
  入口而非 wait-free 保证。保证成功或等待 timeout 的兼容 API 会 spin/yield，不属于硬实时路径。
- 快照 sequence 是有限 56 位域：耗尽时 `try_publish()` 返回 `false`，重试型兼容发布/更新接口抛
  `std::overflow_error`，不会无限 spin。
- phase/ticket 状态必须 `< 2^63`；非法 wait 在进入计时/轮询前返回 `InvalidArgument`。LET 槽保留
  `2^63 - 1` 为空态，因此 phase-bound publication 还要求 phase `< 2^63 - 1`。
- callback 的配置可能分配，callback 的执行是任意用户代码，两者都属于非实时诊断/控制面。
- `MpscChannel<T>` 的一次 receive 会移走消息；`DoubleBuffer<T>` 只保存最新完整快照。因此它们都不能表达“多个独立模块各自按 FIFO 收到每一条后续事件”。

因此，用户若自行组合未绑定的 phase gate 和 double buffer，仍需自行约定“哪个相位的数据何时可见”；显式绑定模式则提供本节定义的 LET 保证。选择原语后仍需用目标平台测试验证 payload、callback、缺页、调度和整条周期预算；同步无锁本身不是硬实时证明。

## 阶段 7.8：为既有原语增加 LET 契约

阶段 7.8 不增加独立的 `LetChannel<T>`。LET（Logical Execution Time）作为
`PhaseGate` 与 `DoubleBuffer<T>` / `LatestMailbox<T>` 的可选绑定模式：`PhaseGate` 是唯一的
逻辑时钟，后两者保存与该时钟绑定的相位值。这样用户继续使用已有的阶段、快照和最新值
原语，而框架负责保证“相位 N 的完整输出只在 N+1 边界对读侧可见”。

该模式必须保留现有 API 的兼容语义：未绑定相位门的 `DoubleBuffer<T>::publish()` / `load()`
仍然是普通的最新完整快照，`LatestMailbox<T>::publish()` / `try_load()` 仍是 latest-wins。
绑定后，写侧只能为当前逻辑相位提交，推进相位会封存该相位；读侧只能在下一相位取得前一
相位的完整值。重复提交、跳相位、未就绪读取和读侧落后都通过 `CommResult` 诊断，而不能
退回到应用层约定。

第一版应明确为单写单读；多写者先在非实时控制面仲裁，再由唯一写侧向绑定的
`DoubleBuffer` 或 `LatestMailbox` 提交。相位绑定模式使用构造期固定容量存储和原子相位发布，成功的周期
路径不得获取 mutex、等待 condition variable 或为内部槽分配内存；失败的 `CommResult` 诊断不属于
成功周期路径。`T` 自身的复制/移动及其可能分配不在此保证内；类型需要满足无异常复制约束。
配套验收覆盖同相位不可见、
重复提交、跳相位、半写快照和 RT 路径分配检测。

通信观测提供固定开销的对数延迟直方图与近似 P50/P99。组件内 latency 表示本地等待或发布到
消费的时长，具体含义由组件 API 决定；端到端延迟必须由业务消息携带源时间戳并在目标端计算，
`comm_robot_pipeline` 展示了传感器到控制的测量方式。

`benchmark_realtime_precision --json` 是配套 jitter 基准：它报告周期回调入口相对期望截止时间
的 min/avg/P50/P95/P99，并在 JSON 中记录 compiler、scheduler、采样 CPU 和采样边界（首样本为
基线，启动等待不计入）。该报告应与管线端到端延迟分开解读，不能用单一 jitter 数值替代消息
年龄或传感器到控制延迟。

实时内存规则是：周期回调不得进行隐式堆分配、阻塞等待或执行诊断回调。Linux Debug 可用
`-DEXECUTOR_ENABLE_REALTIME_ALLOCATION_GUARD=ON` 启用 Linux 诊断构建中的
`RealtimeAllocationGuard`。它记录显式包围的线程/阶段中的 C++ `new` 分配，用于回归测试和
定位，不应作为生产运行时机制；它通过进程级 `operator new` 重载工作，可能与宿主 allocator、
内存池或共享库重载冲突。`RealtimeThreadConfig::enable_allocation_guard` 默认关闭，显式开启后
才会在 `cycle_callback` 外层自动挂载记录型 guard（组件为执行器名，阶段为 `cycle_callback`）。
违例策略可选记录或 `Abort`，而事件回调不在实时路径自动调用。

---

## 命名空间与头文件

建议新增聚合头：

```cpp
#include <executor/comm.hpp>
```

核心命名空间：

```cpp
namespace executor::comm {
// Channel, mailbox, phase gate, double buffer, task sequencing helpers.
}
```

建议文件布局：

- `include/executor/comm.hpp`：聚合头。
- `include/executor/comm/types.hpp`：通用结果、策略、统计、事件类型。
- `include/executor/comm/channel.hpp`：`MpscChannel<T>` / `SpscChannel<T>`。
- `include/executor/comm/mailbox.hpp`：`LatestMailbox<T>` / `RealtimeChannel<T>`。
- `include/executor/comm/phase_gate.hpp`：`PhaseGate` / `Sequencer`。
- `include/executor/comm/double_buffer.hpp`：`Snapshot<T>` / `DoubleBuffer<T>`。
- `include/executor/comm/topic.hpp`：进程内 `Topic<T>` / `TopicSubscription<T>`。
- `src/executor/comm/*.cpp`：非模板实现和诊断格式化。

---

## 通用类型

```cpp
namespace executor::comm {

enum class CommErrorCode {
    Ok,
    Closed,
    Full,
    Empty,
    Timeout,
    Stale,
    MissedPhase,
    InvalidArgument,
    NotReady
};

struct CommResult {
    bool ok = true;
    CommErrorCode error_code = CommErrorCode::Ok;
    std::string message;

    explicit operator bool() const noexcept { return ok; }
};

enum class DropPolicy {
    RejectNewest,  // 默认：满时拒绝新消息，不静默丢。
    DropOldest,    // 满时丢最旧消息，记录 drop。
    KeepLatest     // 保留最新值，适合 mailbox，不适合普通 FIFO 语义。
};

struct CommStats {
    uint64_t sent_count = 0;
    uint64_t received_count = 0;
    uint64_t dropped_count = 0;
    uint64_t overwritten_count = 0;
    uint64_t stale_read_count = 0;
    uint64_t closed_send_count = 0;
    uint64_t timeout_count = 0;
    uint64_t missed_phase_count = 0;
    uint64_t current_depth = 0;
    uint64_t peak_depth = 0;
    uint64_t capacity = 0;
    uint64_t producer_lag = 0;
    uint64_t consumer_lag = 0;
    std::chrono::nanoseconds max_latency{0};
    std::chrono::nanoseconds avg_latency{0};
};

enum class CommEventKind {
    Dropped,
    Overwritten,
    ClosedSend,
    Timeout,
    StaleRead,
    MissedPhase,
    ProducerLag,
    ConsumerLag,
    LatencyHigh
};

struct CommEvent {
    CommEventKind kind;
    std::string component_name;
    std::string message;
    uint64_t sequence = 0;
    std::chrono::steady_clock::time_point timestamp =
        std::chrono::steady_clock::now();
};

using CommEventCallback = std::function<void(const CommEvent&)>;

} // namespace executor::comm
```

说明：

- `CommResult` 用于失败可解释的控制操作，如 `close()`、`advance_to()`、`wait_for()`。
- 高频数据路径优先提供 `bool try_*`，避免每条消息构造字符串。
- `CommStats` 是本地累计统计；`CommEventCallback` 是可选诊断，不建议默认对每次 drop 分配字符串。
- 通信事件默认不是 `FailureKind::TaskException`。它们可以汇总到 Executor 诊断面板，但不污染任务失败计数。

---

## P1：Typed Channel

Typed Channel 解决“共享变量 + 锁”易错的问题。它提供明确的所有权转移和容量语义。

### API 草案

```cpp
namespace executor::comm {

struct ChannelOptions {
    size_t capacity = 1024;
    DropPolicy drop_policy = DropPolicy::RejectNewest;
    bool enable_stats = true;
    std::string name;
};

template<class T>
class MpscChannel {
public:
    explicit MpscChannel(ChannelOptions options = {});

    bool try_send(const T& value);
    bool try_send(T&& value);

    template<class Rep, class Period>
    CommResult send_for(T value, std::chrono::duration<Rep, Period> timeout);

    bool try_receive(T& out);

    template<class Rep, class Period>
    CommResult receive_for(T& out, std::chrono::duration<Rep, Period> timeout);

    void close();
    bool is_closed() const;
    bool empty() const;
    size_t size_approx() const;
    size_t capacity() const;
    CommStats stats() const;
    void set_event_callback(CommEventCallback callback);
    bool is_synchronization_lock_free() const;
};

template<class T>
using SpscChannel = MpscChannel<T>; // 初期可复用实现，后续替换为 SPSC 优化。

} // namespace executor::comm
```

### 语义

- 默认有界容量，满时 `try_send()` 返回 `false` 并增加 `dropped_count` 或 `closed_send_count`，不静默丢。
- `DropPolicy::RejectNewest` 是默认策略，适合命令、任务和事件流。
- `DropPolicy::DropOldest` 适合日志、遥测；必须增加 `dropped_count`。
- FIFO channel 不使用 `KeepLatest` 作为默认。只需要最新状态时应使用 `LatestMailbox<T>`。
- `close()` 后生产者不能再提交；消费者可继续 drain 已有数据，直到空。
- `send_for()` / `receive_for()` 在原子核心上 spin/yield 到成功、关闭或 timeout；实时线程应使用
  `try_send()` / `try_receive()` 或带明确预算的 `RealtimeChannel::drain_for_cycle()`。
- 组件只支持一个逻辑消费者；多名独立消费者需要 `Topic`，但 Topic 不是实时路径。

### 当前实现

- 构造时一次性分配固定 node pool；producer 取得私有节点并完成 `T` 构造后才原子发布。
- 单逻辑 consumer 以原子 exchange 分离完整批次并反转为 FIFO；未完成的 producer 不会在队首形成洞。
- `try_send()` / `try_receive()` 的内部队列同步不获取 mutex，构造后不分配节点；payload 操作和
  已配置 callback 不在该保证内。
- 所需指针和整数原子非 lock-free 时构造抛异常，避免静默退化到库锁。

---

## P2：PhaseGate / Sequencer

PhaseGate 解决“线程间步骤顺序不确定”的问题。用户不再手写 condition variable predicate。

### PhaseGate API 草案

```cpp
namespace executor::comm {

class PhaseGate {
public:
    explicit PhaseGate(std::string name = {});

    uint64_t current_phase() const;

    CommResult advance_to(uint64_t phase);
    CommResult advance();

    bool has_reached(uint64_t phase) const;

    template<class Rep, class Period>
    CommResult wait_for(uint64_t phase,
                        std::chrono::duration<Rep, Period> timeout);

    CommResult close();
    bool is_closed() const;
    CommStats stats() const;
    void set_event_callback(CommEventCallback callback);
};

} // namespace executor::comm
```

### Sequencer API 草案

```cpp
namespace executor::comm {

class Sequencer {
public:
    uint64_t next_ticket();
    CommResult publish(uint64_t ticket);
    bool is_published(uint64_t ticket) const;

    template<class Rep, class Period>
    CommResult wait_until_published(uint64_t ticket,
                                    std::chrono::duration<Rep, Period> timeout);
};

} // namespace executor::comm
```

### 语义

- phase 单调递增，不允许倒退。
- `wait_for(p)` 在当前 phase 已达到或超过 `p` 时成功；`wait_for_exact(p)` 在 phase 已越过 `p` 时返回 `MissedPhase`。
- `Sequencer` 的 `publish(ticket)` 推进单调 publication watermark，可直接跳到更大 ticket；它不要求中间 ticket 逐个发布。
- `is_published(ticket)` 只表示 watermark 已达到或越过该 ticket，不证明该 ticket 曾被单独 publish。
- `wait_until_published(ticket)` 是精确等待：watermark 恰好等于 ticket 时成功，已经越过时返回 `MissedPhase`。
- `close()` 原子发布关闭状态，spin/yield waiter 随后观察 `Closed`。
- phase/published ticket 与 closed 位打包在原子状态字中，推进、关闭与查询不获取 mutex。
- wait API 读取 `steady_clock` 并 spin/yield，只是控制面 timeout 适配器；原子核心不等于 wait-free。
- phase/ticket 合法值必须 `< 2^63`。`PhaseGate::wait_for*()` 对 `phase >= 2^63`、
  `Sequencer::wait_until_published()` 对 `ticket == 0` 或 `ticket >= 2^63` 立即返回 `InvalidArgument`。
  `next_ticket()` 在关闭或空间耗尽时返回 `0`。

---

## P3：Realtime Mailbox / RealtimeChannel

实时线程有两类常见消费模式：

- 每周期只关心最新配置或目标值：使用 `LatestMailbox<T>`。
- 每周期 drain 一批消息但不能无限处理：使用 `RealtimeChannel<T>`。

### LatestMailbox API 草案

```cpp
namespace executor::comm {

template<class T>
class LatestMailbox {
public:
    explicit LatestMailbox(std::string name = {});

    void publish(const T& value);
    void publish(T&& value);

    bool try_publish(const T& value, uint64_t* new_sequence = nullptr);
    bool try_publish(T&& value, uint64_t* new_sequence = nullptr);

    // 返回 false 表示从未发布过值。
    bool try_load(T& out) const;

    // 若 sequence 未变化，返回 false，避免重复消费旧数据。
    bool try_load_newer_than(uint64_t last_seen_sequence,
                             T& out,
                             uint64_t& new_sequence) const;

    uint64_t sequence() const;
    CommStats stats() const;
    void set_event_callback(CommEventCallback callback);
    bool is_synchronization_lock_free() const;
};

} // namespace executor::comm
```

### RealtimeChannel API 草案

```cpp
namespace executor::comm {

struct RealtimeChannelOptions {
    size_t capacity = 1024;
    size_t max_items_per_cycle = 64;
    DropPolicy drop_policy = DropPolicy::RejectNewest;
    bool enable_stats = true;
    std::string name;
};

template<class T>
class RealtimeChannel {
public:
    explicit RealtimeChannel(RealtimeChannelOptions options = {});

    bool try_send(const T& value);
    bool try_send(T&& value);

    // 实时线程调用：同步核心不阻塞、不分配队列节点，且不超过 max_items。
    template<class Fn>
    size_t drain_for_cycle(Fn&& handler, size_t max_items = 0);

    void close();
    CommStats stats() const;
    void set_event_callback(CommEventCallback callback);
    bool is_synchronization_lock_free() const;
};

} // namespace executor::comm
```

### 实时约束

- `drain_for_cycle()` 不等待，不调用 condition variable。
- `max_items == 0` 表示使用配置中的 `max_items_per_cycle`。
- handler 抛异常时不在实时路径做复杂恢复；建议返回已处理数量，并记录轻量错误计数。是否把异常桥接到 `Executor` failure event 由集成层决定。
- 队列满策略必须显式，默认拒绝新消息并返回 `false`。
- `RealtimeChannel` 复用预分配 MPSC node pool；`LatestMailbox` 使用四槽 reader pin 快照。
- mailbox 的 `try_load()` 最多检查四个槽；`try_publish()` 非等待且系统级 lock-free，但 CAS 可重试，
  不承诺单次调用有界/wait-free。兼容 `publish()` 在槽繁忙时 spin/yield，在 56 位 sequence 永久耗尽时
  抛 `std::overflow_error`。
- `T` 操作、时钟、handler、异常和 callback 仍由调用方负责；启用 callback 后不能宣称实时路径无分配。

---

## P4：Snapshot / DoubleBuffer

Snapshot / DoubleBuffer 解决“读到半更新状态”和“共享 mutable state”的问题。

### API 草案

```cpp
namespace executor::comm {

template<class T>
struct Snapshot {
    T value;
    uint64_t sequence = 0;
    std::chrono::steady_clock::time_point timestamp;
};

template<class T>
class DoubleBuffer {
public:
    explicit DoubleBuffer(T initial = {});

    // 单写线程或外部保证写入串行。多写版本后续可提供 MultiWriterDoubleBuffer。
    template<class Fn>
    uint64_t update(Fn&& writer);

    uint64_t publish(T value);

    bool try_publish(const T& value, uint64_t* new_sequence = nullptr);
    bool try_publish(T&& value, uint64_t* new_sequence = nullptr);

    bool try_load(Snapshot<T>& out) const;
    Snapshot<T> load() const;
    bool load_newer_than(uint64_t last_seen_sequence, Snapshot<T>& out) const;

    // Explicit LET binding; fixed two-slot SWSR storage.
    CommResult bind_to_phase_gate(PhaseGate& gate, size_t capacity = 2);
    CommResult publish_for_current_phase(T value);
    CommResult load_for_current_phase(Snapshot<T>& out) const;

    uint64_t sequence() const;
    CommStats stats() const;
    bool is_synchronization_lock_free() const;
};

} // namespace executor::comm
```

`bind_to_phase_gate()` is opt-in and does not alter unbound `publish()` / `load()` behavior. In
bound mode the successful periodic operations use preallocated slots and acquire no mutex or
condition-variable wait. The gate rejects a phase transition while a writer lease is active;
the caller retries a `NotReady` result in the next cycle. This deliberately constrains the first
implementation to one writer and one reader; multi-writer arbitration remains outside the
real-time path.

### 语义

- 读者只看到完整发布后的快照。
- 普通模式使用四个 reader-pin 槽；reader 复制期间 writer 不能改写该槽，非平凡 `T` 无 data race。
- `try_load()` 最多检查四个槽；`try_publish()` 是非等待、系统级 lock-free 操作，但竞争 CAS 可重试，
  不承诺 per-call bounded/wait-free。所有槽繁忙或 sequence 永久耗尽时可返回 `false`。
- `publish()` / `load()` / `update()` 保留兼容语义，暂时竞争时可能 spin/yield，不是实时 API；有限的
  56 位 sequence 耗尽时，发布/更新路径抛 `std::overflow_error` 而不是永久重试。
- `update(fn)` 从当前完整快照复制、修改并一次性发布；公开契约为 SWMR，并发 writer 的逻辑更新可能互相覆盖。
- `load_newer_than()` 帮助读者避免重复消费旧状态。
- 多写场景建议先用 `MpscChannel` 汇聚到一个状态 owner。
- `T` 的复制/移动、时钟与 callback 不属于同步无锁进度保证；大型状态应评估复制预算或显式使用不可变 handle。

### LET 绑定模式

`LatestMailbox<T>` 也可通过 `bind_to_phase_gate(PhaseGate&, 2)` 显式加入 LET 契约：
`publish_for_current_phase(value)` 在当前相位提交一次最新值，`load_for_current_phase(out, phase)`
只读取上一完整相位。未绑定的 `publish()` / `try_load()` 行为不变；绑定模式第一版为 SWSR、
固定双槽和无异常复制，重复提交或相位未就绪返回 `CommResult`。绑定 mailbox 是“每相位一条
单值快照”，而不是未绑定 mailbox 的 latest-wins 覆盖语义。`RealtimeChannel` 不自动继承
LET，因为 FIFO 消费预算与单值相位快照是不同语义。

普通 phase/ticket 状态域是 `[0, 2^63)`。绑定 LET 的双槽状态另保留 `2^63 - 1` 为空态，所以
`publish_for_current_phase()` 只接受 phase `< 2^63 - 1`；达到保留值时返回 `InvalidArgument`。

---

## P5：submit_after / when_all

项目已有 `TaskDependencyManager`，但用户仍缺一个不用手写任务 ID 和轮询的时序 API。

### API 草案

```cpp
namespace executor {

class TaskHandle {
public:
    std::string id() const;
    bool valid() const;
};

template<class F, class... Args>
auto Executor::submit_after(const TaskHandle& dependency,
                            F&& f,
                            Args&&... args)
    -> std::future<std::invoke_result_t<F, Args...>>;

template<class F, class... Args>
auto Executor::submit_after(const std::vector<TaskHandle>& dependencies,
                            F&& f,
                            Args&&... args)
    -> std::future<std::invoke_result_t<F, Args...>>;

TaskHandle Executor::when_all(std::vector<TaskHandle> dependencies);

} // namespace executor
```

### 语义

- `submit_after(A, f)` 表示 `f` 只在 A 完成后进入执行器。
- dependency 失败时的默认策略建议为“不执行 dependent task，并让 dependent future 得到异常”，避免用户消费无效中间状态。
- `when_all()` 返回一个逻辑 handle，可作为后续任务依赖。
- 内部可先复用 `TaskDependencyManager`；第一版可限制依赖对象来自同一个 `Executor` 实例。
- 后续可扩展 `when_any()`、取消传播和失败策略。
- 已完成 handle 采用有界终态保留：默认保留最近 1024 个，容量为 0 时立即过期；仍被活动任务依赖的终态节点不得提前裁剪。过期 handle 必须返回可诊断的无效依赖错误。

---

## P7：进程内 Topic / Subscription

### 要解决的缺口

现有组件覆盖的是单消费者消息、最新值和多读快照，不能直接完成“每一个订阅模块都按自己的
速度消费同一条事件流”：

- 两个 consumer 同时调用 `MpscChannel<T>::try_receive()` 时，一条消息只会被其中一个取走。
- `LatestMailbox<T>` 和 `DoubleBuffer<T>` 可以供多个 reader 读取，但慢 reader 只能看到最新值，
  无法得到期间的每一条事件。
- 让 publisher 手工维护多个 channel 会泄漏订阅生命周期、逐订阅者背压和统计责任，且容易让
  一个慢 consumer 意外阻塞其他 consumer。

因此建议补充一个可选的进程内 `Topic<T>`：发布端将每条消息扇出到发布时仍有效的每个订阅者
的独立有界 FIFO。它适用于一份采集事件同时交给规划、记录、告警等独立模块，或一份业务事件
同时交给多个非实时处理器；不是 `MpscChannel<T>`、`LatestMailbox<T>` 或 `DoubleBuffer<T>` 的替代。

以下场景仍不应使用 Topic：只要当前状态的 UI/监控使用 `DoubleBuffer<T>`，只要最新目标或配置
使用 `LatestMailbox<T>`，单一控制 owner 的命令使用 `RealtimeChannel<T>`。Topic 的逐订阅者复制、
锁与 fan-out 时间都不适合硬实时周期；网络化发布订阅则由 ROS 2、NATS、MQTT 等传输层处理，
再接入本地通信原语。

### API 草案

```cpp
namespace executor::comm {

struct TopicSubscriptionOptions {
    size_t capacity = 1024;
    DropPolicy drop_policy = DropPolicy::RejectNewest;
    bool enable_stats = true;
    std::string name;
};

struct TopicPublishResult {
    size_t matched_subscribers = 0;
    size_t delivered_subscribers = 0;
    size_t rejected_subscribers = 0;

    explicit operator bool() const noexcept {
        return rejected_subscribers == 0;
    }
};

template<class T>
class TopicSubscription {
public:
    TopicSubscription(TopicSubscription&&) noexcept;
    TopicSubscription& operator=(TopicSubscription&&) noexcept;
    TopicSubscription(const TopicSubscription&) = delete;
    TopicSubscription& operator=(const TopicSubscription&) = delete;
    ~TopicSubscription(); // RAII unsubscribe; idempotent.

    bool try_receive(T& out);

    template<class Rep, class Period>
    CommResult receive_for(T& out, std::chrono::duration<Rep, Period> timeout);

    void close();
    bool is_closed() const;
    CommStats stats() const;
    void set_event_callback(CommEventCallback callback);
};

template<class T>
class Topic {
public:
    explicit Topic(std::string name = {});

    TopicSubscription<T> subscribe(TopicSubscriptionOptions options = {});
    TopicPublishResult publish(const T& value);
    TopicPublishResult publish(T&& value); // T must be copyable when more than one subscriber matches.

    size_t subscriber_count() const;
    void close(); // closes the topic and all active subscriptions.
};

} // namespace executor::comm
```

### 交付、背压与生命周期语义

- 发布只投递到 `publish()` 取得订阅快照时仍有效的 subscription；新订阅者不接收历史消息，
  已注销订阅者不再接收后续消息。
- 每个 subscription 都拥有独立的 `BoundedQueue<T>` 和 `DropPolicy`。慢订阅者的队列满只影响
  自身，不阻塞或回滚其他订阅者的投递。
- `TopicPublishResult` 必须报告匹配、成功和拒绝数量；结果为 false 表示至少一个订阅者未接收，
  不是“所有订阅者均未接收”。逐订阅者的 drop、timeout、深度、延迟和 close 后发送由其 `CommStats`
  与 callback 报告；Topic 可另有轻量 aggregate stats，但不能用它替代逐订阅者诊断。
- 同一 subscription 内的成功消息保持 FIFO。不同 subscription 的投递和消费相互独立，Topic 不承诺
  它们观察到消息的同时性，也不提供跨订阅者的 exactly-once、原子全量投递或事务回滚。
- `Topic<T>` 与仍存活的同一 subscription 可被不同线程使用；实现必须在 publisher 取得订阅快照与
  RAII 注销并发时保证内部状态存活。跨线程停止等待使用 `TopicSubscription::close()` 或
  `Topic::close()`，它们原子关闭内部通道，使 `receive_for()` 轮询后观察 `Closed`，且不丢弃已成功投递的消息。析构执行同一 RAII 注销/关闭，
  但调用方必须先结束对句柄对象本身的并发成员调用，不能一边销毁 C++ 对象一边继续调用它。
- 第一版要求 `T` 可复制。为了减少大型不可变消息的 fan-out 成本，调用方可使用
  `Topic<std::shared_ptr<const T>>`；库不隐式共享可变对象。
- callback 在内部锁外调用并隔离异常，沿用现有通信组件的诊断规则；避免在高频发布或实时路径默认记录日志。

### 实现与验证边界

当前 Topic 由 mutex 保护 subscription registry，并令每个 subscription 复用
`MpscChannel<T>`。发布时先锁 registry 并动态分配稳定订阅引用快照，再在 registry 锁外向各队列
投递，避免慢订阅者阻塞 subscribe/unsubscribe。实现必须定义注销与在途发布的线性化点，并用
`shared_ptr` 或等价所有权保证在途投递不会访问已析构的 subscription。

因此不只是 subscribe/unsubscribe，`publish()` fan-out 本身也不是无锁、无分配或硬实时实现。
未来若控制场景确实需要确定性广播，应另行设计固定订阅数、预分配、
无诊断回调的专用 realtime fan-out 原语，而不能弱化本 Topic 的契约。

---

## P6：通信时序监控

每个通信组件至少提供本地 `stats()`。可选地，`Executor` 增加通信诊断聚合：

```cpp
namespace executor {

struct CommStatusSnapshot {
    std::vector<executor::comm::CommStats> components;
    uint64_t total_dropped = 0;
    uint64_t total_stale_reads = 0;
    uint64_t total_missed_phases = 0;
    uint64_t total_latency_high = 0;
};

class Executor {
public:
    void set_comm_event_callback(executor::comm::CommEventCallback callback);
    CommStatusSnapshot get_comm_status() const;
};

} // namespace executor
```

建议监控指标：

- `drop`：满队列拒绝、DropOldest、KeepLatest 覆盖。
- `latency`：消息从 publish/send 到 receive/drain 的时间。
- `stale`：读者重复读旧 sequence 或显式要求新值但没有新值。
- `missed phase`：等待者发现目标步骤已经错过。
- `producer lag`：生产 sequence 与消费 sequence 的差。
- `consumer lag`：队列深度、未消费消息数或 phase 差。

默认只累计数字；高频事件不默认写日志。超过阈值时才触发 `CommEventCallback`，避免诊断本身干扰实时行为。

---

## 示例

### 采集线程到规划线程

```cpp
executor::comm::MpscChannel<SensorFrame> frames({.capacity = 256});

// producer threads
if (!frames.try_send(read_sensor())) {
    auto stats = frames.stats();
    // stats.dropped_count 可用于告警或降采样。
}

// planner thread
SensorFrame frame;
while (frames.try_receive(frame)) {
    plan(frame);
}
```

### 配置线程到实时控制线程

```cpp
executor::comm::LatestMailbox<ControlConfig> config_box;

// config thread
config_box.publish(load_config());

// realtime cycle
uint64_t seen = 0;
ControlConfig cfg;
uint64_t seq = 0;
if (config_box.try_load_newer_than(seen, cfg, seq)) {
    seen = seq;
    apply_config(cfg);
}
```

### 初始化顺序控制

```cpp
executor::comm::PhaseGate gate("startup");

std::thread worker([&] {
    auto ready = gate.wait_for(2, std::chrono::seconds(3));
    if (ready) {
        run_worker();
    }
});

initialize_io();
gate.advance_to(1);
initialize_planner();
gate.advance_to(2);
```

### 状态快照

```cpp
executor::comm::DoubleBuffer<SystemState> state;

// writer
state.update([](SystemState& next) {
    next.position = read_position();
    next.velocity = read_velocity();
});

// readers
auto snapshot = state.load();
render(snapshot.value);
```

---

## 风险与待决问题

- 预分配 MPSC 节点在目标生产者数量、drop policy 和 payload 下的延迟尾部仍需 benchmark 与 TSAN 压力测试。
- `DoubleBuffer<T>` / `LatestMailbox<T>` 会复制 `T`；大型状态需要明确的不可变 handle 或后续 `SnapshotPtr<T>` API。
- 当前四槽快照只有 `try_load()` 具有固定四次槽检查；`try_publish()` 是系统级 lock-free，但不是
  单次有界/wait-free。仍需以目标平台最大争用时间验收周期预算。
- `submit_after()` 的失败传播策略会影响用户预期，需在 API 文档中明确默认值并提供可选策略。
- 通信事件是否进入 `ExecutorFailureEvent` 需要谨慎。建议先保留独立 `CommEvent`，避免把正常背压误报为任务失败。
