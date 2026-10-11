#include "kairo/feedback_aggregator.hpp"

#include "kairo/scheduler.hpp"
#include "kairo/scheduling.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>

namespace kairo::scheduling {
namespace {

// 分片数固定：典型 worker 数（≤32）内一一对应、零争用；超过时多线程
// 共享分片，仍无锁（原子累加）。分片按进程级 thread_local 槽位稳定映射，
// 线程生命周期内不变。
constexpr size_t kShardCount = 32;

std::atomic<size_t> g_thread_slot_counter{0};

size_t thread_shard_slot() noexcept {
    thread_local const size_t slot =
        g_thread_slot_counter.fetch_add(1, std::memory_order_relaxed) % kShardCount;
    return slot;
}

int64_t steady_ns_now() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// record() 对样本值夹取到 [0, kMaxSampleNs]：负值无意义（steady 时钟单调，
// 正常路径不会出现），上界保证 EWMA 定点乘法 (sample - ewma) * 1000 不溢出。
int64_t clamp_sample(int64_t value) noexcept {
    if (value < 0) {
        return 0;
    }
    if (value > FeedbackAggregator::kMaxSampleNs) {
        return FeedbackAggregator::kMaxSampleNs;
    }
    return value;
}

// 桶定位：x <= bounds[0] → 0；bounds[j-1] < x <= bounds[j] → j；
// x > bounds[n-1] → n（溢出桶）。恰等于上界归上桶。
unsigned histogram_bucket_index(const int64_t* bounds, unsigned bound_count,
                                int64_t sample) noexcept {
    unsigned index = 0;
    while (index < bound_count && sample > bounds[index]) {
        ++index;
    }
    return index;
}

void update_ewma(std::atomic<int64_t>& cell, int64_t sample,
                 unsigned alpha_permille) noexcept {
    // CAS 循环：分片通常单线程独占，一次即成功；共享分片时退化自旋。
    int64_t current = cell.load(std::memory_order_relaxed);
    for (;;) {
        const int64_t target = current +
            (sample - current) * static_cast<int64_t>(alpha_permille) / 1000;
        if (cell.compare_exchange_weak(current, target, std::memory_order_relaxed)) {
            return;
        }
    }
}

const char* backend_to_string(ExecutionBackend backend) noexcept {
    switch (backend) {
    case ExecutionBackend::Gpu:
        return "Gpu";
    case ExecutionBackend::LockFree:
        return "LockFree";
    case ExecutionBackend::Realtime:
        return "Realtime";
    case ExecutionBackend::BlockingIo:
        return "BlockingIo";
    case ExecutionBackend::DefaultAsync:
    default:
        return "DefaultAsync";
    }
}

}  // namespace

// ---- 分片与槽位（对公开头隐藏）----

struct FeedbackAggregator::Shards {
    // 键槽：键字段一经发布不可变（慢路径写入、release 发布掩码位），统计
    // 字段全程原子累加。无槽位淘汰——键表只增不减，快路径键比较因此不
    // 需要任何同步。
    struct Slot {
        Slot() noexcept { clear(); }

        void clear() noexcept {
            executor_name[0] = '\0';
            name_length = 0;
            backend = ExecutionBackend::DefaultAsync;
            qos = QosClass::Standard;
            constexpr auto relaxed = std::memory_order_relaxed;
            attempts.store(0, relaxed);
            failures.store(0, relaxed);
            deadline_misses.store(0, relaxed);
            ewma_queue_wait_ns.store(0, relaxed);
            ewma_execution_duration_ns.store(0, relaxed);
            for (auto& count : queue_wait_hist) {
                count.store(0, relaxed);
            }
            for (auto& count : execution_hist) {
                count.store(0, relaxed);
            }
        }

        void set_key(ExecutionBackend key_backend, QosClass key_qos,
                     const char* name, size_t length) noexcept {
            name_length = static_cast<uint16_t>(
                std::min(length, kExecutorNameCapacity - 1));
            std::memcpy(executor_name, name, name_length);
            executor_name[name_length] = '\0';
            backend = key_backend;
            qos = key_qos;
        }

        bool key_matches(ExecutionBackend key_backend, QosClass key_qos,
                         const char* name, size_t length) const noexcept {
            return backend == key_backend && qos == key_qos &&
                   name_length == length &&
                   std::memcmp(executor_name, name, length) == 0;
        }

        // ---- 键（发布后不可变）----
        char executor_name[kExecutorNameCapacity];
        uint16_t name_length;
        ExecutionBackend backend;
        QosClass qos;

