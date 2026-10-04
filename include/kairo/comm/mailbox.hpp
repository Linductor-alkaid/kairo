#pragma once

#include <kairo/comm/bounded_queue.hpp>
#include <kairo/comm/fwd.hpp>
#include <kairo/comm/lockfree_core.hpp>
#include <kairo/comm/phase_gate.hpp>
#include <kairo/comm/snapshot_store.hpp>
#include <kairo/comm/types.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace kairo::comm {

template <class T>
class LatestMailbox {
public:
    explicit LatestMailbox(std::string name = {})
        : name_(std::move(name)), stats_(true) {}

    ~LatestMailbox() {
        PhaseGate::unregister_let_binding(let_core_.get());
    }

    CommResult bind_to_phase_gate(PhaseGate& gate, size_t capacity = 2) {
        static_assert(std::is_nothrow_copy_assignable_v<T>,
                      "LET LatestMailbox requires nothrow copy assignment");
        static_assert(std::is_nothrow_copy_constructible_v<T>,
                      "LET LatestMailbox requires nothrow copy construction");
        if (capacity != 2 || let_core_) {
            return CommResult::failure(
                CommErrorCode::InvalidArgument,
                "LET LatestMailbox capacity is fixed at two and may bind once");
        }
        let_core_ = gate.register_let_binding();
        return CommResult::success();
    }

    bool is_phase_bound() const noexcept { return static_cast<bool>(let_core_); }

    CommResult publish_for_current_phase(T value) noexcept {
        if (!let_core_) {
            return CommResult::failure(CommErrorCode::InvalidArgument);
        }
        auto lease = PhaseGate::try_begin_let_access(let_core_.get(), true);
        if (!lease) {
            return CommResult::failure(CommErrorCode::NotReady);
        }
        const uint64_t phase = lease->phase;
        if (phase >= kLetEmpty) {
            return CommResult::failure(CommErrorCode::InvalidArgument);
        }

        const size_t index = static_cast<size_t>(phase & 1U);
        uint64_t state = let_states_[index].load(std::memory_order_acquire);
        for (;;) {
            if (let_phase_of(state) == phase) {
                return CommResult::failure(CommErrorCode::MissedPhase);
            }
            if ((state & kLetWritingBit) != 0) {
                return CommResult::failure(CommErrorCode::NotReady);
            }
            if (let_states_[index].compare_exchange_weak(
                    state, kLetWritingBit | phase,
                    std::memory_order_acquire,
                    std::memory_order_relaxed)) {
                break;
            }
        }

        let_buffers_[index].emplace(value);
        let_timestamps_[index] = std::chrono::steady_clock::now();
        let_states_[index].store(phase, std::memory_order_release);
        return CommResult::success();
    }

    CommResult load_for_current_phase(T& out,
                                      uint64_t* visible_phase = nullptr) const noexcept {
        if (!let_core_) {
            return CommResult::failure(CommErrorCode::InvalidArgument);
        }
        auto lease = PhaseGate::try_begin_let_access(let_core_.get(), false);
        if (!lease) {
            return CommResult::failure(CommErrorCode::NotReady);
        }
        if (lease->phase == 0) {
            return CommResult::failure(CommErrorCode::NotReady);
        }
        const uint64_t expected = lease->phase - 1;
        const size_t index = static_cast<size_t>(expected & 1U);
        if (let_states_[index].load(std::memory_order_acquire) != expected) {
            return CommResult::failure(CommErrorCode::NotReady);
        }
        out = *let_buffers_[index];
        if (visible_phase) *visible_phase = expected;
        return CommResult::success();
    }

    void publish(const T& value) { (void)publish_wait(value); }
    void publish(T&& value) { (void)publish_wait(std::move(value)); }

    // Lock-free, non-waiting publication for realtime callers. false means every
    // preallocated version slot is temporarily pinned or owned by a writer.
    bool try_publish(const T& value, uint64_t* new_sequence = nullptr) {
        return try_publish_impl(value, new_sequence);
    }

    bool try_publish(T&& value, uint64_t* new_sequence = nullptr) {
        return try_publish_impl(std::move(value), new_sequence);
    }

    bool try_load(T& out) const {
        typename Store::ReadResult result;
        if (!store_.try_load(result)) return false;
        out = std::move(*result.value);
        record_load(result.sequence, result.timestamp);
        return true;
    }

    bool try_load_newer_than(uint64_t last_seen_sequence,
                             T& out,
                             uint64_t& new_sequence) const {
        if (store_.sequence() <= last_seen_sequence) {
            record_stale_read();
            return false;
        }

        typename Store::ReadResult result;
        if (!store_.try_load(result)) {
            return false;
        }
        if (result.sequence <= last_seen_sequence) {
            record_stale_read();
            return false;
        }
        out = std::move(*result.value);
        new_sequence = result.sequence;
        record_load(result.sequence, result.timestamp);
        return true;
    }

    uint64_t sequence() const noexcept { return store_.sequence(); }

    CommStats stats() const noexcept {
        const uint64_t current = sequence();
        return stats_.snapshot(store_.has_value() ? 1U : 0U, 1U, current,
                               consumer_lag_.load(std::memory_order_relaxed));
    }

    // Configuration-plane operation. Configure before concurrent use.
    void set_event_callback(CommEventCallback callback) {
        callback_.set(std::move(callback));
    }

    bool is_synchronization_lock_free() const noexcept {
        return store_.is_synchronization_lock_free() &&
               consumed_sequence_.is_lock_free() &&
               consumer_lag_.is_lock_free() && stats_.is_lock_free() &&
               callback_.is_lock_free() && let_states_[0].is_lock_free();
    }


private:
    using Store = detail::SnapshotStore<T>;
    static constexpr uint64_t kLetWritingBit = uint64_t{1} << 63U;
    static constexpr uint64_t kLetEmpty = kLetWritingBit - 1U;

