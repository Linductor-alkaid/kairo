# submit_auto 路由智能化增强方案

> **提议人**：设计评审（2026-10-13）  
> **目标版本**：v0.7.1  
> **状态**：提议阶段，待实施  

## 1. 背景与问题

### 1.1 当前状态

`submit_auto` 是 Kairo 统一提交 API 的核心，旨在为用户提供"自动"路由能力。但实际使用中存在以下问题：

1. **路由能力未充分体现**：
   - 默认 `DefaultScheduler` 只做确定性的意图路由
   - 0.7.0 M3 引入的 `AdaptiveScheduler` 虽然提供了 CPU/GPU 自适应选择，但仍需手动注入
   - 用户仍需显式指定 `LowLatency`/`RealtimeQueue` 的 `preferred_executor`

2. **性能开销较大**：
   - 相对 `submit` 慢 65-70%（CR-106 已优化 CPU-only 纯策略路径）
   - 每次需要采集执行器能力快照（锁定 5 把注册表）
   - 添加测量包装（当 `wants_feedback()` 时）增加额外开销

3. **"auto"名不副实**：
   - 对于复杂场景，用户仍需深入理解内部执行器架构
   - 缺少基于任务特征的自动推断能力
   - 缺少快速路径优化（常见简单场景仍走完整路由）

### 1.2 用户期望

用户希望 `submit_auto` 能够：
- 真正"自动"地选择合适的执行路径
- 性能最优，不牺牲太多性能来换取灵活性
- 路由决策透明可解释
- 保持向后兼容，不引入破坏性变化

## 2. 设计目标

### 2.1 核心原则

延续 Kairo 0.7.x 系列的既定原则：

1. **自适应是 opt-in**：默认行为保持 0.6.1 语义，新能力通过配置开启
2. **每个决策可解释**：自动决策记录到 `RoutingDecision` 和 `SchedulingMetrics`
3. **热路径预算先定、后实现**：每项优化都有明确的性能验收线
4. **不引入破坏性变更**：所有增强都是 additive

### 2.2 性能目标

| 路径 | 当前（0.7.0） | 目标（0.7.1） | 改进 |
|------|--------------|--------------|------|
| CPU-only 纯策略 | ~50ns | ~35ns | 30% |
| CPU/GPU 选择（缓存命中） | ~120ns | ~60ns | 50% |
| 自适应路由 | ~150ns | ~80ns | 47% |

## 3. 三阶段实施方案

### 阶段一：能力快照缓存与快速路径（短期，1-2 周）

#### 3.1.1 能力快照 TTL 缓存

**问题**：当前每次 `submit_auto` 都锁定并采集执行器能力，成为主要性能瓶颈。

**方案**：引入基于 TTL 的能力快照缓存：

```cpp
// 在 Executor 类中添加
class Executor {
private:
    // 能力缓存（RCU 风格：写时复制，读无锁）
    std::shared_ptr<const std::vector<ExecutorCapability>> cached_capabilities_;
    std::atomic<int64_t> capabilities_cache_timestamp_{0};
    mutable std::mutex capabilities_cache_mutex_;
    
    static constexpr int64_t CACHE_TTL_NS = 100'000'000; // 100ms
    
    // 获取能力快照（优先从缓存）
    std::vector<ExecutorCapability> get_executor_capabilities_cached() const {
        const int64_t now = steady_clock_ns();
        
        // 快速路径：缓存有效
        auto cached = std::atomic_load_explicit(
            &cached_capabilities_, std::memory_order_acquire);
        const int64_t ts = capabilities_cache_timestamp_.load(
            std::memory_order_acquire);
        
        if (cached && (now - ts) < CACHE_TTL_NS) {
            // 缓存命中
            scheduling_counters_.capability_cache_hit.fetch_add(
                1, std::memory_order_relaxed);
            return *cached;
        }
        
        // 慢速路径：缓存过期或不存在，需要刷新
        return refresh_capabilities_cache();
    }
    
    std::vector<ExecutorCapability> refresh_capabilities_cache() const {
        std::lock_guard<std::mutex> lock(capabilities_cache_mutex_);
        
        // 双重检查：可能其他线程已刷新
        const int64_t now = steady_clock_ns();
        const int64_t ts = capabilities_cache_timestamp_.load(
            std::memory_order_relaxed);
        auto cached = std::atomic_load_explicit(
            &cached_capabilities_, std::memory_order_relaxed);
        
        if (cached && (now - ts) < CACHE_TTL_NS) {
            return *cached;
        }
        
        // 真正采集能力
        auto fresh = manager_->get_executor_capabilities();
        auto fresh_shared = std::make_shared<
            const std::vector<ExecutorCapability>>(std::move(fresh));
        
        std::atomic_store_explicit(&cached_capabilities_, fresh_shared,
                                   std::memory_order_release);
        capabilities_cache_timestamp_.store(now, std::memory_order_release);
        
        scheduling_counters_.capability_cache_miss.fetch_add(
            1, std::memory_order_relaxed);
        
        return *fresh_shared;
    }
};
```

