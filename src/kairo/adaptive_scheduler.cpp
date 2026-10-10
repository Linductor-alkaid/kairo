#include "kairo/adaptive_scheduler.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <string>
#include <utility>

namespace kairo {

namespace {

// QoS 类索引/位：QosClass 枚举序即优先级秩（BestEffort=0 < Standard <
// Interactive < Critical=3）。越界值按 Standard 处理（防御，宽容语义）。
constexpr size_t qos_index(QosClass qos) noexcept {
    const size_t index = static_cast<size_t>(qos);
    return index < AdaptiveScheduler::kQosClassCount ? index : 1;
}

constexpr uint32_t qos_bit(QosClass qos) noexcept {
    return uint32_t{1} << qos_index(qos);
}

// 聚合器槽内 executor 名容量截断（record 侧同样截断，查找端保持一致，
// 否则超长名永远查不到自己的样本）。
std::string truncate_executor_name(const std::string& name) {
    const size_t capacity =
        scheduling::FeedbackAggregator::kExecutorNameCapacity - 1;
    if (name.size() <= capacity) {
        return name;
    }
    return name.substr(0, capacity);
}

}  // namespace

// ============================================================================
// 派生状态机（pimpl）：每个新合并快照在互斥锁内重评估一次，结果以两个
// 原子掩码发布给提交线程。评估节奏由聚合器 merge_interval 门限约束。
// ============================================================================

struct AdaptiveScheduler::State {
    mutable std::mutex mutex;
    // 双层新快照门限：route() 热路径只付一次 acquire 原子读；与门限
    // 不一致（每个合并周期至多一次）才进互斥锁重评估。evaluated_
    // merge_count_fast 与 evaluated_merge_count 在持锁更新时同步。
    std::atomic<uint64_t> evaluated_merge_count_fast{0};
    uint64_t evaluated_merge_count = 0;

    // 连击计数与激活态只在 mutex 内读写；掩码是唯一的跨线程发布面。
    std::array<uint32_t, kQosClassCount> shed_streak{};
    std::array<uint32_t, kQosClassCount> promotion_streak{};
    std::array<uint32_t, kQosClassCount> shed_recover_streak{};
    std::array<uint32_t, kQosClassCount> promotion_recover_streak{};
    std::array<bool, kQosClassCount> shed_active{};
    std::array<bool, kQosClassCount> promoted{};
    // 上一评估时各类的累计 queue-wait 桶计数（差分出窗口增量；聚合器
    // 直方图自构造起累计，直接评估会让"证据不足→fail-open"在预热后
    // 不可达，且陈旧 p99 无遗忘、流量停止后闸门无法解除）。
    std::array<std::vector<uint64_t>, kQosClassCount> last_cumulative_counts{};
    std::atomic<uint32_t> shed_mask{0};
    std::atomic<uint32_t> promoted_mask{0};

