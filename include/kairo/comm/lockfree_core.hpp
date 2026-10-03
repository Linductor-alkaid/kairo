#pragma once

#include <kairo/comm/types.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace kairo::comm::detail {

class AtomicCommStats {
public:
    explicit AtomicCommStats(bool enabled = true) noexcept
        : enabled_(enabled) {}

    bool enabled() const noexcept { return enabled_; }

    void record_send(uint64_t depth = 0) noexcept {
        if (!enabled_) return;
        sent_count_.fetch_add(1, std::memory_order_relaxed);
        update_peak(depth);
    }

    void record_receive(std::chrono::nanoseconds latency = {}) noexcept {
        if (!enabled_) return;
        received_count_.fetch_add(1, std::memory_order_relaxed);
        const uint64_t value = nonnegative_count(latency);
        total_latency_ns_.fetch_add(value, std::memory_order_relaxed);
        update_max(max_latency_ns_, value);
        latency_histogram_[latency_histogram_bucket(std::chrono::nanoseconds{value})]
            .fetch_add(1, std::memory_order_relaxed);
    }

    void record_drop() noexcept { increment(dropped_count_); }
    void record_overwrite() noexcept { increment(overwritten_count_); }
    void record_stale_read() noexcept { increment(stale_read_count_); }
    void record_closed_send() noexcept { increment(closed_send_count_); }
    void record_timeout() noexcept { increment(timeout_count_); }
    void record_handler_exception() noexcept { increment(handler_exception_count_); }
    void record_missed_phase() noexcept { increment(missed_phase_count_); }

    uint64_t sent_count() const noexcept {
        return sent_count_.load(std::memory_order_relaxed);
    }

    CommStats snapshot(uint64_t depth,
                       uint64_t capacity,
                       uint64_t producer_lag,
                       uint64_t consumer_lag) const noexcept {
        CommStats result;
        if (enabled_) {
            result.sent_count = sent_count_.load(std::memory_order_relaxed);
            result.received_count = received_count_.load(std::memory_order_relaxed);
            result.dropped_count = dropped_count_.load(std::memory_order_relaxed);
            result.overwritten_count = overwritten_count_.load(std::memory_order_relaxed);
            result.stale_read_count = stale_read_count_.load(std::memory_order_relaxed);
            result.closed_send_count = closed_send_count_.load(std::memory_order_relaxed);
            result.timeout_count = timeout_count_.load(std::memory_order_relaxed);
            result.handler_exception_count =
                handler_exception_count_.load(std::memory_order_relaxed);
            result.missed_phase_count = missed_phase_count_.load(std::memory_order_relaxed);
            result.peak_depth = peak_depth_.load(std::memory_order_relaxed);
            result.producer_lag = producer_lag;
            result.consumer_lag = consumer_lag;
            const uint64_t max_latency = max_latency_ns_.load(std::memory_order_relaxed);
            const uint64_t total_latency = total_latency_ns_.load(std::memory_order_relaxed);
            result.max_latency = std::chrono::nanoseconds{clamp_duration(max_latency)};
            if (result.received_count != 0) {
                result.avg_latency = std::chrono::nanoseconds{
                    clamp_duration(total_latency / result.received_count)};
            }
            for (size_t index = 0; index < result.latency_histogram.size(); ++index) {
                result.latency_histogram[index] =
                    latency_histogram_[index].load(std::memory_order_relaxed);
            }
            result.p50_latency = latency_histogram_quantile(result, 50, 100);
            result.p99_latency = latency_histogram_quantile(result, 99, 100);
        }
        result.current_depth = depth;
        result.capacity = capacity;
        return result;
    }

