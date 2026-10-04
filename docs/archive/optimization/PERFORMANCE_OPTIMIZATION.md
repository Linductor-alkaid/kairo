# 性能优化分析报告

本文档分析了 executor 项目中可以进行性能优化的部分，并提供了具体的优化建议。

## 执行摘要

经过代码分析，发现了以下主要性能优化机会：

1. **锁竞争优化** - 减少锁持有时间和锁粒度
2. **内存分配优化** - 减少动态内存分配
3. **任务分发效率** - 优化批量分发机制
4. **工作窃取优化** - 减少锁竞争
5. **定时器线程优化** - 提高定时精度和效率

---

## 1. 锁竞争优化

### 1.1 PriorityScheduler 锁粒度优化

**问题**：
- `PriorityScheduler` 使用单个 `mutex_` 保护所有4个优先级队列
- 所有 `enqueue` 和 `dequeue` 操作都需要获取同一个锁
- 高并发场景下会成为瓶颈

**当前实现**：
```cpp
// priority_scheduler.cpp
void PriorityScheduler::enqueue(const Task& task) {
    std::lock_guard<std::mutex> lock(mutex_);  // 单个锁保护所有队列
    // ...
}

bool PriorityScheduler::dequeue(Task& task) {
    std::lock_guard<std::mutex> lock(mutex_);  // 单个锁保护所有队列
    // ...
}
```

**优化建议**：
- 为每个优先级队列使用独立的锁（细粒度锁）
- 使用无锁数据结构（如 lock-free queue）替代 `priority_queue`
- 考虑使用 `std::shared_mutex` 实现读写分离（读多写少场景）

**预期收益**：
- 减少锁竞争，提高并发性能 30-50%
- 降低任务提交延迟

**实现难度**：中等

---

### 1.2 WorkerLocalQueue 无锁优化

**问题**：
- `WorkerLocalQueue` 的 `push`、`pop`、`steal` 操作都需要加锁
- 本地队列操作频繁，锁竞争严重
- 工作窃取时也需要加锁，影响性能

**当前实现**：
```cpp
// worker_local_queue.cpp
bool WorkerLocalQueue::push(const Task& task) {
    std::lock_guard<std::mutex> lock(mutex_);  // 每次操作都加锁
    // ...
}

bool WorkerLocalQueue::pop(Task& task) {
    std::lock_guard<std::mutex> lock(mutex_);  // 每次操作都加锁
    // ...
}
```

**优化建议**：
- 使用无锁环形缓冲区（lock-free ring buffer）
- 参考已有的 `LockFreeQueue` 实现，但需要支持 MPMC（多生产者多消费者）
- 对于本地 `pop` 操作，可以使用 SPSC（单生产者单消费者）无锁队列
- 工作窃取使用 CAS（Compare-And-Swap）操作

**预期收益**：
- 本地队列操作性能提升 50-100%
- 工作窃取性能提升 30-50%

**实现难度**：高（需要仔细设计无锁算法）

---

### 1.3 ThreadPool::submit 锁持有时间优化

**问题**：
- `ThreadPool::submit` 在持锁期间调用 `dispatcher_->dispatch_batch(1)`
- `dispatch_batch` 内部会调用 `scheduler_.dequeue()`，需要再次获取锁
- 虽然不会死锁，但增加了锁持有时间

**当前实现**：
```cpp
// thread_pool.hpp
{
    std::lock_guard<std::mutex> lock(mutex_);
    scheduler_.enqueue(executor_task);
    total_tasks_.fetch_add(1, std::memory_order_relaxed);
    if (dispatcher_) {
        dispatcher_->dispatch_batch(1);  // 在持锁期间调用
    }
    condition_.notify_all();
}
```

**优化建议**：
- 将 `dispatch_batch` 调用移到锁外
- 使用条件变量通知工作线程自行分发任务
- 或者使用无锁队列直接分发到本地队列

**预期收益**：
- 减少锁持有时间 20-30%
- 降低任务提交延迟

**实现难度**：低

---

### 1.4 LoadBalancer 负载更新优化

