# 普通执行路径堆分配优化方案

> **日期**：2026-10-13  
> **基于**：v0.7.0 M0 清债成果  
> **目标版本**：v0.7.2  
> **性能基线**：当前 0.7.0（9 allocs/task）  

## 1. 当前状态分析

### 1.1 v0.7.0 M0 已完成的优化（CR-107 第一阶段）

**基线对比**（`benchmark_submit_allocations`，确定性计数）：

| 路径 | v0.6.1 | v0.7.0 M0 | 改进 |
|------|--------|----------|------|
| submit void no-args | 10 allocs/task | **9 allocs/task** | -1 (10%) |
| submit value-returning | 10 allocs/task | **9 allocs/task** | -1 (10%) |
| submit with runtime args | 10 allocs/task | **9 allocs/task** | -1 (10%) |
| tracked no-args | 19.13 allocs/task | 19.13 allocs/task | 0 |
| tracked with args | 19.13 allocs/task | 19.13 allocs/task | 0 |

**字节数**：816 → 768 bytes/task (-6%)

**已完成的优化项**：
- CR-110：promise 与就绪标志合并为单次分配 `CompletionState`（旧：`make_shared` × 2 + 双控制块）
- CR-112：`task_dispatcher` 的 `unique_ptr<shared_lock>` → `optional<shared_lock>`（免一次堆分配）
- CR-121：`before_publish_hook` 访问加原子标志快速路径（常态零 shared_ptr 原子读）
- CR-122：`push_tasks_batch` 临时 vector → thread_local 便签

### 1.2 剩余的 9 次堆分配来源

根据 CR-107 分析和 M0 结论，剩余 9 次分配的来源：

1. **Promise/Future 基础设施**（~3-4 次）：
   - `std::promise<T>` 内部状态（1 次）
   - `std::future<T>` 共享状态（1 次，与 promise 可能共享控制块）
   - CompletionState（1 次，M0 已优化合并）
   - 异常处理相关（如果有异常，额外 1-2 次）

2. **可调用对象包装**（~2-3 次）：
   - `std::function` 堆分配（捕获的 lambda，1-2 次）
   - `std::bind` 结果包装（如果有参数绑定，1 次）
   - Task 对象本身（1 次）

3. **调度与监控**（~2-3 次）：
   - TaskMonitor 采样记录（如果采样，1 次）
   - DrainBag 引用计数块（2 次，M0 分析提到）
   - 优先级队列节点（1 次）

4. **其他**（~1 次）：
   - 任务 ID 字符串（如果生成，1 次）
   - 临时对象/容器（视路径）

### 1.3 结构性约束

M0 结论指出，部分分配受以下约束：
- **move-only 可调用间接层**：`std::function` 需要类型擦除
- **双 std::function 接口**：公开 API 契约
- **Task 含 atomic**：不可移动，必须堆分配
- **快照字符串公开契约**：诊断接口返回 `std::string`

## 2. 优化方案设计

### 阶段一：对象池与小对象优化（v0.7.2，目标 9 → 7 allocs/task）

#### 2.1.1 Task 对象池扩展（优先级最高）

**问题**：每个任务都分配 Task 对象，当前只有 LockFreeTaskExecutor 有 wrapper 池。

**方案**：扩展 ThreadPool 的 Task 对象池：