    void evaluate(const AdaptiveSchedulerConfig& config,
                  const scheduling::FeedbackSnapshot& snapshot) {
        for (size_t q = 0; q < kQosClassCount; ++q) {
            const int64_t target = config.queue_wait_p99_target_ns[q];

            // 按 QoS 类合并累计 queue-wait 直方图（同聚合器配置 → 桶界
            // 一致；桶数仍按各自规模取 min，防御性对齐）。
            scheduling::FeedbackHistogram cumulative;
            for (const scheduling::FeedbackEntry& entry : snapshot.entries) {
                if (qos_index(entry.key.qos) != q) {
                    continue;
                }
                if (cumulative.buckets.empty()) {
                    cumulative.buckets = entry.queue_wait.buckets;
                    continue;
                }
                const size_t common = std::min(cumulative.buckets.size(),
                                               entry.queue_wait.buckets.size());
                for (size_t b = 0; b < common; ++b) {
                    cumulative.buckets[b].count += entry.queue_wait.buckets[b].count;
                }
            }

            // 窗口差分：保存本次累计桶计数，再减去上次累计得到两次评估
            // 之间的样本增量。桶数变化（理论上不发生）时按全量重算处理。
            scheduling::FeedbackHistogram window = cumulative;
            std::vector<uint64_t>& last = last_cumulative_counts[q];
            std::vector<uint64_t> current(window.buckets.size());
            for (size_t b = 0; b < window.buckets.size(); ++b) {
                current[b] = window.buckets[b].count;
            }
            if (current.size() == last.size()) {
                for (size_t b = 0; b < window.buckets.size(); ++b) {
                    window.buckets[b].count =
                        current[b] > last[b] ? current[b] - last[b] : 0;
                }
            }
            last = std::move(current);

            const uint64_t window_samples = window.total();
            // 未知即宽容：目标未配置或窗口证据不足 → 清零连击并解除激
            // 活态（fail-open：无证据支持继续降载/提升时回到宽容侧；样
            // 本恢复后状态机按连续证据重新进入）。
            if (target <= 0 || window_samples < config.min_window_samples) {
                shed_streak[q] = 0;
                promotion_streak[q] = 0;
                shed_recover_streak[q] = 0;
                promotion_recover_streak[q] = 0;
                shed_active[q] = false;
                promoted[q] = false;
                continue;
            }

            const int64_t p99 =
                scheduling::histogram_quantile_ns(window, 0.99);
            const double recovery_ns =
                static_cast<double>(target) * config.recovery_ratio;
            const bool breached = p99 > target;
            const bool recovered = static_cast<double>(p99) < recovery_ns;

            // 降载状态机（对称滞回）：开闸需连续 shed_breach_windows 个
            // 超阈窗口；关闸需连续 shed_close_windows 个恢复窗口；两者
            // 之间维持现态，防止在阈值附近反复开关。
            shed_recover_streak[q] = recovered ? shed_recover_streak[q] + 1 : 0;
            if (shed_active[q]) {
                if (shed_recover_streak[q] >= config.shed_close_windows) {
                    shed_active[q] = false;
                }
            } else {
                shed_streak[q] = breached ? shed_streak[q] + 1 : 0;
                if (config.load_shedding_enabled &&
                    shed_streak[q] >= config.shed_breach_windows) {
                    shed_active[q] = true;
                }
            }

            // 提升状态机：同一超阈信号、更长的持续窗口（"长期"语义），
            // 取消提升同样需要持续恢复。
            if (config.priority_promotion_enabled) {
                promotion_recover_streak[q] =
                    recovered ? promotion_recover_streak[q] + 1 : 0;
                if (promoted[q]) {
                    if (promotion_recover_streak[q] >=
                        config.promotion_close_windows) {
                        promoted[q] = false;
                    }
                } else {
                    promotion_streak[q] = breached ? promotion_streak[q] + 1 : 0;
                    if (promotion_streak[q] >= config.promotion_windows) {
                        promoted[q] = true;
                    }
                }
            } else {
                promotion_streak[q] = 0;
                promoted[q] = false;
            }
        }

        // 全量重建掩码并发布（release：提交线程 acquire 读到即完整）。
        uint32_t shed_bits = 0;
        uint32_t promoted_bits = 0;
        for (size_t q = 0; q < kQosClassCount; ++q) {
            if (shed_active[q]) {
                shed_bits |= uint32_t{1} << q;
            }
            if (promoted[q]) {
                promoted_bits |= uint32_t{1} << q;
            }
        }
        shed_mask.store(shed_bits, std::memory_order_release);
        promoted_mask.store(promoted_bits, std::memory_order_release);
    }
};

// ============================================================================
// 构造 / 生命周期
// ============================================================================

AdaptiveScheduler::AdaptiveScheduler(AdaptiveSchedulerConfig config)
    : config_(std::move(config)), aggregator_(config_.aggregator) {
    // 注意：aggregator_ 必须经 config_.aggregator 构造（评估节奏 merge_
    // interval、桶界、EWMA 系数都是决策输入的一部分）；成员声明序保证
    // config_ 先于 aggregator_ 初始化。
    config_.min_samples =
        std::clamp<size_t>(config_.min_samples, 1, 1'000'000);
    config_.hysteresis_margin =
        std::clamp(config_.hysteresis_margin, 0.05, 0.9);
    config_.shed_breach_windows =
        std::clamp<size_t>(config_.shed_breach_windows, 1, 1'000'000);
    config_.shed_close_windows =
        std::clamp<size_t>(config_.shed_close_windows, 1, 1'000'000);
    // 提升须比降载更"长期"，否则两类决策没有区分度。
    config_.promotion_windows = std::clamp<size_t>(
        config_.promotion_windows, config_.shed_breach_windows, 1'000'000);
    config_.promotion_close_windows = std::clamp<size_t>(
        config_.promotion_close_windows, config_.promotion_windows, 1'000'000);
    config_.min_window_samples =
        std::clamp<size_t>(config_.min_window_samples, 1, 1'000'000);
    config_.recovery_ratio = std::clamp(config_.recovery_ratio, 0.1, 0.95);
    for (int64_t& target : config_.queue_wait_p99_target_ns) {
        target = std::max<int64_t>(target, 0);
    }
    state_ = std::make_unique<State>();
}

AdaptiveScheduler::~AdaptiveScheduler() = default;

// ============================================================================
// route()：pipeline 基线决策 → 派生状态重评估 → CPU/GPU 历史覆盖 → 降载
// ============================================================================

void AdaptiveScheduler::evaluate_if_new_snapshot() const {
    const std::shared_ptr<const scheduling::FeedbackSnapshot> snapshot =
        aggregator_.refresh_if_stale();
    if (!snapshot) {
        return;
    }
    // 热路径快速门限：无锁比较，绝大多数调用在此返回。
    if (state_->evaluated_merge_count_fast.load(std::memory_order_acquire) ==
        snapshot->merge_count) {
        return;
    }
    State& state = *state_;
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.evaluated_merge_count == snapshot->merge_count) {
        return;
    }
    state.evaluate(config_, *snapshot);
    state.evaluated_merge_count = snapshot->merge_count;
    state.evaluated_merge_count_fast.store(snapshot->merge_count,
                                           std::memory_order_release);
}