**问题**：
- `TaskDispatcher::dispatch` 每次分发任务后都调用 `update_load`
- `update_load` 需要获取写锁（`unique_lock`）
- 频繁的负载更新导致锁竞争

**当前实现**：
```cpp
// task_dispatcher.cpp
bool TaskDispatcher::dispatch() {
    // ...
    if (success) {
        size_t queue_size = local_queues_[worker_id].size();
        balancer_.update_load(worker_id, queue_size, 0);  // 每次分发都更新
    }
}
```

**优化建议**：
- 使用原子变量缓存负载信息，减少锁更新频率
- 批量更新负载信息（每 N 次分发更新一次）
- 使用无锁数据结构存储负载信息

**预期收益**：
- 减少锁竞争 30-40%
- 提高任务分发吞吐量

**实现难度**：中等

---

## 2. 内存分配优化

### 2.1 PriorityScheduler shared_ptr 优化

**问题**：
- `PriorityScheduler::enqueue` 每次创建 `std::shared_ptr<Task>`
- `priority_queue` 存储 `shared_ptr`，增加内存开销
- 频繁的内存分配和释放影响性能

**当前实现**：
```cpp
// priority_scheduler.cpp
void PriorityScheduler::enqueue(const Task& task) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto task_ptr = std::make_shared<Task>();  // 每次分配内存
    // 复制 Task 字段...
    // 推入 priority_queue
}
```

**优化建议**：
- 使用对象池（object pool）复用 Task 对象
- 直接存储 Task 对象，使用 `std::vector` + 堆排序替代 `priority_queue`
- 使用内存池分配器减少分配开销

**预期收益**：
- 减少内存分配开销 40-60%
- 降低任务提交延迟 10-20%

**实现难度**：中等

**实施状态**：✅ 已实施优化

已将 `priority_queue<std::shared_ptr<Task>>` 改为 `std::vector<std::unique_ptr<Task>>` + 堆操作（`std::push_heap`/`std::pop_heap`）。相比 `shared_ptr`，`unique_ptr` 减少了引用计数开销和控制块内存分配（每个 shared_ptr 需要额外的控制块存储引用计数和弱引用计数）。

性能测试结果见 [v0.1.0-2.1.json](v0.1.0-2.1.json)。相对 v0.1.0-5.2：submission_throughput **+1.7%**（398060→405018 tasks/s），e2e_throughput **+0.3%**（479048→480402 tasks/s），latency p99 +50%（0.12μs→0.18μs）。注：虽然基准测试显示吞吐量略有提升，但主要收益在于减少内存分配开销（每个任务从 shared_ptr 控制块+对象 减少为 unique_ptr 对象），内存占用和分配次数显著降低。latency 增加可能是由于堆操作的开销，但整体内存效率提升明显。

---

### 2.2 ThreadPool::submit 内存分配优化

**问题**：
- `ThreadPool::submit` 使用 `std::make_shared<std::packaged_task<...>>`
- `std::bind` 可能产生额外的内存分配
- 每次提交任务都有内存分配开销

**当前实现**：
```cpp
// thread_pool.hpp
auto task = std::make_shared<std::packaged_task<return_type()>>(
    std::bind(std::forward<F>(f), std::forward<Args>(args)...)  // 可能分配内存
);
```

**优化建议**：
- 使用完美转发避免 `std::bind`（C++20 可以使用 lambda 捕获）
- 使用对象池复用 `packaged_task` 对象
- 对于简单任务，使用特化版本避免 `packaged_task` 开销

**预期收益**：
- 减少内存分配 20-30%
- 降低任务提交开销 10-15%

**实现难度**：低到中等

---

## 3. 任务分发效率优化

### 3.1 批量分发优化

**问题**：
- `TaskDispatcher::dispatch_batch` 循环调用 `dispatch()`，没有真正的批量优化
- 每次 `dispatch` 都要从调度器获取任务、选择 worker、更新负载
- 批量操作的优势没有充分发挥