        // ---- 统计（record 与 refresh 并发，relaxed）----
        std::atomic<uint64_t> attempts;
        std::atomic<uint64_t> failures;
        std::atomic<uint64_t> deadline_misses;
        std::atomic<int64_t> ewma_queue_wait_ns;
        std::atomic<int64_t> ewma_execution_duration_ns;
        std::atomic<uint64_t> queue_wait_hist[kMaxHistogramBuckets];
        std::atomic<uint64_t> execution_hist[kMaxHistogramBuckets];
    };

    struct alignas(64) Shard {
        // 已发布键槽位图：位由慢路径在写入键字段之后 release 置位；快路径
        // acquire 读取，看到位即看到完整键。掩码只置位不清除（表只增不减）。
        std::atomic<uint64_t> occupied_mask{0};
        std::atomic<uint64_t> dropped_samples{0};
        std::unique_ptr<Slot[]> slots;
    };

    explicit Shards(size_t max_keys) {
        for (auto& shard : shard) {
            shard.slots = std::make_unique<Slot[]>(max_keys);
        }
    }

    // worker 热路径的单槽累加（EWMA 定点 CAS + 直方图桶累加）。
    void record_into(Slot& slot, const SchedulingFeedback& feedback,
                     unsigned alpha_permille) noexcept {
        constexpr auto relaxed = std::memory_order_relaxed;
        slot.attempts.fetch_add(1, relaxed);
        if (!feedback.success) {
            slot.failures.fetch_add(1, relaxed);
        }
        if (feedback.deadline_missed) {
            slot.deadline_misses.fetch_add(1, relaxed);
        }
        const int64_t queue_wait = clamp_sample(feedback.queue_wait_ns);
        const int64_t duration = clamp_sample(feedback.execution_duration_ns);
        update_ewma(slot.ewma_queue_wait_ns, queue_wait, alpha_permille);
        update_ewma(slot.ewma_execution_duration_ns, duration, alpha_permille);
        slot.queue_wait_hist[histogram_bucket_index(
            queue_wait_bounds, queue_wait_bound_count, queue_wait)]
            .fetch_add(1, relaxed);
        slot.execution_hist[histogram_bucket_index(
            execution_bounds, execution_bound_count, duration)]
            .fetch_add(1, relaxed);
    }

    Shard shard[kShardCount];