void AdaptiveScheduler::adapt_cpu_or_gpu(
    const TaskRouter::Request& request,
    const std::vector<ExecutorCapability>& capabilities,
    RoutingDecision& decision) const {
    const TaskOptions& options = request.options;

    // 显式用户约束优先于历史：RequireRequestedBackend 表达"点名该 GPU"，
    // 自适应不得翻向 CPU（与"显式 priority 永不被覆盖"同一原则）。
    if (options.fallback == FallbackPolicy::RequireRequestedBackend) {
        return;
    }

    // GPU 侧目标：决策已是 GPU 用决策名；决策是 CPU 时需要存在可提交的
    // GPU 才谈得上切换（无可用 GPU → 维持 CPU，不产生历史决策）。
    std::string gpu_name;
    if (decision.selected_backend == ExecutionBackend::Gpu) {
        gpu_name = decision.selected_executor_name;
    } else {
        const ExecutorCapability* gpu = detail::find_gpu_capability(
            capabilities, options.preferred_executor);
        if (gpu == nullptr || !detail::gpu_submittable(gpu)) {
            return;
        }
        gpu_name = gpu->name;
    }

    const std::shared_ptr<const scheduling::FeedbackSnapshot> snapshot =
        aggregator_.snapshot();
    const scheduling::FeedbackEntry* cpu_entry = scheduling::find_entry(
        *snapshot, ExecutionBackend::DefaultAsync, options.qos, "default");
    const scheduling::FeedbackEntry* gpu_entry = scheduling::find_entry(
        *snapshot, ExecutionBackend::Gpu, options.qos,
        truncate_executor_name(gpu_name));

    // 双侧样本都达到 min_samples 才采信历史；任一侧不足回退 0.6.1 启发
    // 式结果（不标记、不改写）。
    if (cpu_entry == nullptr || gpu_entry == nullptr ||
        cpu_entry->attempts < config_.min_samples ||
        gpu_entry->attempts < config_.min_samples) {
        return;
    }

    // 端到端时延 EWMA：queue wait + execution duration。
    const int64_t cpu_total =
        cpu_entry->ewma_queue_wait_ns + cpu_entry->ewma_execution_duration_ns;
    const int64_t gpu_total =
        gpu_entry->ewma_queue_wait_ns + gpu_entry->ewma_execution_duration_ns;
    const bool current_is_gpu =
        decision.selected_backend == ExecutionBackend::Gpu;

    // 滞回：挑战侧须优于在用侧 (1 - margin) 倍才切换；边际内维持现侧
    // （历史驱动的"维持"同样可解释）。
    const double challenge_ratio = 1.0 - config_.hysteresis_margin;
    const bool gpu_wins = static_cast<double>(gpu_total) <
                          static_cast<double>(cpu_total) * challenge_ratio;
    const bool cpu_wins = static_cast<double>(cpu_total) <
                          static_cast<double>(gpu_total) * challenge_ratio;

    bool target_gpu = current_is_gpu;
    std::string outcome;
    if (current_is_gpu && cpu_wins) {
        target_gpu = false;
        outcome = " -> CPU (beyond hysteresis margin)";
    } else if (!current_is_gpu && gpu_wins) {
        target_gpu = true;
        outcome = " -> GPU (beyond hysteresis margin)";
    } else {
        outcome = current_is_gpu ? " -> GPU (within hysteresis margin)"
                                 : " -> CPU (within hysteresis margin)";
    }

    decision.reason = RoutingReason::AdaptiveHistory;
    decision.diagnostics |= RoutingDiagnostics::AdaptiveHistory;
    decision.selected_backend = target_gpu ? ExecutionBackend::Gpu
                                           : ExecutionBackend::DefaultAsync;
    decision.selected_executor_name = target_gpu ? gpu_name : "default";
    decision.detail = "adaptive: cpu ewma " + std::to_string(cpu_total) +
                      "ns vs gpu ewma " + std::to_string(gpu_total) + "ns" +
                      outcome;
    // fell_back 保持原值：自适应选择是意图内的正常结果，不是回退。
}

