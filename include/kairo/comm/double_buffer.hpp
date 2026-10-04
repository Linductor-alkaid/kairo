#pragma once

#include <kairo/comm/fwd.hpp>
#include <kairo/comm/lockfree_core.hpp>
#include <kairo/comm/phase_gate.hpp>
#include <kairo/comm/snapshot_store.hpp>
#include <kairo/comm/types.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <thread>
#include <utility>

namespace kairo::comm {

template <class T>
struct Snapshot {
    T value;
    uint64_t sequence = 0;
    std::chrono::steady_clock::time_point timestamp =
        std::chrono::steady_clock::now();
};

template <class T>
class DoubleBuffer {
public:
    explicit DoubleBuffer(T initial, std::string name = {})
        : name_(std::move(name)), store_(std::move(initial)), stats_(true) {}

    template <class U = T>
    requires std::is_default_constructible_v<U>
    explicit DoubleBuffer(std::string name = {})
        : DoubleBuffer(T{}, std::move(name)) {}

    ~DoubleBuffer() {
        PhaseGate::unregister_let_binding(let_core_.get());
    }

    // Opt into phase-bound LET semantics. This is a configuration operation;
    // storage and synchronization are preallocated and lock-free. T operations,
    // clocks, and diagnostic-result construction are outside that guarantee.
    CommResult bind_to_phase_gate(PhaseGate& gate, size_t capacity = 2) {
        static_assert(std::is_nothrow_copy_assignable_v<T>,
                      "LET DoubleBuffer requires nothrow copy assignment");
        static_assert(std::is_nothrow_copy_constructible_v<T>,
                      "LET DoubleBuffer requires nothrow copy construction");
        if (capacity != 2 || let_core_) {
            return CommResult::failure(CommErrorCode::InvalidArgument,
                                       "LET DoubleBuffer capacity is fixed at two and may bind once");
        }
        let_core_ = gate.register_let_binding();
        let_states_[0].store(kLetEmpty, std::memory_order_relaxed);
        let_states_[1].store(kLetEmpty, std::memory_order_relaxed);
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

    CommResult load_for_current_phase(Snapshot<T>& out) const noexcept {
        if (!let_core_) {
            return CommResult::failure(CommErrorCode::InvalidArgument);
        }
        auto lease = PhaseGate::try_begin_let_access(let_core_.get(), false);
        if (!lease) {
            return CommResult::failure(CommErrorCode::NotReady);
        }
        const uint64_t phase = lease->phase;
        if (phase == 0) {
            return CommResult::failure(CommErrorCode::NotReady);
        }
        const uint64_t expected = phase - 1;
        const size_t index = static_cast<size_t>(expected & 1U);
        if (let_states_[index].load(std::memory_order_acquire) != expected) {
            return CommResult::failure(CommErrorCode::NotReady);
        }
        out.value = *let_buffers_[index];
        out.sequence = expected;
        out.timestamp = let_timestamps_[index];
        return CommResult::success();
    }

    uint64_t publish(T value) {
        const uint64_t sequence = store_.publish_wait(std::move(value));
        record_publish();
        return sequence;
    }

    // Lock-free, non-waiting synchronization path. false means every preallocated
    // version slot is pinned or writer-owned. T operations and clock access are
    // outside this progress guarantee.
    bool try_publish(const T& value, uint64_t* new_sequence = nullptr) {
        return try_publish_impl(value, new_sequence);
    }

    bool try_publish(T&& value, uint64_t* new_sequence = nullptr) {
        return try_publish_impl(std::move(value), new_sequence);
    }

    // SWMR compatibility adapter: concurrent writers may overwrite each other
    // logically even though stored snapshots remain data-race-free.
    template <class Fn>
    uint64_t update(Fn&& writer) {
        auto current = load_wait();
        std::forward<Fn>(writer)(*current.value);
        const uint64_t sequence = store_.publish_wait(std::move(*current.value));
        record_publish();
        return sequence;
    }

    bool try_load(Snapshot<T>& out) const {
        typename Store::ReadResult result;
        if (!store_.try_load(result)) return false;
        record_load(result.sequence, result.timestamp);
        out = Snapshot<T>{std::move(*result.value), result.sequence, result.timestamp};
        return true;
    }

    Snapshot<T> load() const {
        auto result = load_wait();
        record_load(result.sequence, result.timestamp);
        return Snapshot<T>{std::move(*result.value), result.sequence, result.timestamp};
    }

    bool load_newer_than(uint64_t last_seen_sequence, Snapshot<T>& out) const {
        const uint64_t observed = store_.sequence();
        if (observed <= last_seen_sequence) {
            record_stale_read(observed);
            return false;
        }

        auto result = load_wait();
        if (result.sequence <= last_seen_sequence) {
            record_stale_read(result.sequence);
            return false;
        }

        record_load(result.sequence, result.timestamp);
        out = Snapshot<T>{std::move(*result.value), result.sequence, result.timestamp};
        return true;
    }

    uint64_t sequence() const noexcept { return store_.sequence(); }

    CommStats stats() const noexcept {
        CommStats snapshot = stats_.snapshot(
            1, 2, sequence(), consumer_lag_.load(std::memory_order_relaxed));
        if (snapshot.peak_depth == 0) snapshot.peak_depth = 1;
        return snapshot;
    }

    // Configuration-plane operation. Configure before concurrent use.
    void set_event_callback(CommEventCallback callback) {
        callback_.set(std::move(callback));
    }

    bool is_synchronization_lock_free() const noexcept {
        return store_.is_synchronization_lock_free() &&
               consumed_sequence_.is_lock_free() &&
               consumer_lag_.is_lock_free() && stats_.is_lock_free() &&
               callback_.is_lock_free() && let_states_[0].is_lock_free() &&
               let_states_[1].is_lock_free();
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

    typename Store::ReadResult load_wait() const {
        typename Store::ReadResult result;
        while (!store_.try_load(result)) {
            std::this_thread::yield();
        }
        return result;
    }

    template <class U>
    bool try_publish_impl(U&& value, uint64_t* new_sequence) {
        uint64_t sequence = 0;
        if (!store_.try_publish(std::forward<U>(value), sequence)) return false;
        record_publish();
        if (new_sequence) *new_sequence = sequence;
        return true;
    }

    void record_publish() noexcept {
        stats_.record_send(1);
    }

    void record_load(uint64_t sequence,
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

    void record_stale_read(uint64_t sequence) const {
        stats_.record_stale_read();
        if (callback_.configured()) {
            callback_.emit(CommEvent{CommEventKind::StaleRead, name_,
                                     "double buffer has no newer snapshot", sequence});
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

} // namespace kairo::comm