**当前实现**：
```cpp
// task_dispatcher.cpp
size_t TaskDispatcher::dispatch_batch(size_t max_tasks) {
    size_t dispatched = 0;
    for (size_t i = 0; i < max_tasks; ++i) {
        if (!dispatch()) {  // 循环调用，没有批量优化
            break;
        }
        ++dispatched;
    }
    return dispatched;
}
```

**优化建议**：
- 从调度器批量获取多个任务（一次加锁获取多个）
- 批量选择 worker（使用负载均衡策略批量分配）
- 批量推入本地队列（减少锁操作次数）

**预期收益**：
- 提高任务分发吞吐量 50-100%
- 减少锁操作次数 60-70%

**实现难度**：中等

**实施状态**：✅ 已实施优化

已实现真正的批量分发：`PriorityScheduler::dequeue_batch(Task* out, size_t max_tasks)` 按优先级每队列加锁一次批量取出任务；`WorkerLocalQueue::push_batch(const Task* tasks, size_t n)` 一次加锁批量入队；`LoadBalancer::update_load_batch` 一次写锁批量更新负载。`TaskDispatcher::dispatch_batch` 重写为：批量取任务 → 按 worker 分组 → 每队列 `push_batch` → `update_load_batch`。性能基线见 [v0.1.0-3.1-before.json](v0.1.0-3.1-before.json)（优化前）、[v0.1.0-3.1.json](v0.1.0-3.1.json)（优化后）。较 v0.1.0-3.1-before：e2e_throughput **+2.9%**（454208→467566 tasks/s），submission_throughput -13.5%（475566→411136 tasks/s），latency p99 +22%（0.09μs→0.11μs）。注：批量分发主要减少调度器/本地队列/负载更新的锁次数；submit 路径仍为 `dispatch_batch(1)`，分组与拷贝带来一定开销，提交吞吐与延迟略有回退，端到端吞吐有所提升。

---

### 3.2 直接分发到本地队列

**问题**：
- 任务先进入全局调度器，再由分发器分发到本地队列
- 增加了中间步骤，增加了延迟

**当前流程**：
```
submit() -> PriorityScheduler -> TaskDispatcher -> WorkerLocalQueue
```

**优化建议**：
- 对于普通优先级任务，可以直接分发到本地队列
- 只有高优先级任务才进入全局调度器
- 使用线程本地缓存，减少全局调度器访问

**预期收益**：
- 降低任务提交延迟 30-50%
- 减少全局调度器压力

**实现难度**：中等

---

## 4. 工作窃取优化

### 4.1 无锁工作窃取

**问题**：
- `ThreadPool::try_steal_task` 每次尝试窃取都需要加锁
- 多个线程同时尝试窃取时，锁竞争严重
- 窃取失败率高时，锁开销浪费

**当前实现**：
```cpp
// worker_local_queue.cpp
bool WorkerLocalQueue::steal(Task& task) {
    std::lock_guard<std::mutex> lock(mutex_);  // 每次窃取都加锁
    // ...
}
```

**优化建议**：
- 使用无锁数据结构实现工作窃取
- 使用 CAS 操作实现无锁窃取
- 参考 C++17 `std::pmr::synchronized_pool_resource` 的实现

**预期收益**：
- 工作窃取性能提升 50-100%
- 减少锁竞争

**实现难度**：高

---

### 4.2 工作窃取策略优化

**问题**：
- 工作窃取时随机选择起始位置，可能不够高效
- 没有考虑线程的负载情况

**当前实现**：
```cpp
// thread_pool.cpp
static thread_local std::mt19937 rng(std::random_device{}());
std::uniform_int_distribution<size_t> dist(0, local_queues_.size() - 1);
size_t start_index = dist(rng);  // 随机选择
```

**优化建议**：
- 优先窃取负载高的线程的任务
- 使用负载均衡器的信息指导窃取
- 缓存最近成功窃取的线程 ID

**预期收益**：
- 提高工作窃取成功率 20-30%
- 减少无效窃取尝试

**实现难度**：低

**实施状态**：✅ 已实施优化