**缓存失效策略**：
- TTL 超时（默认 100ms）
- 后端状态变化（stop/start/register）主动失效
- `shutdown()` 时清空缓存

#### 3.1.2 按后端类型分片缓存（可选优化）

对于更细粒度的优化，可以按后端类型分开缓存：

```cpp
struct CapabilityCache {
    std::atomic<ExecutorCapability*> default_cpu{nullptr};
    std::atomic<ExecutorCapability*> gpu{nullptr};
    std::atomic<ExecutorCapability*> lockfree{nullptr};
    std::atomic<ExecutorCapability*> realtime{nullptr};
    std::atomic<int64_t> timestamp{0};
};
```

但这会增加代码复杂度，建议先实现全局缓存，性能不足时再考虑分片。

#### 3.1.3 零路由开销快速路径

为最常见的 CPU-only 无约束场景提供 `submit_fast()`：

```cpp
/**
 * @brief 零路由开销的快速提交路径。
 * 
 * 适用场景：
 * - 确定是 CPU-only 任务（不涉及 GPU）
 * - 无特殊约束（deadline/affinity/resources）
 * - 默认 QoS（Standard）和优先级（NORMAL）可接受
 * 
 * 跳过路由决策，直接提交到默认线程池。性能约 35ns，
 * 相比 submit_auto 纯策略路径（50ns）有 30% 改进。
 * 
 * 不推荐用于：
 * - 需要自适应调度的场景
 * - 需要诊断路由决策的场景
 * - 不确定任务特性的场景
 */
template<typename F, typename... Args>
auto submit_fast(F&& f, Args&&... args)
    -> std::future<typename std::invoke_result<F, Args...>::type> {
    TaskSchedulingMeta meta;
    meta.qos = QosClass::Standard;
    return submit_priority_scheduled(
        static_cast<int>(TaskPriority::NORMAL),
        std::move(meta),
        std::forward<F>(f),
        std::forward<Args>(args)...
    );
}
```

**使用指南**（文档）：
- ✅ 推荐：循环中的大量简单 CPU 计算任务
- ✅ 推荐：已确定无需路由决策的热路径
- ❌ 不推荐：混合 CPU/GPU 的任务
- ❌ 不推荐：需要 QoS 保证或自适应调度的任务

#### 3.1.4 验收标准

- [ ] `get_executor_capabilities_cached()` 缓存命中时无锁，~10ns 开销
- [ ] CPU/GPU 选择路径从 ~120ns 降至 ~60ns（50% 改进）
- [ ] `submit_fast()` 端到端 ~35ns（相比 submit_auto 纯策略 ~50ns 有 30% 改进）
- [ ] 配对 A/B 基准（10 轮，方向一致性 ≥ 80%）
- [ ] TSAN 全绿，无数据竞争
- [ ] 缓存过期/后端状态变化的契约测试

---

### 阶段二：任务特征自动推断（中期，1-2 月）

#### 3.2.1 任务特征分析器

引入 `TaskCharacteristics` 结构，基于任务名称、历史统计等信息推断任务特征：