```cpp
// 在 ThreadPool 中添加
class ThreadPool {
private:
    // Task 对象池（per-worker，避免锁竞争）
    struct TaskPool {
        static constexpr size_t kPoolCapacity = 256;  // per-worker
        
        std::array<Task*, kPoolCapacity> pool_;
        std::atomic<size_t> head_{0};  // 栈顶（lock-free LIFO）
        
        Task* acquire() {
            size_t h = head_.load(std::memory_order_relaxed);
            while (h > 0) {
                if (head_.compare_exchange_weak(
                        h, h - 1, std::memory_order_acquire,
                        std::memory_order_relaxed)) {
                    return pool_[h - 1];
                }
            }
            return nullptr;  // 池空，回退到堆分配
        }
        
        bool release(Task* task) {
            size_t h = head_.load(std::memory_order_relaxed);
            if (h >= kPoolCapacity) return false;  // 池满
            
            // 重置任务状态（关键：避免泄漏捕获）
            task->function = {};  // 清空 std::function
            task->priority = TaskPriority::NORMAL;
            // ... 重置其他字段
            
            if (head_.compare_exchange_strong(
                    h, h + 1, std::memory_order_release,
                    std::memory_order_relaxed)) {
                pool_[h] = task;
                return true;
            }
            return false;
        }
    };
    
    std::vector<TaskPool> task_pools_;  // per-worker
    
    Task* acquire_task(size_t worker_id) {
        if (Task* task = task_pools_[worker_id].acquire()) {
            return task;  // 从池获取，零堆分配
        }
        return new Task{};  // 回退到堆分配
    }
    
    void release_task(Task* task, size_t worker_id) {
        if (!task_pools_[worker_id].release(task)) {
            delete task;  // 池满，释放到堆
        }
    }
};
```

**预期收益**：
- 稳态（预热后）：**-1 alloc/task**（Task 从池获取）
- 命中率预期：> 95%（per-worker 池避免竞争，256 容量足够吸收突发）
- 性能影响：acquire/release 各 ~10-20ns（lock-free 栈操作）

#### 2.1.2 小对象 std::function 内联存储

**问题**：`std::function` 对于小闭包（<= 16 bytes）也会堆分配。

**方案**：使用 Small Buffer Optimization（SBO）版本的 function wrapper：

```cpp
namespace kairo::detail {

// 固定大小的函数包装器（SBO，无堆分配用于小闭包）
template<typename Signature, size_t InlineSize = 32>
class InlineFunction;

template<typename R, typename... Args, size_t InlineSize>
class InlineFunction<R(Args...), InlineSize> {
public:
    InlineFunction() = default;
    
    template<typename F>
    InlineFunction(F&& f) {
        static_assert(sizeof(F) <= InlineSize, 
                      "Callable too large for inline storage");
        static_assert(std::is_trivially_destructible_v<F>,
                      "Only trivial-destructor callables supported");
        new (storage_.data()) std::decay_t<F>(std::forward<F>(f));
        invoker_ = [](void* storage, Args... args) -> R {
            auto& func = *static_cast<std::decay_t<F>*>(storage);
            return func(std::forward<Args>(args)...);
        };
    }
    
    R operator()(Args... args) const {
        return invoker_(const_cast<void*>(
            static_cast<const void*>(storage_.data())), 
            std::forward<Args>(args)...);
    }
    
    explicit operator bool() const noexcept {
        return invoker_ != nullptr;
    }

private:
    alignas(std::max_align_t) std::array<std::byte, InlineSize> storage_;
    R (*invoker_)(void*, Args...) = nullptr;
};

} // namespace kairo::detail

// 在 Task 中使用
struct Task {
    // 替换 std::function<void()> function;
    detail::InlineFunction<void(), 32> function;  // 32 字节内联存储
    // ... 其他字段
};
```

**限制**：
- 只支持 trivially-destructible 的闭包（无 unique_ptr/shared_ptr 捕获）
- 闭包大小 <= 32 bytes（覆盖绝大多数简单 lambda）
- 超过限制的回退到 `std::function`（保持兼容）

**预期收益**：
- 简单 lambda（无捕获或捕获 POD）：**-1 alloc/task**
- 覆盖率预期：70-80% 的用户任务
- 对于复杂闭包：零影响（回退到 std::function）

#### 2.1.3 DrainBag 引用计数优化

**问题**：M0 分析提到 DrainBag × 2 次堆分配（引用计数块）。

**方案**：使用侵入式引用计数（intrusive_ptr），将计数嵌入对象：