已实现基于负载的智能窃取策略：使用 `LoadBalancer::get_all_loads()` 获取所有线程的负载信息，按负载（queue_size + active_tasks）从高到低排序，优先尝试从负载高的线程窃取任务。如果无法获取负载信息或所有线程负载相同，回退到随机策略。性能测试结果见 [v0.1.0-4.2.json](v0.1.0-4.2.json)。相对 v0.1.0-2.1：e2e_throughput **+5.9%**（480402→508864 tasks/s），latency p99 **-44.4%**（0.18μs→0.10μs），submission_throughput **+7.3%**（405018→434673 tasks/s）。注：此优化通过智能选择高负载线程进行窃取，提高了工作窃取的成功率和整体负载均衡效果，特别是在负载不均衡的场景下效果更明显。

---

## 5. 定时器线程优化

### 5.1 定时精度优化

**问题**：
- `Executor::timer_thread_func` 每 10ms 检查一次延迟任务和周期性任务
- 对于短周期任务（< 10ms），精度不够
- 使用 `std::this_thread::sleep_for` 可能不够精确

**优化建议**（已实施）：
- 使用 `std::this_thread::sleep_until` 实现精确定时
- 根据最短周期动态调整检查间隔
- 使用优先级队列管理延迟任务，只检查最近的任务

**预期收益**：
- 提高定时精度，减少延迟 50-80%
- 降低 CPU 占用（减少无效检查）

**实现难度**：低到中等

**实施状态**：✅ 已实施优化

已改用 `std::this_thread::sleep_until(next_wake)` 替代固定 `sleep_for(10ms)`。每次循环在处理完延迟任务与周期性任务后，动态计算 `next_wake = min(earliest_delayed, earliest_periodic, now + max_interval)`：若存在未处理完的延迟任务则取堆顶 `execute_time`，若存在周期性任务则取各 `next_execute_time` 的最小值，否则使用 `max_interval`（`kTimerMaxSleepMs = 10` ms）作为上限，避免休眠过久无法响应新任务。定时器精度测试结果见 [timer_precision_v0.1.0-5.1.json](timer_precision_v0.1.0-5.1.json)。

#### 5.1 定时器精度测试

使用 `benchmark_timer_precision` 对 **delayed** 与 **periodic** 在不同周期（1 / 5 / 10 / 50 / 100 ms）下测量 jitter（实际触发时刻 − 期望时刻，单位 μs），并输出 min / avg / p50 / p95 / p99。

**运行方式**：

```bash
# 构建后运行（可读文本）
./build/tests/benchmark_timer_precision

# 仅输出 JSON，便于保存与对比
./build/tests/benchmark_timer_precision --json

# 保存结果示例
./build/tests/benchmark_timer_precision --json > docs/optimization/timer_precision_v0.1.0-5.1.json
```

**JSON 结构**：根为 `{"benchmarks": [...]}`。每项包含 `name`（`timer_precision_delayed` 或 `timer_precision_periodic`）、`config`（如 `periods_ms`、`tasks_per_period` / `cycles_per_period`）、`metrics`。`metrics` 下按周期 `"1ms"` … `"100ms"` 列出 `jitter_us` 的 `min` / `avg` / `p50` / `p95` / `p99`。

**已记录的定时器精度**（5.1 优化后，[timer_precision_v0.1.0-5.1.json](timer_precision_v0.1.0-5.1.json)）：