    bool is_lock_free() const noexcept {
        if (!sent_count_.is_lock_free() || !dropped_count_.is_lock_free() ||
            !overwritten_count_.is_lock_free() || !closed_send_count_.is_lock_free() ||
            !peak_depth_.is_lock_free() || !received_count_.is_lock_free() ||
            !stale_read_count_.is_lock_free() || !timeout_count_.is_lock_free() ||
            !handler_exception_count_.is_lock_free() ||
            !missed_phase_count_.is_lock_free() || !max_latency_ns_.is_lock_free() ||
            !total_latency_ns_.is_lock_free()) {
            return false;
        }
        for (const auto& bucket : latency_histogram_) {
            if (!bucket.is_lock_free()) return false;
        }
        return true;
    }

private:
    static uint64_t nonnegative_count(std::chrono::nanoseconds value) noexcept {
        return value.count() > 0 ? static_cast<uint64_t>(value.count()) : 0;
    }

    static int64_t clamp_duration(uint64_t value) noexcept {
        constexpr uint64_t limit = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
        return static_cast<int64_t>(std::min(value, limit));
    }

    void increment(std::atomic<uint64_t>& counter) noexcept {
        if (enabled_) counter.fetch_add(1, std::memory_order_relaxed);
    }

    void update_peak(uint64_t depth) noexcept {
        uint64_t current = peak_depth_.load(std::memory_order_relaxed);
        while (current < depth &&
               !peak_depth_.compare_exchange_weak(current, depth,
                                                   std::memory_order_relaxed,
                                                   std::memory_order_relaxed)) {
        }
    }

    static void update_max(std::atomic<uint64_t>& target, uint64_t value) noexcept {
        uint64_t current = target.load(std::memory_order_relaxed);
        while (current < value &&
               !target.compare_exchange_weak(current, value,
                                              std::memory_order_relaxed,
                                              std::memory_order_relaxed)) {
        }
    }

    const bool enabled_;
    alignas(64) std::atomic<uint64_t> sent_count_{0};
    std::atomic<uint64_t> dropped_count_{0};
    std::atomic<uint64_t> overwritten_count_{0};
    std::atomic<uint64_t> closed_send_count_{0};
    std::atomic<uint64_t> peak_depth_{0};

    alignas(64) std::atomic<uint64_t> received_count_{0};
    std::atomic<uint64_t> stale_read_count_{0};
    std::atomic<uint64_t> timeout_count_{0};
    std::atomic<uint64_t> handler_exception_count_{0};
    std::atomic<uint64_t> missed_phase_count_{0};
    std::atomic<uint64_t> max_latency_ns_{0};
    std::atomic<uint64_t> total_latency_ns_{0};
    std::array<std::atomic<uint64_t>, CommStats::kLatencyHistogramBuckets>
        latency_histogram_{};
};

class CallbackSlot {
public:
    CallbackSlot() = default;
    CallbackSlot(const CallbackSlot&) = delete;
    CallbackSlot& operator=(const CallbackSlot&) = delete;

    // Configuration-plane operation. Concurrent setters are not supported;
    // previously installed entries stay alive until the owning primitive dies.
    void set(CommEventCallback callback) {
        if (!callback) {
            current_.store(nullptr, std::memory_order_release);
            return;
        }
        auto entry = std::make_unique<CommEventCallback>(std::move(callback));
        const CommEventCallback* pointer = entry.get();
        entries_.push_back(std::move(entry));
        current_.store(pointer, std::memory_order_release);
    }

    bool configured() const noexcept {
        return current_.load(std::memory_order_acquire) != nullptr;
    }

    void emit(const std::optional<CommEvent>& event) const noexcept {
        if (!event) return;
        const CommEventCallback* callback = current_.load(std::memory_order_acquire);
        if (!callback) return;
        try {
            (*callback)(*event);
        } catch (...) {
            // Diagnostics must not affect the communication result.
        }
    }

    bool is_lock_free() const noexcept { return current_.is_lock_free(); }

private:
    std::atomic<const CommEventCallback*> current_{nullptr};
    std::vector<std::unique_ptr<CommEventCallback>> entries_;
};

} // namespace kairo::comm::detail