```cpp
namespace kairo::detail {

// 侵入式引用计数的 DrainBag
class DrainBag {
public:
    DrainBag() : ref_count_(1) {}
    
    void add_ref() {
        ref_count_.fetch_add(1, std::memory_order_relaxed);
    }
    
    void release() {
        if (ref_count_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete this;
        }
    }
    
    // ... 其他方法

private:
    std::atomic<size_t> ref_count_;
    // ... 数据成员
};

// 侵入式智能指针（零额外分配）
class DrainBagPtr {
public:
    explicit DrainBagPtr(DrainBag* p = nullptr) : ptr_(p) {}
    
    DrainBagPtr(const DrainBagPtr& other) : ptr_(other.ptr_) {
        if (ptr_) ptr_->add_ref();
    }
    
    DrainBagPtr& operator=(const DrainBagPtr& other) {
        if (ptr_ != other.ptr_) {
            if (ptr_) ptr_->release();
            ptr_ = other.ptr_;
            if (ptr_) ptr_->add_ref();
        }
        return *this;
    }
    
    ~DrainBagPtr() {
        if (ptr_) ptr_->release();
    }
    
    DrainBag* get() const { return ptr_; }
    DrainBag* operator->() const { return ptr_; }

private:
    DrainBag* ptr_;
};

} // namespace kairo::detail
```

**预期收益**：
- **-1 alloc/task**（DrainBag 引用计数块从 2 次降到 1 次：只分配对象本身）
- 性能影响：引用计数操作与 shared_ptr 相同（原子操作）

**验收标准（阶段一总计）**：
- [ ] `benchmark_submit_allocations`：9 → **7 allocs/task**（-22%）
- [ ] 字节数：768 → ~600 bytes/task（-22%）
- [ ] 配对 A/B 吞吐基准：不弱于 0.7.0（允许 ±5% 噪声）
- [ ] 全量测试通过（包括 ASAN/TSAN）
- [ ] Task 池命中率 > 95%（稳态，通过计数器监控）

---

### 阶段二：Promise/Future 优化（v0.7.3，目标 7 → 5-6 allocs/task）

#### 2.2.1 自定义轻量 Future 实现

**问题**：`std::promise/future` 实现通常有 3-4 次堆分配（共享状态 + 控制块）。

**方案**：实现轻量的 `kairo::Task<T>` 作为返回值类型（类似 folly::SemiFuture）：

```cpp
namespace kairo {

// 轻量任务句柄（单次分配的 promise/future 替代）
template<typename T>
class TaskResult {
public:
    // 内部共享状态（单次分配）
    struct SharedState {
        std::mutex mutex;
        std::condition_variable cv;
        std::optional<T> value;        // 或 std::variant<T, std::exception_ptr>
        std::exception_ptr exception;
        std::atomic<bool> ready{false};
        
        // 内联引用计数（避免额外分配）
        std::atomic<size_t> ref_count{1};
    };
    
    // Promise 侧（内部使用）
    class Promise {
    public:
        explicit Promise(SharedState* state) : state_(state) {}
        
        void set_value(T&& value) {
            std::unique_lock lock(state_->mutex);
            state_->value = std::move(value);
            state_->ready.store(true, std::memory_order_release);
            state_->cv.notify_all();
        }
        
        void set_exception(std::exception_ptr ex) {
            std::unique_lock lock(state_->mutex);
            state_->exception = ex;
            state_->ready.store(true, std::memory_order_release);
            state_->cv.notify_all();
        }

    private:
        SharedState* state_;
    };
    
    // Future 侧（用户可见）
    TaskResult() : state_(new SharedState{}) {}
    
    T get() {
        std::unique_lock lock(state_->mutex);
        state_->cv.wait(lock, [this] {
            return state_->ready.load(std::memory_order_acquire);
        });
        
        if (state_->exception) {
            std::rethrow_exception(state_->exception);
        }
        return std::move(*state_->value);
    }
    
    bool is_ready() const {
        return state_->ready.load(std::memory_order_acquire);
    }

private:
    SharedState* state_;  // 侵入式引用计数
};

} // namespace kairo
```

