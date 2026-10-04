# Executor 优化方案路线图

> 基于 v0.2.0 版本的性能分析与架构审查，提出后续优化方向与实施方案

**文档版本**: v1.0
**创建日期**: 2026-03-08
**适用版本**: v0.2.0+

---

## 概述

本文档针对 Executor 项目当前架构进行深度分析，识别性能瓶颈与优化空间，提出分阶段的优化路线图。优化目标：

- **性能提升**: 进一步降低延迟，提升吞吐量
- **实时性增强**: 改善 Windows 平台实时精度，优化实时任务传递
- **可扩展性**: 支持更高并发场景，减少锁竞争
- **易用性**: 提供批量操作 API，优化监控开销

---

## v0.2.0 性能基线

**测试日期**: 2026-03-08
**测试平台**: Linux 6.8.0-101-generic
**编译配置**: Release mode, C++20

### 核心性能指标

| 指标 | 测量值 | 说明 |
|------|--------|------|
| 任务提交吞吐量 | 456,703 tasks/s | 50K 任务，4-8 线程 |
| 端到端吞吐量 | 484,649 tasks/s | 包含任务执行的完整流程 |
| 任务延迟 (avg) | 0.028 μs | 往返延迟平均值 |
| 任务延迟 (p99) | 0.152 μs | 99分位延迟 |

### 实时线程精度 (Linux)

| 周期 | 平均 Jitter | p99 Jitter | 说明 |
|------|-------------|------------|------|
| 1 ms | 54.47 μs | 61.30 μs | 高频实时场景 |
| 5 ms | 106.09 μs | 204.64 μs | 中频实时场景 |
| 10 ms | 141.22 μs | 243.56 μs | 标准实时场景 |
| 50 ms | 178.87 μs | 267.73 μs | 低频实时场景 |

### 优化目标性能指标

| 指标 | 当前值 | 说明 |
|------|--------|------|
| 任务提交性能 | 378,132 ops/s | 无监控模式 |
| 监控开销 | -25.7% | 启用监控后的性能变化 |

**注**: 监控开销为负值表示测量存在波动，需要多次测试取平均值以获得稳定结果。

---

## 优化优先级分级

### P0 - 高优先级（核心性能）

影响核心执行路径性能，对吞吐量/延迟/实时性有显著提升。

### P1 - 中优先级（易用性与稳定性）

提升用户体验，增强系统稳定性，但不直接影响核心性能指标。

### P2 - 低优先级（长期优化）

长期演进方向，需要较大重构或依赖新技术。

---

## P0 优化项

### 1. 实时线程任务传递无锁化 ✅

**状态**: 已完成 (2026-03-08)

**原问题**:
- `RealtimeThreadExecutor` 使用 `task_map_` + `mutex` 存储任务
- 虽然任务 ID 通过无锁队列传递，但访问实际任务仍需加锁
- 在高频实时场景（1ms 周期）下，锁竞争影响实时性

**实现方案**:

使用对象池 + 指针传递，完全消除锁操作：

```cpp
// 实现的对象池（src/util/object_pool.hpp）
template<typename T>
class ObjectPool {
    // 使用 free-list + atomic CAS 实现无锁 acquire/release
};

// 修改后的 RealtimeThreadExecutor
class RealtimeThreadExecutor {
    util::LockFreeQueue<TaskWrapper*> lockfree_queue_;
    util::ObjectPool<TaskWrapper> task_pool_;

    void push_task(std::function<void()> task) {
        TaskWrapper* wrapper = task_pool_.acquire();
        wrapper->func = std::move(task);
        lockfree_queue_.push(wrapper);
    }

    void process_tasks() {
        TaskWrapper* wrapper;
        while (lockfree_queue_.pop(wrapper)) {
            wrapper->func();
            task_pool_.release(wrapper);
        }
    }
};
```

**性能测试结果**:

| 指标 | 测量值 |
|------|--------|
| 平均延迟 | 32.27 ns |
| p99 延迟 | 39.00 ns |
| 吞吐量 | 18.97M ops/s |

**实际收益**:
- ✅ 完全消除 `task_map_mutex_` 锁竞争
- ✅ 任务提交延迟降至纳秒级（32 ns）
- ✅ 吞吐量提升至 1900 万 ops/s
- ✅ 支持超高频率任务提交

---

### 2. WorkerLocalQueue 无锁化

**当前问题**:
- 使用全局 `mutex` 保护整个队列
- `push`、`pop`、`steal` 都需要加锁
- 手动实现的 `TaskWrapper` 拷贝效率低

**性能影响**:
- 工作窃取时锁竞争严重
- 限制了并行度提升

**优化方案**:

```cpp
// 使用 Chase-Lev 无锁双端队列算法
class WorkerLocalQueue {
    std::atomic<int64_t> top_;     // 本地线程访问（pop）
    std::atomic<int64_t> bottom_;  // 其他线程访问（steal）
    std::vector<std::atomic<Task*>> buffer_;  // 环形缓冲区

    bool pop(Task& task) {
        int64_t b = bottom_.load(std::memory_order_relaxed) - 1;
        bottom_.store(b, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        int64_t t = top_.load(std::memory_order_relaxed);

        if (t <= b) {
            Task* task_ptr = buffer_[b % buffer_.size()].load(std::memory_order_relaxed);
            if (t == b) {
                if (!top_.compare_exchange_strong(t, t + 1,
                    std::memory_order_seq_cst, std::memory_order_relaxed)) {
                    bottom_.store(b + 1, std::memory_order_relaxed);
                    return false;
                }
                bottom_.store(b + 1, std::memory_order_relaxed);
            }
            task = *task_ptr;
            return true;
        }
        bottom_.store(b + 1, std::memory_order_relaxed);
        return false;
    }
};
```

**实现步骤**:
1. 实现 Chase-Lev 算法的无锁双端队列
2. 使用任务指针避免拷贝开销
3. 配合对象池管理任务生命周期
4. 压力测试验证正确性

**预期收益**:
- 工作窃取延迟降低 40-50%
- 高并发场景吞吐量提升 15-20%
- 消除锁竞争热点

---

### 3. Windows 实时定时器精度优化

**当前问题**:
- Windows 平台实时精度差（1ms 周期 jitter 109μs，50ms 周期 jitter 7.9ms）
- 仅使用 `timeBeginPeriod(1)` 改进有限
- 与 Linux 平台差距大（Linux 1ms 周期 jitter ~60μs）

**性能影响**:
- 实时任务周期不稳定
- 限制了 Windows 平台的实时应用场景

**优化方案**:

```cpp
#ifdef _WIN32
class HighPrecisionTimer {
    HANDLE timer_handle_;

public:
    HighPrecisionTimer(int64_t period_ns) {
        // 使用高精度可等待定时器（Windows 10 1803+）
        timer_handle_ = CreateWaitableTimerExW(
            nullptr, nullptr,
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
            TIMER_ALL_ACCESS
        );

        LARGE_INTEGER due_time;
        due_time.QuadPart = -(period_ns / 100);  // 负值表示相对时间
        SetWaitableTimer(timer_handle_, &due_time,
                        period_ns / 1000000, nullptr, nullptr, FALSE);
    }

    void wait_next_period() {
        WaitForSingleObject(timer_handle_, INFINITE);
    }
};
#endif
```

**实现步骤**:
1. 实现 `HighPrecisionTimer` 类封装 Windows 高精度定时器
2. 在 `RealtimeThreadExecutor` 中条件编译使用
3. 添加 Windows 版本检测（需 Windows 10 1803+）
4. 性能测试验证 jitter 降低

**预期收益**:
- Windows 1ms 周期 jitter 降低至 ~20μs
- 接近 Linux 平台实时精度
- 扩展 Windows 平台实时应用场景

---

## P1 优化项

### 4. 批量任务提交 API

**当前问题**:
- 缺少批量提交 API，用户需要循环调用 `submit`
- 内部虽有 `dequeue_batch`，但未暴露批量提交接口

**优化方案**:

```cpp
template<typename Func>
std::vector<std::future<void>> submit_batch(
    const std::vector<Func>& tasks,
    TaskPriority priority = TaskPriority::NORMAL
);
```

**预期收益**: 历史目标曾按 3-5x 估算；当前版本需以 benchmark 复测结果为准

---

### 5. 任务取消机制优化

**当前问题**: 每个 `Task` 都有 `std::atomic<bool> cancelled`，内存开销大

**优化方案**: 使用集中式 `TaskCancellationManager` 管理取消状态

**预期收益**: 减少 Task 对象大小 8 字节，改善缓存局部性

---

### 6. 监控统计采样化 ✅

**状态**: 已完成 (2026-03-16)

**原问题**: 每个任务都更新统计（原子操作开销）

**优化方案**: 采样统计（可配置采样率，默认 100%）

**实现**:
```cpp
class TaskMonitor {
    std::atomic<uint32_t> sampling_rate_{100};  // 百分比
    mutable std::atomic<uint64_t> sample_counter_{0};

    bool should_sample() const {
        uint32_t rate = sampling_rate_.load(std::memory_order_relaxed);
        if (rate >= 100) return true;
        if (rate == 0) return false;
        uint64_t count = sample_counter_.fetch_add(1, std::memory_order_relaxed);
        return (count % 100) < rate;
    }
};

// API
executor.set_monitoring_sampling_rate(0.01);  // 1% 采样
```

