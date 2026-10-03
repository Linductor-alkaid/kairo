#pragma once

#include <chrono>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace kairo::comm {

/**
 * @brief Common error codes for communication facade control operations.
 */
enum class CommErrorCode {
    Ok,
    Closed,
    Full,
    Empty,
    Timeout,
    Stale,
    MissedPhase,
    InvalidArgument,
    NotReady,
    Unknown
};

inline const char* comm_error_code_to_string(CommErrorCode code) noexcept {
    switch (code) {
    case CommErrorCode::Ok:
        return "Ok";
    case CommErrorCode::Closed:
        return "Closed";
    case CommErrorCode::Full:
        return "Full";
    case CommErrorCode::Empty:
        return "Empty";
    case CommErrorCode::Timeout:
        return "Timeout";
    case CommErrorCode::Stale:
        return "Stale";
    case CommErrorCode::MissedPhase:
        return "MissedPhase";
    case CommErrorCode::InvalidArgument:
        return "InvalidArgument";
    case CommErrorCode::NotReady:
        return "NotReady";
    case CommErrorCode::Unknown:
        return "Unknown";
    default:
        return "Unknown";
    }
}

/**
 * @brief Lightweight result for communication facade operations with diagnostics.
 */
struct CommResult {
    bool ok = true;
    CommErrorCode error_code = CommErrorCode::Ok;
    std::string message;

    explicit operator bool() const noexcept {
        return ok;
    }

    static CommResult success() noexcept {
        return {};
    }

    static CommResult success(std::string msg) {
        CommResult result;
        result.message = std::move(msg);
        return result;
    }

    static CommResult failure(CommErrorCode code) noexcept {
        CommResult result;
        result.ok = false;
        result.error_code = code;
        return result;
    }

    static CommResult failure(CommErrorCode code, std::string msg) {
        CommResult result;
        result.ok = false;
        result.error_code = code;
        result.message = std::move(msg);
        return result;
    }
};

enum class DropPolicy {
    RejectNewest,
    DropOldest,
    // 注意（CR-040）：DropOldest/KeepLatest 的"保最新"是 best-effort——
    // 队列内部对在途消费者/并发生产者的竞争窗口做有界自旋重试，重试耗尽
    // 时仍会按满队丢弃新值（事件为 Dropped）。需要强"最新值"语义的场景
    // 应使用 SnapshotStore/Mailbox。
    KeepLatest
};

inline const char* drop_policy_to_string(DropPolicy policy) noexcept {
    switch (policy) {
    case DropPolicy::RejectNewest:
        return "RejectNewest";
    case DropPolicy::DropOldest:
        return "DropOldest";
    case DropPolicy::KeepLatest:
        return "KeepLatest";
    default:
        return "Unknown";
    }
}

struct ChannelOptions {
    size_t capacity = 1024;
    DropPolicy drop_policy = DropPolicy::RejectNewest;
    bool enable_stats = true;
    std::string name;
};

struct RealtimeChannelOptions {
    size_t capacity = 1024;
    size_t max_items_per_cycle = 64;
    DropPolicy drop_policy = DropPolicy::RejectNewest;
    bool enable_stats = true;
    std::string name;
};

/**
 * @brief Local cumulative communication statistics.
 */
struct CommStats {
    static constexpr size_t kLatencyHistogramBuckets = 64;
    uint64_t sent_count = 0;
    uint64_t received_count = 0;
    uint64_t dropped_count = 0;
    uint64_t overwritten_count = 0;
    uint64_t stale_read_count = 0;
    uint64_t closed_send_count = 0;
    uint64_t timeout_count = 0;
    uint64_t handler_exception_count = 0;
    uint64_t missed_phase_count = 0;
    uint64_t current_depth = 0;
    uint64_t peak_depth = 0;
    uint64_t capacity = 0;
    uint64_t producer_lag = 0;
    uint64_t consumer_lag = 0;
    std::chrono::nanoseconds max_latency{0};
    std::chrono::nanoseconds avg_latency{0};
    std::chrono::nanoseconds p50_latency{0};
    std::chrono::nanoseconds p99_latency{0};
    std::array<uint64_t, kLatencyHistogramBuckets> latency_histogram{};
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
    LatencyHigh,
    HandlerException
};