void AdaptiveScheduler::maybe_shed(const TaskRouter::Request& request,
                                   RoutingDecision& decision) const {
    if (!config_.load_shedding_enabled) {
        return;
    }
    const size_t index = qos_index(request.options.qos);
    // 严格更高 QoS 类处于超阈态才拒绝（同类与更低类不互为保护对象）。
    const uint32_t higher =
        state_->shed_mask.load(std::memory_order_acquire) &
        ~((uint32_t{1} << (index + 1)) - 1);
    if (higher == 0) {
        return;
    }
    const size_t breached = static_cast<size_t>(std::countr_zero(higher));
    const auto breached_class = static_cast<QosClass>(breached);
    decision.reason = RoutingReason::LoadShedding;
    decision.status = RoutingStatus::Rejected;
    decision.diagnostics |= RoutingDiagnostics::LoadShedding;
    decision.selected_backend = ExecutionBackend::DefaultAsync;
    decision.selected_executor_name = "default";
    decision.detail =
        std::string("adaptive load shedding: QoS '") +
        qos_class_to_string(breached_class) +
        "' queue-wait p99 exceeded target " +
        std::to_string(config_.queue_wait_p99_target_ns[breached]) +
        "ns; rejecting lower QoS '" +
        qos_class_to_string(request.options.qos) + "' submission";
}

RoutingDecision AdaptiveScheduler::route(
    const TaskRouter::Request& request,
    const std::vector<ExecutorCapability>& capabilities) {
    // 阶段一~四：与 DefaultScheduler 相同的 0.6.1 基线（约束过滤 /
    // 候选生成 / 空评分 / 选择 + affinity advisory）。
    RoutingDecision decision = pipeline_.route(request, capabilities);
    if (decision.status == RoutingStatus::Rejected) {
        return decision;
    }

    // 派生状态重评估（merge_interval 门限内至多一次，冷路径）。
    evaluate_if_new_snapshot();

    // CPU/GPU 历史覆盖（仅 CpuOrGpu，且基线候选可切换时生效）。
    if (request.cpu_gpu_task) {
        adapt_cpu_or_gpu(request, capabilities, decision);
    }

    // QoS 降载：目的地为默认池的提交才受默认池 queue wait 降载影响。
    if (decision.status != RoutingStatus::Rejected &&
        decision.selected_backend == ExecutionBackend::DefaultAsync) {
        maybe_shed(request, decision);
    }
    return decision;
}

// ============================================================================
// 反馈与优先级咨询
// ============================================================================

void AdaptiveScheduler::on_task_completed(const SchedulingFeedback& feedback) {
    // worker 热路径：无锁、不阻塞、不抛异常（record 本身 noexcept）。
    aggregator_.record(feedback);
}

int AdaptiveScheduler::effective_priority_for(const TaskOptions& options,
                                              int default_priority) const {
    if (options.priority_set) {
        // 防御性双保险：facade 已保证显式 priority 不进入本方法。
        return default_priority;
    }
    if ((state_->promoted_mask.load(std::memory_order_acquire) &
         qos_bit(options.qos)) == 0) {
        return default_priority;
    }
    // 有界提升：恰好 +1 级，封顶 CRITICAL。
    return std::min(default_priority + 1, static_cast<int>(TaskPriority::CRITICAL));
}

// ============================================================================
// 诊断只读接口
// ============================================================================

bool AdaptiveScheduler::load_shed_active(QosClass breached_class) const noexcept {
    return (state_->shed_mask.load(std::memory_order_acquire) &
            qos_bit(breached_class)) != 0;
}

bool AdaptiveScheduler::priority_promoted(QosClass qos) const noexcept {
    return (state_->promoted_mask.load(std::memory_order_acquire) &
            qos_bit(qos)) != 0;
}

std::shared_ptr<const scheduling::FeedbackSnapshot>
AdaptiveScheduler::feedback_snapshot() const {
    evaluate_if_new_snapshot();
    return aggregator_.snapshot();
}

std::string AdaptiveScheduler::format_state_text() const {
    const State& state = *state_;
    std::lock_guard<std::mutex> lock(state.mutex);
    std::string text = "adaptive: shed=[";
    bool first = true;
    for (size_t q = 0; q < kQosClassCount; ++q) {
        if (!state.shed_active[q]) {
            continue;
        }
        text += first ? "" : ",";
        text += qos_class_to_string(static_cast<QosClass>(q));
        first = false;
    }
    text += "] promoted=[";
    first = true;
    for (size_t q = 0; q < kQosClassCount; ++q) {
        if (!state.promoted[q]) {
            continue;
        }
        text += first ? "" : ",";
        text += qos_class_to_string(static_cast<QosClass>(q));
        first = false;
    }
    text += "] merges=" + std::to_string(state.evaluated_merge_count);
    return text;
}

}  // namespace kairo