    static uint64_t let_phase_of(uint64_t state) noexcept {
        return state & ~kLetWritingBit;
    }

    static std::chrono::nanoseconds elapsed(
        std::chrono::steady_clock::time_point start) noexcept {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start);
    }

    template <class U>
    bool try_publish_impl(U&& value, uint64_t* new_sequence) {
        uint64_t sequence = 0;
        if (!store_.try_publish(std::forward<U>(value), sequence)) return false;
        record_publish(sequence);
        if (new_sequence) *new_sequence = sequence;
        return true;
    }

    template <class U>
    uint64_t publish_wait(U&& value) {
        const uint64_t sequence = store_.publish_wait(std::forward<U>(value));
        record_publish(sequence);
        return sequence;
    }

    void record_load(
        uint64_t sequence,
        std::chrono::steady_clock::time_point timestamp) const noexcept {
        uint64_t previous =
            consumed_sequence_.load(std::memory_order_relaxed);
        while (previous < sequence &&
               !consumed_sequence_.compare_exchange_weak(
                   previous, sequence,
                   std::memory_order_relaxed,
                   std::memory_order_relaxed)) {
        }
        const uint64_t skipped =
            sequence > previous ? sequence - previous - 1 : 0;
        consumer_lag_.store(skipped, std::memory_order_relaxed);
        stats_.record_receive(elapsed(timestamp));
    }

    void record_publish(uint64_t sequence) {
        stats_.record_send(1);
        if (sequence <= 1) return;
        stats_.record_overwrite();
        if (callback_.configured()) {
            callback_.emit(CommEvent{CommEventKind::Overwritten, name_,
                                     "mailbox value overwritten", sequence});
        }
    }

    void record_stale_read() const {
        stats_.record_stale_read();
        if (callback_.configured()) {
            callback_.emit(CommEvent{CommEventKind::StaleRead, name_,
                                     "mailbox has no newer value", sequence()});
        }
    }

    std::string name_;
    Store store_;
    mutable detail::AtomicCommStats stats_;
    mutable std::atomic<uint64_t> consumed_sequence_{0};
    mutable std::atomic<uint64_t> consumer_lag_{0};
    detail::CallbackSlot callback_;

    PhaseGate::CoreHandle let_core_;
    std::optional<T> let_buffers_[2];
    std::chrono::steady_clock::time_point let_timestamps_[2]{};
    std::atomic<uint64_t> let_states_[2]{{kLetEmpty}, {kLetEmpty}};
};

template <class T>
class RealtimeChannel {
public:
    explicit RealtimeChannel(RealtimeChannelOptions options = {})
        : options_(normalize_options(std::move(options))),
          queue_(options_.capacity, options_.drop_policy, options_.enable_stats,
                 options_.name, "realtime channel") {}

    bool try_send(const T& value) { return try_send_impl(value); }
    bool try_send(T&& value) { return try_send_impl(std::move(value)); }

    template <class Fn>
    size_t drain_for_cycle(Fn&& handler, size_t max_items = 0) {
        // CR-042: 参数层 0 = "取 options 配置"；options 层 0 一律回退到内置
        // 默认值。此前 options 配 0 会静默变成"无限制 drain"——与参数层的
        // 0 语义相反，也与 RT 周期预算模型相悖。
        constexpr size_t kDefaultMaxItemsPerCycle = 64;
        const size_t configured_budget =
            options_.max_items_per_cycle == 0
                ? kDefaultMaxItemsPerCycle
                : options_.max_items_per_cycle;
        const size_t budget = max_items == 0 ? configured_budget : max_items;

        size_t drained = 0;
        while (drained < budget) {
            auto item = queue_.try_pop();
            if (!item) break;
            try {
                invoke_handler(handler, item->value);
            } catch (...) {
                record_handler_exception_event();
                throw;
            }
            ++drained;
        }
        return drained;
    }

    void close() noexcept { queue_.close(); }
    bool is_closed() const noexcept { return queue_.is_closed(); }
    bool is_drained() const noexcept { return queue_.is_drained(); }
    bool empty() const noexcept { return queue_.empty(); }
    size_t size_approx() const noexcept { return queue_.size(); }
    size_t capacity() const noexcept { return queue_.capacity(); }
    size_t max_items_per_cycle() const noexcept { return options_.max_items_per_cycle; }
    CommStats stats() const noexcept { return queue_.stats(); }

    // Configuration-plane operation. A callback makes exceptional/drop paths
    // unsuitable for hard realtime because user code is invoked synchronously.
    void set_event_callback(CommEventCallback callback) {
        queue_.set_event_callback(std::move(callback));
    }

    bool is_synchronization_lock_free() const noexcept {
        return queue_.is_lock_free();
    }


private:
    static RealtimeChannelOptions normalize_options(RealtimeChannelOptions options) {
        if (options.capacity == 0) options.capacity = 1;
        return options;
    }

    template <class U>
    bool try_send_impl(U&& value) {
        std::optional<CommEvent> event;
        const bool sent = queue_.enqueue(std::forward<U>(value), event);
        queue_.emit(event);
        return sent;
    }

    template <class Fn>
    static void invoke_handler(Fn& handler, T& item) {
        if constexpr (requires { handler(item); }) {
            handler(item);
        } else {
            handler(std::move(item));
        }
    }

    void record_handler_exception_event() {
        std::optional<CommEvent> event;
        queue_.record_handler_exception(event);
        queue_.emit(event);
    }

    RealtimeChannelOptions options_;
    BoundedQueue<T> queue_;
};

} // namespace kairo::comm