```cpp
namespace kairo {

struct TaskCharacteristics {
    bool is_blocking_io = false;      // 可能包含阻塞 I/O
    bool needs_low_latency = false;   // 延迟敏感
    bool is_periodic = false;         // 周期性调用
    size_t estimated_duration_us = 0; // 预估执行时长（基于历史）
    
    // 推断信心度（0.0-1.0）
    double confidence = 0.0;
    
    // 推断依据（用于诊断）
    std::string inference_reason;
};

class TaskCharacteristicsAnalyzer {
public:
    /**
     * @brief 分析任务特征。
     * 
     * 推断策略：
     * 1. 任务名称模式匹配（启发式）
     * 2. 历史执行统计（需要 min_history_samples）
     * 3. 提交时间间隔分析（周期性检测）
     */
    TaskCharacteristics analyze(
        const std::string& task_name,
        const std::optional<std::string>& task_type_hint = std::nullopt) const;
    
    /**
     * @brief 记录任务执行结果，用于历史统计。
     */
    void record_execution(
        const std::string& task_name,
        int64_t queue_wait_ns,
        int64_t execution_duration_ns,
        bool success);
    
    /**
     * @brief 配置项。
     */
    struct Config {
        size_t min_history_samples = 32;  // 历史样本最少数量
        bool enable_name_patterns = true;  // 启用名称模式匹配
        bool enable_periodic_detection = true;  // 启用周期性检测
    };
    
    void configure(const Config& config);

private:
    // 历史统计（按任务名分键）
    struct TaskHistory {
        size_t sample_count = 0;
        int64_t avg_queue_wait_ns = 0;
        int64_t avg_execution_ns = 0;
        double execution_stddev = 0;
        
        // 周期性检测
        std::deque<int64_t> submit_timestamps;  // 最近 N 次提交时间
    };
    
    mutable std::mutex history_mutex_;
    std::unordered_map<std::string, TaskHistory> task_history_;
    
    Config config_;
};

} // namespace kairo
```

#### 3.2.2 推断策略

**1. 阻塞 I/O 检测**：

```cpp
bool TaskCharacteristicsAnalyzer::is_likely_blocking_io(
    const std::string& task_name,
    const TaskHistory* history) const {
    
    // 启发式：名称模式
    if (config_.enable_name_patterns) {
        static const std::vector<std::string> io_patterns = {
            "_io_", "_read_", "_write_", "_socket_", "_file_",
            "_network_", "_disk_", "_blocking_"
        };
        for (const auto& pattern : io_patterns) {
            if (task_name.find(pattern) != std::string::npos) {
                return true;  // 信心度：中等
            }
        }
    }
    
    // 历史统计：执行时长长但 CPU 占用率低
    if (history && history->sample_count >= config_.min_history_samples) {
        // 执行时长 > 10ms 且变异系数大（说明等待外部 I/O）
        if (history->avg_execution_ns > 10'000'000 &&
            history->execution_stddev / history->avg_execution_ns > 0.5) {
            return true;  // 信心度：高
        }
    }
    
    return false;
}
```

**2. 低延迟需求检测**：

```cpp
bool TaskCharacteristicsAnalyzer::needs_low_latency(
    const std::string& task_name,
    const TaskHistory* history) const {
    
    // 启发式：名称模式
    if (config_.enable_name_patterns) {
        static const std::vector<std::string> latency_patterns = {
            "realtime", "control", "critical", "low_latency", "urgent"
        };
        for (const auto& pattern : latency_patterns) {
            if (task_name.find(pattern) != std::string::npos) {
                return true;
            }
        }
    }
    
    // 历史统计：queue_wait 敏感（执行时长短但等待长会影响结果）
    if (history && history->sample_count >= config_.min_history_samples) {
        // 执行时长 < 100us 且 queue wait 经常超过执行时长
        if (history->avg_execution_ns < 100'000 &&
            history->avg_queue_wait_ns > history->avg_execution_ns * 2) {
            return true;
        }
    }
    
    return false;
}
```

**3. 周期性检测**：

```cpp
bool TaskCharacteristicsAnalyzer::is_periodic(
    const std::string& task_name,
    const TaskHistory* history) const {
    
    if (!config_.enable_periodic_detection || !history) {
        return false;
    }
    
    const auto& timestamps = history->submit_timestamps;
    if (timestamps.size() < 10) {
        return false;  // 样本不足
    }
    
    // 计算提交间隔的标准差
    std::vector<int64_t> intervals;
    for (size_t i = 1; i < timestamps.size(); ++i) {
        intervals.push_back(timestamps[i] - timestamps[i-1]);
    }
    
    const double mean = std::accumulate(intervals.begin(), intervals.end(), 0.0) 
                        / intervals.size();
    double variance = 0.0;
    for (auto interval : intervals) {
        variance += (interval - mean) * (interval - mean);
    }
    const double stddev = std::sqrt(variance / intervals.size());
    
    // 变异系数 < 10% 认为是周期性
    return (stddev / mean) < 0.1;
}
```