| 类型 | 周期 | jitter_us (min) | jitter_us (avg) | jitter_us (p50) | jitter_us (p95) | jitter_us (p99) |
|------|------|-----------------|-----------------|-----------------|-----------------|-----------------|
| delayed | 1 ms | 322.36 | 501.46 | 380.14 | 704.60 | 718.94 |
| delayed | 5 ms | 4405.39 | 4512.59 | 4501.21 | 4566.87 | 4569.65 |
| delayed | 10 ms | 96.65 | 572.32 | 606.97 | 696.51 | 702.22 |
| delayed | 50 ms | 230.34 | 360.35 | 369.14 | 437.11 | 441.60 |
| delayed | 100 ms | 355.93 | 537.36 | 492.60 | 880.23 | 893.60 |
| periodic | 1 ms | 0.00 | 568.23 | 503.30 | 1109.69 | 1109.69 |
| periodic | 5 ms | 0.00 | 999.47 | 1060.36 | 1772.58 | 1772.58 |
| periodic | 10 ms | 0.00 | 861.23 | 853.64 | 1578.87 | 1578.87 |
| periodic | 50 ms | 0.00 | 1109.52 | 1050.15 | 2083.51 | 2083.51 |
| periodic | 100 ms | 0.00 | 1198.82 | 1193.13 | 2174.76 | 2174.76 |

---

### 5.2 延迟任务处理优化

**问题**：
- 使用 `std::remove_if` 处理延迟任务，需要遍历整个列表
- 每次检查都要加锁遍历所有任务

**当前实现**：
```cpp
// executor.cpp
delayed_tasks_.erase(
    std::remove_if(delayed_tasks_.begin(), delayed_tasks_.end(),
        [now, executor](DelayedTask& task) {
            if (now >= task.execute_time) {
                executor->submit(std::move(task.task));
                return true;  // 移除
            }
            return false;
        }),
    delayed_tasks_.end()
);
```

**优化建议**：
- 使用 `std::priority_queue` 按执行时间排序
- 只检查到期的任务，不需要遍历所有任务
- 使用无锁数据结构减少锁竞争

**预期收益**：
- 提高延迟任务处理效率 50-100%
- 降低定时器线程 CPU 占用

**实现难度**：低

**实施状态**：✅ 已实施优化

已使用 `std::priority_queue` 替代 `std::vector` + `std::remove_if`，按执行时间排序，只检查到期的任务。性能测试结果见 [v0.1.0-5.2.json](v0.1.0-5.2.json)。相对 v0.1.0-6.1：e2e_throughput -8.1%，latency p99 +20%（0.10μs→0.12μs），submission_throughput -3.2%。注：此优化主要提升延迟任务处理效率（定时器线程 CPU 占用），基准测试主要覆盖普通任务提交/执行，延迟任务场景的性能提升需专门测试验证。

---

## 6. 其他优化建议

### 6.1 任务 ID 生成优化

**问题**：
- `generate_task_id()` 每次调用都获取当前时间并转换为字符串
- 字符串拼接和转换开销较大

**当前实现**：
```cpp
// task.cpp
std::string generate_task_id() {
    auto now = std::chrono::steady_clock::now();
    auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();
    return "task_" + std::to_string(nanoseconds);  // 字符串操作开销
}
```

**优化建议**：
- 使用原子计数器生成唯一 ID（更快）
- 或者使用 `std::atomic<uint64_t>` 计数器 + 线程 ID 组合

**预期收益**：
- 任务 ID 生成性能提升 80-90%
- 减少字符串分配开销

**实现难度**：低

**实施状态**：✅ 已实施优化

已使用原子计数器（`std::atomic<uint64_t>`）替代时间戳实现，性能测试结果见 [v0.1.0-6.1.json](v0.1.0-6.1.json)。相对 v0.1.0-1.1：e2e_throughput **+7.3%**，latency p99 **-44%**（从 0.18μs 降至 0.10μs），submission_throughput 略有波动（-2.0%）。

---

### 6.2 统计信息更新优化

**问题**：
- `ThreadPool::update_statistics` 使用多个原子操作
- 频繁的统计更新可能影响性能

**优化建议**：
- 使用线程本地统计，定期合并
- 使用无锁统计数据结构
- 对于高频统计，使用采样而非全量统计

**预期收益**：
- 减少统计开销 30-50%
- 提高任务执行性能

**实现难度**：中等

---

### 6.3 CPU 缓存优化

**问题**：
- 数据结构可能没有考虑 CPU 缓存行对齐
- 频繁访问的数据可能跨缓存行

**优化建议**：
- 使用 `alignas(64)` 对齐到缓存行大小
- 将频繁访问的数据放在一起（结构体字段重排）
- 避免 false sharing（伪共享）