**接口迁移策略**：
- 新 API：`TaskResult<T> submit_v2(F&& f)`（opt-in，逐步迁移）
- 旧 API：`std::future<T> submit(F&& f)`（保持向后兼容，内部适配）
- 0.8.0 前标记 `submit()` 为 deprecated，0.9.0 移除

**预期收益**：
- **-2 allocs/task**（共享状态从 3-4 次降到 1 次）
- 内存占用：~200 bytes（共享状态）vs ~300 bytes（std::future 控制块）
- 性能：get() 路径与 std::future 相当（都是 mutex + cv）

#### 2.2.2 无等待快速路径优化

**问题**：大多数任务在 `get()` 前就已完成，无需等待。

**方案**：添加快速路径（spin-wait + atomic）：

```cpp
template<typename T>
T TaskResult<T>::get() {
    // 快速路径：任务已完成，无需加锁
    if (state_->ready.load(std::memory_order_acquire)) {
        if (state_->exception) {
            std::rethrow_exception(state_->exception);
        }
        return std::move(*state_->value);
    }
    
    // 短暂 spin（通常几微秒内就完成）
    for (int spin = 0; spin < 100; ++spin) {
        if (state_->ready.load(std::memory_order_acquire)) {
            if (state_->exception) {
                std::rethrow_exception(state_->exception);
            }
            return std::move(*state_->value);
        }
        std::this_thread::yield();
    }
    
    // 慢速路径：真正等待
    std::unique_lock lock(state_->mutex);
    state_->cv.wait(lock, [this] {
        return state_->ready.load(std::memory_order_acquire);
    });
    
    if (state_->exception) {
        std::rethrow_exception(state_->exception);
    }
    return std::move(*state_->value);
}
```

**预期收益**：
- 快速路径命中（任务已完成）：~10-20ns（原子读 + 移动）
- 慢速路径（真正等待）：与 std::future 相当
- 命中率预期：> 60%（取决于任务粒度和提交模式）

**验收标准（阶段二总计）**：
- [ ] `benchmark_submit_allocations`：7 → **5-6 allocs/task**（-17-29%）
- [ ] 字节数：~600 → ~400-500 bytes/task
- [ ] `TaskResult::get()` 快速路径命中率 > 60%（通过计数器）
- [ ] 配对 A/B 吞吐基准：不弱于阶段一（±5%）
- [ ] 全量测试 + 向后兼容性验证

---

### 阶段三：可选的零分配模式（v0.8.0，实验性）

#### 2.3.1 基于协程的零分配 submit

**前提**：C++20 协程支持（可选特性）。

**方案**：利用协程的栈上状态，避免堆分配：

```cpp
namespace kairo {

// 协程任务（零额外堆分配）
template<typename T = void>
struct CoroTask {
    struct promise_type {
        CoroTask get_return_object() {
            return CoroTask{
                std::coroutine_handle<promise_type>::from_promise(*this)
            };
        }
        
        std::suspend_never initial_suspend() { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        
        void return_value(T value) {
            result_ = std::move(value);
        }
        
        void unhandled_exception() {
            exception_ = std::current_exception();
        }
        
        T result_;
        std::exception_ptr exception_;
    };
    
    std::coroutine_handle<promise_type> handle_;
    
    T get() {
        if (handle_.promise().exception_) {
            std::rethrow_exception(handle_.promise().exception_);
        }
        return std::move(handle_.promise().result_);
    }
};

// 协程版本的 submit（零额外堆分配）
template<typename F>
auto submit_coro(F&& f) -> CoroTask<typename std::invoke_result<F>::type> {
    // 直接在协程帧中执行（单次分配：协程帧本身）
    co_return f();
}

} // namespace kairo
```

**限制**：
- 需要 C++20 编译器支持
- 协程帧本身仍有一次堆分配（但比 promise/future 轻量）
- 仅适用于 CPU 任务（不支持跨线程调度）

**预期收益**：
- **-2 allocs/task**（协程帧替代 promise/future/task 结构）
- 适用场景：高频短任务、对延迟敏感的路径
- 性能：与 `submit_fast()` 相当（~35-40ns）