#### 3.2.3 集成到 submit_auto

```cpp
template<typename F, typename... Args>
auto Executor::submit_auto(F&& f, Args&&... args)
    -> std::future<typename std::invoke_result<F, Args...>::type> {
    
    TaskOptions options;
    
    // 如果用户未显式设置 intent，尝试自动推断
    if (!options.intent_explicitly_set && 
        config_.enable_auto_inference) {
        
        auto chars = task_characteristics_analyzer_.analyze(options.name);
        
        if (chars.confidence > 0.6) {  // 信心度阈值
            if (chars.is_blocking_io) {
                // 建议使用 BlockingWorker（记录诊断，不自动切换）
                record_auto_intent_suggestion(
                    options.name, "blocking_io_detected", chars.confidence);
                // 用户应该使用 start_worker() 而不是 submit_auto
                
            } else if (chars.needs_low_latency && 
                       chars.estimated_duration_us < 100) {
                // 自动选择 LockFree（如果有可用的）
                if (auto executor = select_best_lockfree_executor()) {
                    options.intent = ExecutionIntent::LowLatency;
                    options.preferred_executor = *executor;
                    options.diagnostics |= RoutingDiagnostics::AutoInferredIntent;
                    record_adaptive_decision("auto_selected_lockfree", 
                                           chars.confidence);
                }
                
            } else if (chars.is_periodic) {
                // 周期性任务建议（记录诊断，不自动切换）
                record_auto_intent_suggestion(
                    options.name, "periodic_pattern", chars.confidence);
                // 用户应该考虑使用 submit_periodic() 或 RealtimeQueue
            }
        }
    }
    
    // 继续原有的路由流程
    return submit_auto_impl(std::forward<F>(f), std::forward<Args>(args)...);
}
```

#### 3.2.4 边界与限制

**严格限制**：
1. **仅在用户未显式设置 intent 时生效**：如果 `TaskOptions::intent` 已设置，跳过推断
2. **需要显式开启**：通过 `ExecutorConfig::enable_auto_inference = true` 启用（默认 false）
3. **建议优先于自动切换**：对于语义差异大的路径（如 BlockingWorker、RealtimeQueue），只记录建议，不自动切换
4. **信心度阈值**：只有信心度 > 0.6 的推断才会生效

**可配置项**：
```cpp
struct ExecutorConfig {
    // ... 现有字段
    
    // 0.7.1 新增：任务特征自动推断
    bool enable_auto_inference = false;  // 默认关闭
    double auto_inference_confidence_threshold = 0.6;  // 信心度阈值
    size_t min_history_samples = 32;  // 最少历史样本数
};
```

#### 3.2.5 验收标准

- [ ] 合成负载矩阵测试：
  - 阻塞 I/O 任务检测准确率 > 85%
  - 低延迟任务检测准确率 > 80%
  - 周期性任务检测准确率 > 90%
- [ ] 误判率 < 5%（不应该推断的场景）
- [ ] 冷启动期间（< min_history_samples）不产生误导性推断
- [ ] 推断结果记录到 `RoutingDecision` 可复原
- [ ] 文档清晰说明推断策略和边界

---

### 阶段三：AdaptiveScheduler 扩展（与阶段二并行）

#### 3.3.1 执行器选择优化

扩展 0.7.0 M3 的 `AdaptiveScheduler`，增加执行器自动选择能力：

```cpp
class AdaptiveScheduler : public IScheduler {
public:
    // ... 现有接口
    
    /**
     * @brief 为给定意图选择最优执行器。
     * 
     * 当有多个候选执行器时（如多个 lockfree executor），
     * 基于历史负载自动选择负载最轻的一个。
     */
    std::optional<std::string> select_best_executor(
        ExecutionIntent intent,
        const std::vector<ExecutorCapability>& capabilities) const;

private:
    // 执行器负载统计（按执行器名分键）
    struct ExecutorLoadStats {
        size_t recent_task_count = 0;
        int64_t avg_queue_wait_ns = 0;
        int64_t p99_queue_wait_ns = 0;
    };
    
    mutable std::shared_ptr<const std::unordered_map<
        std::string, ExecutorLoadStats>> executor_load_stats_;
};
```