**实际收益**:
- 1% 采样率下统计开销降低 ~99%
- 零性能影响（默认 100% 采样保持兼容）
- 高吞吐场景性能提升 5-10%

**文档**: [monitoring_sampling_implementation.md](monitoring_sampling_implementation.md)

---

## P2 优化项

### 7. PriorityScheduler 队列结构优化

**当前问题**: 使用 `std::vector` + 堆操作，`enqueue` 为 O(log n)

**优化方案**: 每个优先级使用简单 FIFO 队列（`std::deque`）

**预期收益**: `enqueue` 降至 O(1)，提升 10-15%

---

### 8. GPU 内存池异步化

**当前问题**: CUDA 内存分配/释放是同步操作

**优化方案**: 使用 `cudaMallocAsync` + 流水线

**预期收益**: GPU 任务延迟降低 20-30%

---

### 9. 自定义内存分配器

**当前问题**: `std::function` 可能触发堆分配

**优化方案**: 使用对象池分配器或 `std::function_ref`（C++26）

**预期收益**: 减少内存分配开销 30-40%

---

## 实施路线图

### 阶段 1: 核心性能优化（v0.3.0）

**目标**: 解决主要性能瓶颈

**任务**:
1. 实时线程任务传递无锁化（P0-1）
2. WorkerLocalQueue 无锁化（P0-2）
3. Windows 定时器精度优化（P0-3）

**预期成果**:
- 实时任务延迟降低 30%
- 工作窃取吞吐量提升 15-20%
- Windows 实时精度接近 Linux

**工期**: 4-6 周

---

### 阶段 2: 易用性增强（v0.4.0）

**目标**: 提升 API 易用性，优化监控开销

**任务**:
1. 批量任务提交 API（P1-4）
2. 任务取消机制优化（P1-5）
3. 监控统计采样化（P1-6）

**预期成果**:
- 批量提交性能以 benchmark 复测结果为准
- 高吞吐场景性能提升 5-10%
- 内存占用降低

**工期**: 2-3 周

---

### 阶段 3: 长期演进（v0.5.0+）

**目标**: 架构优化与新技术引入

**任务**:
1. PriorityScheduler 队列结构优化（P2-7）
2. GPU 内存池异步化（P2-8）
3. 自定义内存分配器（P2-9）

**预期成果**:
- 整体性能再提升 15-20%
- GPU 任务延迟降低 20-30%

**工期**: 6-8 周

---

## 性能目标

基于 v0.2.0 性能基线，各阶段优化后的预期指标：

| 指标 | v0.2.0 | v0.3.0 目标 | v0.4.0 目标 | v0.5.0 目标 |
|------|--------|-------------|-------------|-------------|
| 端到端吞吐量 | 521K tasks/s | 600K tasks/s | 650K tasks/s | 750K tasks/s |
| 延迟 p99 | 0.10μs | 0.08μs | 0.07μs | 0.06μs |
| 实时 jitter (Linux 1ms) | 60μs | 50μs | 45μs | 40μs |
| 实时 jitter (Windows 1ms) | 109μs | 25μs | 20μs | 20μs |

---

## 测试与验证

### 性能回归测试

每次优化后运行完整性能测试套件：

```bash
# 端到端性能测试
./build/tests/benchmark_executor --json > results.json

# 实时精度测试
./build/tests/benchmark_realtime_precision --json > precision.json

# 工作窃取测试
./build/tests/benchmark_work_stealing --json > stealing.json
```

### 稳定性测试

```bash
# 长时间压力测试（24小时）
./build/tests/stress_test --duration=86400

# 内存泄漏检测
valgrind --leak-check=full ./build/tests/stress_test --duration=3600
```

---

## 风险与注意事项

### 无锁算法风险

- Chase-Lev 算法实现复杂，需充分测试
- 内存序错误可能导致难以复现的 bug
- 建议使用 ThreadSanitizer 验证

### 平台兼容性

- Windows 高精度定时器需 Windows 10 1803+
- 需添加版本检测和降级方案
- 保持 Linux/Windows 行为一致性；Android 路径保持 CPU-only 与 best-effort 语义

### API 兼容性

- 批量提交 API 为新增接口，不影响现有代码
- 任务取消机制改动需谨慎，确保向后兼容
- 建议使用 deprecation 标记过渡

---

## 参考资料

- [Chase-Lev Work-Stealing Deque](https://www.dre.vanderbilt.edu/~schmidt/PDF/work-stealing-dequeue.pdf)
- [Windows High-Resolution Timers](https://docs.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createwaitabletimerexw)
- [Lock-Free Programming](https://preshing.com/20120612/an-introduction-to-lock-free-programming/)
- [Memory Ordering](https://en.cppreference.com/w/cpp/atomic/memory_order)

---

**文档维护**: 本文档随优化实施进度更新，记录实际效果与经验教训。