**验收标准（阶段三）**：
- [ ] `benchmark_submit_allocations`：**3-4 allocs/task**（使用 submit_coro）
- [ ] 协程版本作为 opt-in 特性（`KAIRO_ENABLE_CORO_TASK`）
- [ ] 性能不弱于 `submit_fast()`
- [ ] 文档说明使用场景和限制

---

## 3. 实施路线图

### 3.1 时间线

| 阶段 | 工作项 | 预计时间 | 里程碑 |
|------|--------|---------|--------|
| 阶段一 Phase 1 | Task 对象池设计与实现 | 3-5 天 | 单元测试通过 |
| 阶段一 Phase 2 | InlineFunction SBO 实现 | 3-4 天 | 覆盖率测试 |
| 阶段一 Phase 3 | DrainBag 侵入式引用计数 | 2-3 天 | ASAN/TSAN 验证 |
| 阶段一 总结 | 集成测试与基准验证 | 2-3 天 | **9 → 7 allocs** |
| 阶段二 Phase 1 | TaskResult 轻量 Future | 5-7 天 | API 设计审查 |
| 阶段二 Phase 2 | 快速路径优化 | 2-3 天 | 性能基准通过 |
| 阶段二 Phase 3 | 向后兼容适配层 | 3-5 天 | 迁移指南完成 |
| 阶段二 总结 | 长时间 soak 测试 | 24-48 小时 | **7 → 5-6 allocs** |
| 阶段三 | 协程版本（实验性） | 5-7 天 | 可选特性 |
| 文档与发布 | 文档同步、性能报告 | 3-5 天 | v0.7.2 发布 |

**总计**：
- 阶段一：约 2 周（v0.7.2）
- 阶段二：约 3 周（v0.7.3）
- 阶段三：约 1-2 周（v0.8.0，可选）

### 3.2 版本分配

- **v0.7.2**：阶段一（9 → 7 allocs/task）
- **v0.7.3**：阶段二（7 → 5-6 allocs/task）
- **v0.8.0**：阶段三（3-4 allocs/task，协程版本）

## 4. 风险与缓解

| 风险 | 影响 | 缓解措施 |
|------|------|---------|
| Task 对象池未正确重置状态导致泄漏 | 内存泄漏、UAF | 严格的重置协议；ASAN 持续验证；单元测试覆盖池行为 |
| InlineFunction 与 std::function 行为差异 | 兼容性问题 | 限制在 trivial-destructor 闭包；回退机制；充分测试 |
| 自定义 TaskResult 破坏 std::future 语义 | 破坏性变更 | 新 API（submit_v2）；旧 API 保持兼容；逐步迁移策略 |
| 性能优化引入回归 | 吞吐下降 | 每阶段配对 A/B 基准；验收线：不弱于前一阶段 ±5% |
| 对象池在高并发下竞争 | 吞吐瓶颈 | Per-worker 池设计；lock-free 栈操作；监控命中率 |

## 5. 性能预期总结

| 指标 | v0.7.0 基线 | 阶段一 (v0.7.2) | 阶段二 (v0.7.3) | 阶段三 (v0.8.0) |
|------|------------|----------------|----------------|----------------|
| allocs/task | 9 | **7** (-22%) | **5-6** (-33-44%) | **3-4** (-56-67%) |
| bytes/task | 768 | ~600 (-22%) | ~400-500 (-34-48%) | ~200-300 (-61-74%) |
| 吞吐回归容忍 | - | ±5% | ±5% | ±5% |

**关键约束**：每阶段必须保证不弱于前一阶段性能（配对 A/B 基准）。

## 6. 可观测性

### 6.1 新增指标