**选择策略**：
1. 过滤出符合意图的候选执行器（如 `LowLatency` → 所有 lockfree executor）
2. 按 p99 queue_wait 排序，选择最低的
3. 如果样本数不足（< min_samples），按能力 `pending_work` 排序（静默降级）

#### 3.3.2 动态意图调整建议

当检测到任务长期不适合当前路径时，记录建议：

```cpp
void AdaptiveScheduler::maybe_suggest_intent_adjustment(
    const TaskRouter::Request& request,
    RoutingDecision& decision) const {
    
    auto snapshot = feedback_snapshot();
    if (!snapshot) return;
    
    // 查找该任务的历史统计
    const auto key = scheduling::FeedbackKey{
        decision.selected_backend,
        decision.selected_executor_name,
        request.options.qos
    };
    
    auto stats = snapshot->get_stats(key);
    if (!stats || stats->attempts < config_.min_samples) {
        return;
    }
    
    // 例如：CPU 任务但 queue_wait 一直很低，适合 lockfree
    if (decision.selected_backend == ExecutionBackend::DefaultAsync &&
        stats->queue_wait_p99_ns < 1'000'000) {  // < 1ms
        
        decision.diagnostics |= RoutingDiagnostics::IntentAdjustmentSuggested;
        decision.detail += "; suggestion: consider LowLatency intent";
        
        scheduling_counters_.intent_adjustment_suggested.fetch_add(
            1, std::memory_order_relaxed);
    }
}
```

**原则**：建议优先于自动切换，不会静默改变语义。

#### 3.3.3 验收标准

- [ ] 多 lockfree executor 场景下，负载不均时自动选择将整体 p99 queue_wait 降低 > 20%
- [ ] 冷启动期间（< min_samples）误选率 < 5%
- [ ] 意图调整建议的准确率 > 75%（通过人工标注的测试集验证）
- [ ] 热路径开销：能力缓存命中时 route 路径 < 80ns

---

## 4. 可观测性与诊断

### 4.1 新增诊断位

扩展 `RoutingDiagnostics` 枚举：

```cpp
enum class RoutingDiagnostics : uint32_t {
    // ... 现有诊断位
    
    // 0.7.1 新增
    AutoInferredIntent = 0x20,           // 意图由自动推断产生
    AutoExecutorSelection = 0x40,        // 执行器由历史负载自动选择
    CapabilityCacheHit = 0x80,          // 使用了缓存的能力快照
    IntentAdjustmentSuggested = 0x100,  // 建议调整意图
};
```

### 4.2 新增指标

扩展 `SchedulingMetrics` 结构：

```cpp
struct SchedulingMetrics {
    // ... 现有字段
    
    // 0.7.1 新增
    std::atomic<uint64_t> auto_inferred_count{0};
    std::atomic<uint64_t> auto_executor_selected_count{0};
    std::atomic<uint64_t> capability_cache_hit_count{0};
    std::atomic<uint64_t> capability_cache_miss_count{0};
    std::atomic<uint64_t> auto_inference_rejected_count{0};
    std::atomic<uint64_t> intent_adjustment_suggested_count{0};
};
```

### 4.3 RoutingDecision 增强

```cpp
struct RoutingDecision {
    // ... 现有字段
    
    // 0.7.1 新增
    bool capability_from_cache = false;
    std::string inferred_reason;  // 如 "blocking_io_pattern_match"
    double inference_confidence = 0.0;
    std::optional<ExecutionIntent> original_intent;  // 推断前的原始意图
};
```

### 4.4 诊断 API

```cpp
class Executor {
public:
    /**
     * @brief 获取任务特征分析器的历史统计。
     * 
     * 用于诊断自动推断的准确性。
     */
    std::unordered_map<std::string, TaskCharacteristics> 
    get_task_characteristics_history() const;
    
    /**
     * @brief 清空任务特征历史（用于测试或重置学习）。
     */
    void clear_task_characteristics_history();
    
    /**
     * @brief 获取能力缓存统计。
     */
    struct CapabilityCacheStats {
        size_t hit_count;
        size_t miss_count;
        double hit_rate;
        int64_t avg_refresh_interval_ns;
    };
    CapabilityCacheStats get_capability_cache_stats() const;
};
```