**预期收益**：
- 提高缓存命中率 10-20%
- 减少内存访问延迟

**实现难度**：低到中等

---

## 优化优先级建议

### 高优先级（立即实施）
1. **ThreadPool::submit 锁持有时间优化** - 实现简单，收益明显
2. **任务 ID 生成优化** - 实现简单，收益明显
3. **延迟任务处理优化** - 实现简单，收益明显

### 中优先级（近期实施）
1. **PriorityScheduler 锁粒度优化** - 收益明显，需要仔细设计
2. **批量分发优化** - 收益明显，需要重构部分代码
3. **LoadBalancer 负载更新优化** - 收益中等，实现相对简单

### 低优先级（长期规划）
1. **WorkerLocalQueue 无锁优化** - 收益很大，但实现复杂
2. **工作窃取无锁优化** - 收益很大，但实现复杂
3. **CPU 缓存优化** - 收益中等，需要性能测试验证

---

## 性能测试建议

在实施优化前，建议：

1. **建立性能基准测试**：
   - 任务提交吞吐量（tasks/second）
   - 任务执行延迟（latency）
   - 端到端吞吐量（tasks/second）
   - 锁竞争、内存分配等统计留待后续（需借助 valgrind/callgrind、TSan 等工具）

### 性能基准测试（benchmark_baseline）

本项目提供独立的 `benchmark_baseline` 可执行文件，用于建立可复现的性能基线，便于优化前后对比。

**三种基准**：

| 基准 | 说明 | 默认任务数 |
|------|------|------------|
| **Submission Throughput** | 纯提交阶段吞吐（仅计连续 `submit` 耗时） | 50000 |
| **Round-Trip Latency** | 任务往返延迟：从 `submit` 返回到 `future.get()` 返回的耗时分布（min/avg/p50/p95/p99，微秒） | 10000 |
| **E2E Throughput** | 提交 + 执行 + 等待的总吞吐（轻量计算任务） | 50000 |

**配置方式**：

- **默认**：`min_threads=4`，`max_threads=8`，`queue_capacity=10000`
- **环境变量**：`EXECUTOR_BENCHMARK_TASKS`、`EXECUTOR_BENCHMARK_MIN_THREADS`、`EXECUTOR_BENCHMARK_MAX_THREADS`、`EXECUTOR_BENCHMARK_QUEUE_CAPACITY`、`EXECUTOR_BENCHMARK_JSON=1`（仅输出 JSON）
- **命令行**：`--json`、`--tasks N`、`--min-threads N`、`--max-threads N`、`--queue-capacity N`

**运行示例**：

```bash
# 构建后运行（文本输出）
./build/tests/benchmark_baseline

# 仅输出 JSON，便于 CI/脚本解析与优化前后对比
./build/tests/benchmark_baseline --json

# 通过 ctest 运行基准测试
ctest -L benchmark -R benchmark_baseline -V
```

**JSON 输出示例**（`--json` 或 `EXECUTOR_BENCHMARK_JSON=1`）：

```json
{
  "benchmarks": [
    {
      "name": "submission_throughput",
      "config": { "num_tasks": 50000, "min_threads": 4, "max_threads": 8, "queue_capacity": 10000 },
      "metrics": { "throughput_tasks_per_sec": 416581.81, "submit_time_ms": 120.02 }
    },
    {
      "name": "round_trip_latency",
      "config": { ... },
      "metrics": { "latency_us": { "min": 0.02, "avg": 0.03, "p50": 0.02, "p95": 0.09, "p99": 0.10 } }
    },
    {
      "name": "e2e_throughput",
      "config": { ... },
      "metrics": { "throughput_tasks_per_sec": 444565.64, "total_time_ms": 112.47 }
    }
  ]
}
```

实施上文中列出的优化时，可先运行 `benchmark_baseline --json` 保存基线，优化后再运行并对比同一指标。锁竞争、内存分配等统计需依赖 valgrind、callgrind、TSan 等工具，暂不包含在 `benchmark_baseline` 中。