```cpp
struct AllocationMetrics {
    std::atomic<uint64_t> task_pool_hit_count{0};      // Task 池命中次数
    std::atomic<uint64_t> task_pool_miss_count{0};     // Task 池未命中次数
    std::atomic<uint64_t> inline_function_count{0};    // InlineFunction 使用次数
    std::atomic<uint64_t> std_function_fallback{0};    // std::function 回退次数
    std::atomic<uint64_t> fast_path_get_count{0};      // TaskResult 快速路径命中
    std::atomic<uint64_t> slow_path_get_count{0};      // TaskResult 慢速路径
};
```

### 6.2 诊断 API

```cpp
class Executor {
public:
    /**
     * @brief 获取堆分配优化统计。
     */
    AllocationMetrics get_allocation_metrics() const;
    
    /**
     * @brief Task 池命中率（0.0-1.0）。
     */
    double get_task_pool_hit_rate() const {
        auto metrics = get_allocation_metrics();
        auto total = metrics.task_pool_hit_count + metrics.task_pool_miss_count;
        if (total == 0) return 0.0;
        return static_cast<double>(metrics.task_pool_hit_count) / total;
    }
};
```

## 7. 文档更新

### 7.1 用户文档

- **API 参考**（`docs/API.md`）：
  - §3.1 新增 `submit_v2()` / `TaskResult<T>` 说明
  - §3.7 补充堆分配优化最佳实践
  
- **性能指南**（新增 `website/{zh,en}/guides/allocation-optimization.md`）：
  - 何时使用对象池
  - InlineFunction 的使用限制
  - TaskResult vs std::future 选择指南

### 7.2 迁移指南

`MIGRATION.md` 新增 0.7.2/0.7.3 节：

```markdown
## v0.7.2 - 堆分配优化（阶段一）

### 无破坏性变更

- Task 对象池：自动生效，用户无感知
- InlineFunction：内部实现，对外透明
- DrainBag：内部优化，API 不变

### 性能改进

- 堆分配：9 → 7 allocs/task (-22%)
- 内存占用：768 → ~600 bytes/task (-22%)

### 可观测性

新增 `Executor::get_allocation_metrics()` 和 `get_task_pool_hit_rate()`。

## v0.7.3 - 堆分配优化（阶段二）

### 新增 API（opt-in）

- `TaskResult<T> submit_v2(F&& f)`：轻量 Future，5-6 allocs/task
- 旧 `std::future<T> submit(F&& f)`：保持兼容，内部适配

### 迁移建议

对于高频短任务，考虑迁移到 `submit_v2()`：
```cpp
// 旧
auto future = ex.submit([] { return 42; });
int result = future.get();

// 新
auto task_result = ex.submit_v2([] { return 42; });
int result = task_result.get();
```

### 性能改进

- 堆分配：7 → 5-6 allocs/task (-17-29%)
- 快速路径 get()：~10-20ns（任务已完成）
```

---

## 8. 总结

### 8.1 核心价值

1. **显著降低堆分配**：9 → 5-6 allocs/task（-33-44%），最终可达 3-4 (-56-67%)
2. **保持性能不回退**：每阶段验收线 ±5%，严格配对 A/B 基准
3. **渐进式优化**：分三阶段实施，每阶段独立验证，风险可控
4. **向后兼容**：旧 API 保持不变，新优化对用户透明或 opt-in

### 8.2 与 v0.7.0 原则的一致性

- ✅ **性能优先于灵活性**：针对热路径（submit）的专项优化
- ✅ **不弱于旧基线**：每阶段严格验证，不允许回归
- ✅ **渐进式采用**：Task 池自动生效 → TaskResult opt-in → 协程实验性
- ✅ **可观测的改进**：新增指标和诊断 API，优化效果可量化

### 8.3 实施建议

1. **优先级**：阶段一 > 阶段二 > 阶段三（收益递减，风险递增）
2. **验证标准**：每阶段必须通过 `benchmark_submit_allocations` 和配对 A/B 基准
3. **回退策略**：如果任一阶段未达验收线，回退并分析根因后重试
4. **社区反馈**：阶段一完成后发布 beta，收集实际应用中的反馈

---

**变更历史**：
- 2026-10-13：初始方案（基于 v0.7.0 M0 分析）