---

## 5. 测试与验证

### 5.1 单元测试

```cpp
// tests/test_submit_auto_enhancement.cpp

TEST(SubmitAutoEnhancement, CapabilityCache) {
    // 测试缓存命中/未命中逻辑
    // 测试 TTL 过期
    // 测试后端状态变化时的失效
}

TEST(SubmitAutoEnhancement, TaskCharacteristics) {
    // 测试阻塞 I/O 检测
    // 测试低延迟需求检测
    // 测试周期性检测
    // 测试信心度计算
}

TEST(SubmitAutoEnhancement, AutoInference) {
    // 测试自动推断的触发条件
    // 测试用户显式设置的优先级
    // 测试冷启动行为
    // 测试误判边界
}

TEST(SubmitAutoEnhancement, SubmitFast) {
    // 测试快速路径的正确性
    // 测试与 submit_auto 的等价性（无约束场景）
}
```

### 5.2 性能基准

```cpp
// tests/benchmark_submit_auto_enhancement.cpp

BENCHMARK(SubmitFast_vs_Submit);
BENCHMARK(SubmitFast_vs_SubmitAuto);
BENCHMARK(CapabilityCache_Hit);
BENCHMARK(CapabilityCache_Miss);
BENCHMARK(AutoInference_ColdStart);
BENCHMARK(AutoInference_HotPath);
```

**验收标准**：配对 A/B 基准，10 轮，方向一致性 ≥ 80%。

### 5.3 集成测试

```cpp
// tests/test_submit_auto_integration.cpp

TEST(SubmitAutoIntegration, MixedWorkload) {
    // CPU-only + CPU/GPU + blocking I/O 混合负载
    // 验证路由决策的正确性和性能
}

TEST(SubmitAutoIntegration, AdaptiveWithInference) {
    // AdaptiveScheduler + 自动推断 联合测试
    // 验证两者协同工作
}

TEST(SubmitAutoIntegration, LongRunSoak) {
    // 长时间 soak 测试（≥ 24h）
    // 监控误判率、震荡、内存泄漏
}
```

### 5.4 合成负载矩阵

| 场景 | 任务特征 | 预期推断 | 验收标准 |
|------|---------|---------|---------|
| CPU 密集计算 | 执行时长 1-10ms，名称无特殊模式 | 无推断（使用默认） | 100% |
| 阻塞 I/O 读取 | 名称含 `_read_`，执行时长 > 10ms | is_blocking_io = true | > 85% |
| 低延迟控制 | 名称含 `control`，执行 < 100us | needs_low_latency = true | > 80% |
| 周期性任务 | 每 10ms 提交一次，连续 100 次 | is_periodic = true | > 90% |
| 混合负载 | 上述四类随机混合 | 各类独立准确率 | > 75% |

---

## 6. 文档更新

### 6.1 用户文档

- **API 参考**（`docs/API.md`）：
  - §3.7 新增 `submit_fast()` 使用说明
  - §3.8 补充自动推断配置和诊断
  
- **使用指南**（`website/{zh,en}/guides/`）：
  - `choosing-submit-api.md`：更新决策树，增加"何时用 submit_fast"
  - `adaptive-scheduling.md`：补充自动推断章节，说明 opt-in 机制
  - 新增 `submit-auto-optimization.md`：性能优化最佳实践

### 6.2 设计文档

- **调度运行时**（`docs/design/scheduling_runtime.md`）：
  - 新增 §9 "0.7.1 submit_auto 增强"
  - 补充能力缓存设计语义
  - 记录自动推断策略和边界

### 6.3 Skill 文档

- **集成 Skill**（`docs/skill/kairo-integration/`）：
  - `references/scheduling.md`：同步新 API 和诊断位
  
- **维护 Skill**（`docs/skill/kairo-maintainer/`）：
  - 补充性能优化指南
  - 记录测试和验证流程

### 6.4 迁移指南

`MIGRATION.md` 新增 0.7.1 节：