    // 直方图桶界的固定拷贝（record 热路径不触碰 config 的堆向量）。
    int64_t queue_wait_bounds[kMaxHistogramBuckets - 1] = {};
    int64_t execution_bounds[kMaxHistogramBuckets - 1] = {};
    unsigned queue_wait_bound_count = 0;
    unsigned execution_bound_count = 0;
};

// ---- 配置归一 ----

namespace {

constexpr size_t kDefaultBoundCount = 8;
constexpr int64_t kDefaultBounds[kDefaultBoundCount] = {
    1'000, 10'000, 100'000, 1'000'000, 10'000'000,
    100'000'000, 1'000'000'000, 10'000'000'000};

// 桶界须严格升序且 2..15 个：超限截断到 15；截断后不足 2 个或存在非升序
// 时整组退回默认界（诊断精度损失可接受，正确性优先）。
void normalize_bounds(const std::vector<int64_t>& input,
                      int64_t (&output)[FeedbackAggregator::kMaxHistogramBuckets - 1],
                      unsigned& count) {
    const size_t limit = std::min(input.size(), size_t{15});
    bool monotonic = limit >= 2;
    for (size_t i = 0; i < limit; ++i) {
        if (i > 0 && input[i] <= input[i - 1]) {
            monotonic = false;
        }
        output[i] = input[i];
    }
    if (!monotonic) {
        for (size_t i = 0; i < kDefaultBoundCount; ++i) {
            output[i] = kDefaultBounds[i];
        }
        count = static_cast<unsigned>(kDefaultBoundCount);
        return;
    }
    count = static_cast<unsigned>(limit);
}

}  // namespace

// ---- 生命周期 ----

FeedbackAggregator::FeedbackAggregator(FeedbackAggregatorConfig config)
    : config_(std::move(config)) {
    config_.max_keys = std::clamp<size_t>(config_.max_keys, 1, kMaxKeys);
    config_.ewma_alpha_permille =
        std::clamp<unsigned>(config_.ewma_alpha_permille, 1, 1000);
    shards_ = std::make_unique<Shards>(config_.max_keys);
    // 归一化后的桶界固化进分片（record 热路径的固定数组视图），并回写
    // config_ 使 config() 反映生效配置。
    normalize_bounds(config_.queue_wait_bucket_bounds_ns,
                     shards_->queue_wait_bounds, shards_->queue_wait_bound_count);
    normalize_bounds(config_.execution_bucket_bounds_ns,
                     shards_->execution_bounds, shards_->execution_bound_count);
    config_.queue_wait_bucket_bounds_ns.assign(
        shards_->queue_wait_bounds,
        shards_->queue_wait_bounds + shards_->queue_wait_bound_count);
    config_.execution_bucket_bounds_ns.assign(
        shards_->execution_bounds,
        shards_->execution_bounds + shards_->execution_bound_count);
    // 构造即发布空快照：snapshot() 契约"永不为空"。
    refresh();
}

FeedbackAggregator::~FeedbackAggregator() = default;

// ---- record（worker 热路径）----

void FeedbackAggregator::record(const SchedulingFeedback& feedback) noexcept {
    Shards::Shard& shard = shards_->shard[thread_shard_slot()];
    // SchedulingFeedback::executor_name 为 SSO 短字符串（"default"），
    // c_str/size 均不分配。键比较长度按槽内截断上限收敛（D3：截断后
    // 相同的键必须命中同一槽）。
    const char* name = feedback.executor_name.c_str();
    const size_t length =
        std::min(feedback.executor_name.size(), kExecutorNameCapacity - 1);
    const auto matches = [&](const Shards::Slot& slot) {
        return slot.key_matches(feedback.backend, feedback.qos, name, length);
    };
    const auto record_in = [&](Shards::Slot& slot) {
        shards_->record_into(slot, feedback, config_.ewma_alpha_permille);
    };

    // 快路径：acquire 读已发布槽位图，扫描本分片的键。
    uint64_t bits = shard.occupied_mask.load(std::memory_order_acquire);
    while (bits != 0) {
        const unsigned index = static_cast<unsigned>(std::countr_zero(bits));
        bits &= bits - 1;
        if (matches(shard.slots[index])) {
            record_in(shard.slots[index]);
            return;
        }
    }

    // 慢路径：登记互斥锁（建槽或丢弃计数），双检避免与并发建槽重复。
    std::lock_guard<std::mutex> lock(merge_mutex_);
    bits = shard.occupied_mask.load(std::memory_order_acquire);
    while (bits != 0) {
        const unsigned index = static_cast<unsigned>(std::countr_zero(bits));
        bits &= bits - 1;
        if (matches(shard.slots[index])) {
            record_in(shard.slots[index]);
            return;
        }
    }
    unsigned free_index = static_cast<unsigned>(kMaxKeys);
    for (size_t index = 0; index < config_.max_keys; ++index) {
        if ((shard.occupied_mask.load(std::memory_order_relaxed) &
             (uint64_t{1} << index)) == 0) {
            free_index = static_cast<unsigned>(index);
            break;
        }
    }
    if (free_index == kMaxKeys) {
        // 键空间有界：表满后新键样本丢弃计数——"未知即宽容"。
        shard.dropped_samples.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Shards::Slot& slot = shard.slots[free_index];
    slot.clear();
    slot.set_key(feedback.backend, feedback.qos, name, length);
    shard.occupied_mask.fetch_or(uint64_t{1} << free_index,
                                 std::memory_order_release);
    record_in(slot);
}

bool FeedbackAggregator::register_key(const FeedbackKey& key) {
    const char* name = key.executor_name.c_str();
    const size_t length =
        std::min(key.executor_name.size(), kExecutorNameCapacity - 1);
    const auto slot_of = [&](Shards::Shard& shard) -> Shards::Slot* {
        uint64_t bits = shard.occupied_mask.load(std::memory_order_acquire);
        while (bits != 0) {
            const unsigned index = static_cast<unsigned>(std::countr_zero(bits));
            bits &= bits - 1;
            if (shard.slots[index].key_matches(key.backend, key.qos, name, length)) {
                return &shard.slots[index];
            }
        }
        return nullptr;
    };
    std::lock_guard<std::mutex> lock(merge_mutex_);
    // 全或无：每个分片要么已有该键、要么有空槽，才动手建槽。
    for (auto& shard : shards_->shard) {
        if (slot_of(shard) != nullptr) {
            continue;
        }
        bool has_free = false;
        for (size_t index = 0; index < config_.max_keys; ++index) {
            if ((shard.occupied_mask.load(std::memory_order_relaxed) &
                 (uint64_t{1} << index)) == 0) {
                has_free = true;
                break;
            }
        }
        if (!has_free) {
            return false;
        }
    }
    for (auto& shard : shards_->shard) {
        if (slot_of(shard) != nullptr) {
            continue;
        }
        for (size_t index = 0; index < config_.max_keys; ++index) {
            if ((shard.occupied_mask.load(std::memory_order_relaxed) &
                 (uint64_t{1} << index)) == 0) {
                Shards::Slot& slot = shard.slots[index];
                slot.clear();
                slot.set_key(key.backend, key.qos, name, length);
                shard.occupied_mask.fetch_or(uint64_t{1} << index,
                                             std::memory_order_release);
                break;
            }
        }
    }
    return true;
}

// ---- 合并 / RCU 发布 ----

std::shared_ptr<const FeedbackSnapshot> FeedbackAggregator::snapshot()
    const noexcept {
    return snapshot_.load();
}

std::shared_ptr<const FeedbackSnapshot> FeedbackAggregator::refresh() const {
    std::lock_guard<std::mutex> lock(merge_mutex_);
    auto merged = std::make_shared<FeedbackSnapshot>();
    merged->merged_at = std::chrono::steady_clock::now();
    merged->merge_count = merge_count_.fetch_add(1, std::memory_order_relaxed) + 1;

    // 跨分片合并的 EWMA 加权累加器（与 entries 平行，不出公开类型）。
    std::vector<int64_t> ewma_queue_numerator;
    std::vector<int64_t> ewma_execution_numerator;
    const auto entry_index_of = [&](const FeedbackKey& key) {
        for (size_t i = 0; i < merged->entries.size(); ++i) {
            if (merged->entries[i].key == key) {
                return i;
            }
        }
        FeedbackEntry entry;
        entry.key = key;
        entry.queue_wait.buckets.reserve(shards_->queue_wait_bound_count + 1);
        for (unsigned b = 0; b < shards_->queue_wait_bound_count; ++b) {
            entry.queue_wait.buckets.push_back({shards_->queue_wait_bounds[b], 0});
        }
        entry.queue_wait.buckets.push_back(
            {std::numeric_limits<int64_t>::max(), 0});
        entry.execution_duration.buckets.reserve(
            shards_->execution_bound_count + 1);
        for (unsigned b = 0; b < shards_->execution_bound_count; ++b) {
            entry.execution_duration.buckets.push_back(
                {shards_->execution_bounds[b], 0});
        }
        entry.execution_duration.buckets.push_back(
            {std::numeric_limits<int64_t>::max(), 0});
        merged->entries.push_back(std::move(entry));
        ewma_queue_numerator.push_back(0);
        ewma_execution_numerator.push_back(0);
        return merged->entries.size() - 1;
    };

    for (auto& shard : shards_->shard) {
        uint64_t bits = shard.occupied_mask.load(std::memory_order_acquire);
        while (bits != 0) {
            const unsigned index = static_cast<unsigned>(std::countr_zero(bits));
            bits &= bits - 1;
            const Shards::Slot& slot = shard.slots[index];
            constexpr auto relaxed = std::memory_order_relaxed;
            const uint64_t attempts = slot.attempts.load(relaxed);
            // 预注册但无样本的键不产生 entry（快照只反映观测到的键）。
            if (attempts == 0) {
                continue;
            }
            FeedbackKey key;
            key.backend = slot.backend;
            key.qos = slot.qos;
            key.executor_name.assign(slot.executor_name, slot.name_length);
            const size_t entry_index = entry_index_of(key);
            FeedbackEntry& entry = merged->entries[entry_index];

            entry.attempts += attempts;
            entry.failures += slot.failures.load(relaxed);
            entry.deadline_misses += slot.deadline_misses.load(relaxed);
            merged->total_attempts += attempts;
            merged->total_failures += slot.failures.load(relaxed);
            // EWMA 跨分片按样本数加权：sum(ewma_i * attempts_i) / sum(attempts_i)。
            ewma_queue_numerator[entry_index] +=
                slot.ewma_queue_wait_ns.load(relaxed) *
                static_cast<int64_t>(attempts);
            ewma_execution_numerator[entry_index] +=
                slot.ewma_execution_duration_ns.load(relaxed) *
                static_cast<int64_t>(attempts);
            // 两个直方图桶数独立可配，各自按自己的桶数合并（D1：禁止
            // 共享循环下标）。
            for (size_t b = 0; b < entry.queue_wait.buckets.size(); ++b) {
                entry.queue_wait.buckets[b].count +=
                    slot.queue_wait_hist[b].load(relaxed);
            }
            for (size_t b = 0; b < entry.execution_duration.buckets.size(); ++b) {
                entry.execution_duration.buckets[b].count +=
                    slot.execution_hist[b].load(relaxed);
            }
        }
        merged->dropped_samples +=
            shard.dropped_samples.load(std::memory_order_relaxed);
    }

    for (size_t i = 0; i < merged->entries.size(); ++i) {
        FeedbackEntry& entry = merged->entries[i];
        if (entry.attempts == 0) {
            continue;
        }
        entry.failure_rate = static_cast<double>(entry.failures) /
                             static_cast<double>(entry.attempts);
        entry.ewma_queue_wait_ns =
            ewma_queue_numerator[i] / static_cast<int64_t>(entry.attempts);
        entry.ewma_execution_duration_ns =
            ewma_execution_numerator[i] / static_cast<int64_t>(entry.attempts);
    }

    // 输出确定性：按 (backend, qos, executor_name) 排序。
    std::sort(merged->entries.begin(), merged->entries.end(),
              [](const FeedbackEntry& lhs, const FeedbackEntry& rhs) {
                  if (lhs.key.backend != rhs.key.backend) {
                      return static_cast<uint8_t>(lhs.key.backend) <
                             static_cast<uint8_t>(rhs.key.backend);
                  }
                  if (lhs.key.qos != rhs.key.qos) {
                      return static_cast<uint8_t>(lhs.key.qos) <
                             static_cast<uint8_t>(rhs.key.qos);
                  }
                  return lhs.key.executor_name < rhs.key.executor_name;
              });

    snapshot_.store(std::move(merged));
    last_merge_steady_ns_.store(steady_ns_now(), std::memory_order_release);
    return snapshot();
}

std::shared_ptr<const FeedbackSnapshot> FeedbackAggregator::refresh_if_stale()
    const {
    const int64_t interval_ms = config_.merge_interval.count();
    if (interval_ms <= 0) {
        return refresh();
    }
    const int64_t now = steady_ns_now();
    const int64_t last = last_merge_steady_ns_.load(std::memory_order_acquire);
    if (now - last >= interval_ms * 1'000'000) {
        return refresh();
    }
    return snapshot();
}

// ---- 诊断文本 ----

std::string FeedbackAggregator::format_text(const FeedbackSnapshot& snapshot) {
    std::ostringstream output;
    output << "scheduling_feedback.merge_count=" << snapshot.merge_count << '\n';
    output << "scheduling_feedback.dropped_samples=" << snapshot.dropped_samples
           << '\n';
    output << "scheduling_feedback.total_attempts=" << snapshot.total_attempts
           << '\n';
    output << "scheduling_feedback.total_failures=" << snapshot.total_failures
           << '\n';
    output << "scheduling_feedback.entries.count=" << snapshot.entries.size()
           << '\n';
    for (size_t i = 0; i < snapshot.entries.size(); ++i) {
        const FeedbackEntry& entry = snapshot.entries[i];
        const std::string prefix =
            "scheduling_feedback.entries[" + std::to_string(i) + "]";
        output << prefix << ".backend=" << backend_to_string(entry.key.backend)
               << '\n';
        output << prefix << ".executor_name=" << entry.key.executor_name << '\n';
        output << prefix << ".qos=" << qos_class_to_string(entry.key.qos) << '\n';
        output << prefix << ".attempts=" << entry.attempts << '\n';
        output << prefix << ".failures=" << entry.failures << '\n';
        output << prefix << ".failure_rate=" << std::fixed
               << std::setprecision(3) << entry.failure_rate << '\n';
        output << prefix << ".deadline_misses=" << entry.deadline_misses << '\n';
        output << prefix << ".ewma_queue_wait_ns=" << entry.ewma_queue_wait_ns
               << '\n';
        output << prefix << ".ewma_execution_duration_ns="
               << entry.ewma_execution_duration_ns << '\n';
        const auto write_histogram = [&](const char* name,
                                         const FeedbackHistogram& histogram) {
            for (size_t b = 0; b < histogram.buckets.size(); ++b) {
                if (histogram.buckets[b].count == 0) {
                    continue;
                }
                output << prefix << "." << name << "[" << b << "].upper_bound_ns="
                       << histogram.buckets[b].upper_bound_ns << '\n';
                output << prefix << "." << name << "[" << b << "].count="
                       << histogram.buckets[b].count << '\n';
            }
        };
        write_histogram("queue_wait_hist", entry.queue_wait);
        write_histogram("execution_hist", entry.execution_duration);
    }
    return output.str();
}

}  // namespace kairo::scheduling