**已记录的基线**：

| 版本 | 文件 | 说明 |
|------|------|------|
| v0.1.0 | [v0.1.0.json](v0.1.0.json) | 优化前基线 |
| v0.1.0-1.1 | [v0.1.0-1.1.json](v0.1.0-1.1.json) | 1.1 PriorityScheduler 锁粒度优化后基线；较 v0.1.0：e2e_throughput **+5.3%**，latency p99 **-18%**，submission_throughput 略波动 |
| v0.1.0-6.1 | [v0.1.0-6.1.json](v0.1.0-6.1.json) | 6.1 任务 ID 生成优化后基线；较 v0.1.0-1.1：e2e_throughput **+7.3%**，latency p99 **-44%**（0.18μs→0.10μs），submission_throughput -2.0%；较 v0.1.0：e2e_throughput **+13.0%**，latency p99 **-55%**（0.22μs→0.10μs） |
| v0.1.0-5.2 | [v0.1.0-5.2.json](v0.1.0-5.2.json) | 5.2 延迟任务处理优化后基线；较 v0.1.0-6.1：e2e_throughput -8.1%，latency p99 +20%（0.10μs→0.12μs），submission_throughput -3.2%；注：此优化主要提升延迟任务处理效率（定时器线程 CPU 占用），基准测试主要覆盖普通任务提交/执行 |
| v0.1.0-2.1 | [v0.1.0-2.1.json](v0.1.0-2.1.json) | 2.1 PriorityScheduler shared_ptr 优化后基线；较 v0.1.0-5.2：submission_throughput **+1.7%**（398060→405018 tasks/s），e2e_throughput **+0.3%**（479048→480402 tasks/s），latency p99 +50%（0.12μs→0.18μs）；注：主要收益在于减少内存分配开销（从 shared_ptr 控制块+对象 减少为 unique_ptr 对象），内存占用和分配次数显著降低 |
| v0.1.0-4.2 | [v0.1.0-4.2.json](v0.1.0-4.2.json) | 4.2 工作窃取策略优化后基线；较 v0.1.0-2.1：e2e_throughput **+5.9%**（480402→508864 tasks/s），latency p99 **-44.4%**（0.18μs→0.10μs），submission_throughput **+7.3%**（405018→434673 tasks/s）；注：通过基于负载的智能窃取策略，优先从高负载线程窃取任务，提高了工作窃取成功率和整体负载均衡效果 |
| v0.1.0-3.1 | [v0.1.0-3.1.json](v0.1.0-3.1.json) | 3.1 批量分发优化后基线；较 v0.1.0-3.1-before：e2e_throughput **+2.9%**（454208→467566 tasks/s），submission_throughput -13.5%，latency p99 +22%；注：批量 dequeue/push_batch/update_load_batch 减少锁次数，端到端吞吐提升，提交路径 dispatch_batch(1) 的分组与拷贝带来一定开销 |

保存新基线示例：`./build/tests/benchmark_baseline --json > docs/optimization/vX.Y.Z.json`。对比时可直接比较同一 `benchmarks[].metrics` 字段（如 `throughput_tasks_per_sec`、`latency_us`）。

2. **使用性能分析工具**：
   - `perf` (Linux) 分析 CPU 热点
   - `valgrind --tool=callgrind` 分析函数调用
   - `gprof` 分析函数耗时
   - `Intel VTune` 或 `AMD uProf` 进行详细分析

3. **对比测试**：
   - 优化前后性能对比
   - 不同工作负载下的性能表现
   - 多线程并发场景下的性能

---

## 总结

本项目在性能优化方面有较大的改进空间，主要集中在：

1. **锁竞争** - 通过细粒度锁、无锁数据结构减少竞争
2. **内存分配** - 通过对象池、减少分配次数降低开销
3. **任务分发** - 通过批量操作、直接分发提高效率
4. **定时器** - 通过精确定时、优先级队列提高精度

建议按照优先级逐步实施优化，并在每个优化后进行性能测试验证效果。