```markdown
## v0.7.1

### 无破坏性变更

0.7.1 是纯 additive 版本，所有现有代码无需修改即可编译运行。

### 新增功能（opt-in）

1. **submit_fast()**：零路由开销的快速提交路径（见 API §3.7）
2. **能力快照缓存**：自动开启，性能改进约 50%（CPU/GPU 选择场景）
3. **任务特征自动推断**：通过 `ExecutorConfig::enable_auto_inference = true` 启用（默认关闭）
4. **AdaptiveScheduler 执行器自动选择**：注入 AdaptiveScheduler 后自动生效

### 配置项

- `ExecutorConfig::enable_auto_inference`：启用任务特征自动推断（默认 false）
- `ExecutorConfig::auto_inference_confidence_threshold`：信心度阈值（默认 0.6）
- `ExecutorConfig::min_history_samples`：最少历史样本数（默认 32）

### 新增诊断位

`RoutingDiagnostics` 新增：
- `AutoInferredIntent` (0x20)
- `AutoExecutorSelection` (0x40)
- `CapabilityCacheHit` (0x80)
- `IntentAdjustmentSuggested` (0x100)

### 性能改进

- CPU/GPU 选择（缓存命中）：~120ns → ~60ns（50% 改进）
- submit_fast()：~35ns（相比 submit_auto ~50ns 有 30% 改进）
- AdaptiveScheduler 路由：~150ns → ~80ns（47% 改进）
```

---

## 7. 实施计划

### 7.1 时间线

| 阶段 | 工作项 | 预计时间 | 里程碑 |
|------|--------|---------|--------|
| 阶段一 | 能力快照缓存 | 3-5 天 | 性能基准通过 |
| 阶段一 | submit_fast() | 2-3 天 | 单元测试通过 |
| 阶段一 | 文档与验收 | 2-3 天 | 阶段一完成 |
| 阶段二 | TaskCharacteristics | 5-7 天 | 推断策略实现 |
| 阶段二 | submit_auto 集成 | 3-5 天 | 集成测试通过 |
| 阶段二 | 合成负载验证 | 5-7 天 | 准确率达标 |
| 阶段三 | AdaptiveScheduler 扩展 | 5-7 天 | 与阶段二并行 |
| 阶段三 | 长时间 soak | 24-48 小时 | 稳定性验证 |
| 总结 | 文档同步与发布 | 3-5 天 | 0.7.1 发布 |

**总计**：约 4-6 周（阶段二和阶段三部分并行）

### 7.2 风险与缓解

| 风险 | 影响 | 缓解措施 |
|------|------|---------|
| 自动推断准确率不达标 | 用户体验下降 | 提高信心度阈值；只做建议不自动切换；提供关闭开关 |
| 能力缓存过时导致错误路由 | 功能正确性 | 保守的 TTL（100ms）；后端状态变化主动失效；文档说明 TOCTOU 窗口 |
| 性能目标未达成 | 优化效果不明显 | 先实施阶段一；如不达标，放弃阶段二/三或调整目标 |
| 代码复杂度上升 | 维护成本增加 | 严格的代码审查；完善的单元测试；清晰的文档 |

---

## 8. 总结

### 8.1 核心价值

1. **性能提升**：通过缓存和快速路径，常见场景性能提升 30-50%
2. **真正的"auto"**：基于任务特征和历史统计的智能路由，减少用户配置负担
3. **可解释性**：所有自动决策记录到诊断位和指标，便于排障和优化
4. **向后兼容**：纯 additive 设计，默认行为不变

### 8.2 与 Kairo 愿景的一致性

- **渐进式采用**：从 submit_fast（零学习成本）→ 自动推断（opt-in）→ 完全自适应（AdaptiveScheduler）
- **诚实的语义**：建议优先于自动切换，不隐藏路由决策
- **可观测的失败**：误判和建议记录到结构化指标，用户可监控和调整
- **统一的入口**：submit_auto 仍是推荐的主入口，只是更智能和高效

### 8.3 下一步

1. 社区反馈：将本提议发布到 GitHub Discussions，收集用户意见
2. 原型验证：先实现阶段一，验证性能目标的可达成性
3. 迭代实施：根据反馈调整阶段二/三的优先级和范围
4. 持续监控：通过遥测收集实际使用中的准确率和性能数据

---

**变更历史**：
- 2026-10-13：初始提议（设计评审）