inline const char* comm_event_kind_to_string(CommEventKind kind) noexcept {
    switch (kind) {
    case CommEventKind::Dropped:
        return "Dropped";
    case CommEventKind::Overwritten:
        return "Overwritten";
    case CommEventKind::ClosedSend:
        return "ClosedSend";
    case CommEventKind::Timeout:
        return "Timeout";
    case CommEventKind::StaleRead:
        return "StaleRead";
    case CommEventKind::MissedPhase:
        return "MissedPhase";
    case CommEventKind::ProducerLag:
        return "ProducerLag";
    case CommEventKind::ConsumerLag:
        return "ConsumerLag";
    case CommEventKind::LatencyHigh:
        return "LatencyHigh";
    case CommEventKind::HandlerException:
        return "HandlerException";
    default:
        return "Unknown";
    }
}

struct CommEvent {
    CommEventKind kind = CommEventKind::Dropped;
    std::string component_name;
    std::string message;
    uint64_t sequence = 0;
    std::chrono::steady_clock::time_point timestamp =
        std::chrono::steady_clock::now();
};

using CommEventCallback = std::function<void(const CommEvent&)>;

inline void emit_comm_event_noexcept(const CommEventCallback& callback,
                                     const std::optional<CommEvent>& event) noexcept {
    if (!callback || !event) {
        return;
    }

    try {
        callback(*event);
    } catch (...) {
        // Communication diagnostics must not change the data path outcome.
    }
}

inline size_t latency_histogram_bucket(std::chrono::nanoseconds latency) noexcept {
    uint64_t value = static_cast<uint64_t>(latency.count());
    size_t bucket = 0;
    while (value > 1 && bucket + 1 < CommStats::kLatencyHistogramBuckets) {
        value >>= 1U;
        ++bucket;
    }
    return bucket;
}

inline std::chrono::nanoseconds latency_histogram_upper_bound(size_t bucket) noexcept {
    if (bucket >= CommStats::kLatencyHistogramBuckets - 1) {
        return std::chrono::nanoseconds{std::numeric_limits<int64_t>::max()};
    }
    return std::chrono::nanoseconds{static_cast<int64_t>(uint64_t{1} << (bucket + 1U))};
}

inline std::chrono::nanoseconds latency_histogram_quantile(const CommStats& stats,
                                                            uint64_t numerator,
                                                            uint64_t denominator) noexcept {
    if (stats.received_count == 0 || denominator == 0) {
        return std::chrono::nanoseconds{0};
    }
    const uint64_t target = (stats.received_count * numerator + denominator - 1) / denominator;
    uint64_t seen = 0;
    for (size_t bucket = 0; bucket < stats.latency_histogram.size(); ++bucket) {
        seen += stats.latency_histogram[bucket];
        if (seen >= target) {
            return latency_histogram_upper_bound(bucket);
        }
    }
    return stats.max_latency;
}

inline void update_latency_stats(CommStats& stats,
                                 std::chrono::nanoseconds& total_latency,
                                 std::chrono::nanoseconds latency) noexcept {
    if (latency > stats.max_latency) {
        stats.max_latency = latency;
    }
    total_latency += latency;
    ++stats.latency_histogram[latency_histogram_bucket(latency)];
    if (stats.received_count > 0) {
        stats.avg_latency = total_latency / static_cast<int64_t>(stats.received_count);
        stats.p50_latency = latency_histogram_quantile(stats, 50, 100);
        stats.p99_latency = latency_histogram_quantile(stats, 99, 100);
    }
}

} // namespace kairo::comm
