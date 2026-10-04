#pragma once

#include <kairo/comm/fwd.hpp>
#include <kairo/comm/lockfree_core.hpp>
#include <kairo/comm/types.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace kairo::comm {

class PhaseGate {
private:
    static constexpr uint64_t kPhaseClosedBit = uint64_t{1} << 63U;
    static constexpr uint64_t kPhaseMask = kPhaseClosedBit - 1U;
    static constexpr uint64_t kLetClosedBit = uint64_t{1} << 63U;
    static constexpr uint64_t kLetTransitionBit = uint64_t{1} << 62U;
    static constexpr uint64_t kLetCountMask = kLetTransitionBit - 1U;

    struct Core {
        alignas(64) std::atomic<uint64_t> phase_state{0};
        std::atomic<uint64_t> let_access_state{0};
        std::atomic<uint64_t> let_binding_count{0};
        std::atomic<uint64_t> ref_count{1};
    };

    class CoreHandle {
    public:
        struct AdoptTag {};

        CoreHandle() noexcept = default;
        CoreHandle(Core* core, AdoptTag) noexcept : core_(core) {}
        explicit CoreHandle(Core* core) noexcept : core_(core) {
            core_->ref_count.fetch_add(1, std::memory_order_relaxed);
        }
        CoreHandle(const CoreHandle&) = delete;
        CoreHandle& operator=(const CoreHandle&) = delete;
        CoreHandle(CoreHandle&& other) noexcept
            : core_(std::exchange(other.core_, nullptr)) {}
        CoreHandle& operator=(CoreHandle&& other) noexcept {
            if (this == &other) return *this;
            reset();
            core_ = std::exchange(other.core_, nullptr);
            return *this;
        }
        ~CoreHandle() { reset(); }

        Core* get() const noexcept { return core_; }
        Core* operator->() const noexcept { return core_; }
        explicit operator bool() const noexcept { return core_ != nullptr; }

        void reset() noexcept {
            Core* core = std::exchange(core_, nullptr);
            if (core &&
                core->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                delete core;
            }
        }

    private:
        Core* core_ = nullptr;
    };

public:
    struct LetWriteLease {
        uint64_t phase = 0;
        bool writer = true;

        LetWriteLease() = default;
        LetWriteLease(const LetWriteLease&) = delete;
        LetWriteLease& operator=(const LetWriteLease&) = delete;
        LetWriteLease(LetWriteLease&&) noexcept = default;
        LetWriteLease& operator=(LetWriteLease&& other) noexcept {
            if (this == &other) return *this;
            release();
            core_ = std::move(other.core_);
            phase = other.phase;
            writer = other.writer;
            return *this;
        }
        ~LetWriteLease() { release(); }

        void release() noexcept {
            if (!core_) return;
            core_->let_access_state.fetch_sub(1, std::memory_order_release);
            core_.reset();
        }

        explicit operator bool() const noexcept { return static_cast<bool>(core_); }

    private:
        friend class PhaseGate;

        LetWriteLease(Core* owner, uint64_t value, bool is_writer) noexcept
            : phase(value), writer(is_writer), core_(owner) {}

        CoreHandle core_;
    };

    explicit PhaseGate(std::string name = {})
        : name_(std::move(name)),
          core_(new Core, typename CoreHandle::AdoptTag{}),
          stats_(true) {
        require_lock_free();
    }

    ~PhaseGate() {
        close_core();
    }

    PhaseGate(const PhaseGate&) = delete;
    PhaseGate& operator=(const PhaseGate&) = delete;

    uint64_t current_phase() const noexcept {
        return core_->phase_state.load(std::memory_order_acquire) & kPhaseMask;
    }

    CommResult advance_to(uint64_t phase) {
        return advance_locked_to(phase);
    }

    CommResult advance() {
        const uint64_t current = current_phase();
        if (current == kPhaseMask) {
            return missed_noalloc();
        }
        return advance_locked_to(current + 1);
    }

    bool has_reached(uint64_t phase) const noexcept {
        return current_phase() >= phase;
    }

    uint64_t let_phase() const noexcept { return current_phase(); }

    template <class Rep, class Period>
    CommResult wait_for(uint64_t phase,
                        std::chrono::duration<Rep, Period> timeout) {
        return wait_until_impl(phase, timeout, false);
    }

    template <class Rep, class Period>
    CommResult wait_for_exact(uint64_t phase,
                              std::chrono::duration<Rep, Period> timeout) {
        return wait_until_impl(phase, timeout, true);
    }

    CommResult close() {
        uint64_t state = core_->phase_state.load(std::memory_order_acquire);
        for (;;) {
            if ((state & kPhaseClosedBit) != 0) {
                return CommResult::failure(CommErrorCode::Closed);
            }
            if (core_->phase_state.compare_exchange_weak(
                    state, state | kPhaseClosedBit,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                core_->let_access_state.fetch_or(kLetClosedBit, std::memory_order_acq_rel);
                return CommResult::success();
            }
        }
    }

    bool is_closed() const noexcept {
        return (core_->phase_state.load(std::memory_order_acquire) & kPhaseClosedBit) != 0;
    }

    CommStats stats() const noexcept {
        const uint64_t phase = current_phase();
        return stats_.snapshot(0, 0, phase,
                               waiter_count_.load(std::memory_order_relaxed));
    }

    // Configuration-plane operation. Configure before concurrent use.
    void set_event_callback(CommEventCallback callback) {
        callback_.set(std::move(callback));
    }

    bool is_synchronization_lock_free() const noexcept {
        return core_->phase_state.is_lock_free() &&
               core_->let_access_state.is_lock_free() &&
               core_->let_binding_count.is_lock_free() &&
               core_->ref_count.is_lock_free() && waiter_count_.is_lock_free() &&
               stats_.is_lock_free() && callback_.is_lock_free();
    }

    bool is_lock_free() const noexcept {
        return is_synchronization_lock_free();
    }

    std::optional<LetWriteLease> try_begin_let_write() noexcept {
        return try_begin_let_access(core_.get(), true);
    }

    std::optional<LetWriteLease> try_begin_let_read() noexcept {
        return try_begin_let_access(core_.get(), false);
    }

private:
    template <class> friend class DoubleBuffer;
    template <class> friend class LatestMailbox;

    void require_lock_free() const {
        if (!is_synchronization_lock_free()) {
            throw std::runtime_error(
                "PhaseGate requires lock-free pointer and 64-bit atomics");
        }
    }

    void close_core() noexcept {
        core_->phase_state.fetch_or(kPhaseClosedBit,
                                    std::memory_order_acq_rel);
        core_->let_access_state.fetch_or(kLetClosedBit,
                                         std::memory_order_acq_rel);
    }

    CoreHandle register_let_binding() noexcept {
        core_->let_binding_count.fetch_add(1, std::memory_order_acq_rel);
        return CoreHandle(core_.get());
    }

    static void unregister_let_binding(Core* core) noexcept {
        if (core) {
            core->let_binding_count.fetch_sub(1, std::memory_order_acq_rel);
        }
    }

    static std::optional<LetWriteLease> try_begin_let_access(
        Core* core, bool writer) noexcept {
        uint64_t state = core->let_access_state.load(std::memory_order_acquire);
        for (;;) {
            if ((state & (kLetClosedBit | kLetTransitionBit)) != 0 ||
                (state & kLetCountMask) == kLetCountMask) {
                return std::nullopt;
            }
            if (core->let_access_state.compare_exchange_weak(
                    state, state + 1,
                    std::memory_order_acquire,
                    std::memory_order_relaxed)) {
                const uint64_t phase_state =
                    core->phase_state.load(std::memory_order_acquire);
                if ((phase_state & kPhaseClosedBit) != 0) {
                    core->let_access_state.fetch_sub(1, std::memory_order_release);
                    return std::nullopt;
                }
                return LetWriteLease(core, phase_state & kPhaseMask, writer);
            }
        }
    }

    CommResult advance_locked_to(uint64_t phase) {
        uint64_t access = core_->let_access_state.load(std::memory_order_acquire);
        for (;;) {
            if ((access & kLetCountMask) != 0 ||
                (access & kLetTransitionBit) != 0) {
                return CommResult::failure(CommErrorCode::NotReady);
            }
            if ((access & kLetClosedBit) != 0) {
                return closed_noalloc();
            }
            if (core_->let_access_state.compare_exchange_weak(
                    access, access | kLetTransitionBit,
                    std::memory_order_acquire,
                    std::memory_order_relaxed)) {
                break;
            }
        }

        uint64_t phase_state = core_->phase_state.load(std::memory_order_acquire);
        for (;;) {
            if ((phase_state & kPhaseClosedBit) != 0) {
                core_->let_access_state.fetch_or(kLetClosedBit, std::memory_order_release);
                core_->let_access_state.fetch_and(~kLetTransitionBit,
                                            std::memory_order_release);
                return closed_noalloc();
            }
            const uint64_t current = phase_state & kPhaseMask;
            const bool let_bound =
                core_->let_binding_count.load(std::memory_order_acquire) != 0;
            if (phase == 0 || phase > kPhaseMask || phase <= current ||
                (let_bound && phase == kPhaseMask)) {
                core_->let_access_state.fetch_and(~kLetTransitionBit,
                                                  std::memory_order_release);
                return missed_noalloc();
            }
            if (core_->phase_state.compare_exchange_weak(
                    phase_state, phase,
                    std::memory_order_release,
                    std::memory_order_acquire)) {
                stats_.record_send();
                core_->let_access_state.fetch_and(~kLetTransitionBit,
                                            std::memory_order_release);
                return CommResult::success();
            }
        }
    }

    template <class Rep, class Period>
    CommResult wait_until_impl(uint64_t phase,
                               std::chrono::duration<Rep, Period> timeout,
                               bool exact) {
        if (phase > kPhaseMask) {
            return CommResult::failure(CommErrorCode::InvalidArgument);
        }
        const auto started = std::chrono::steady_clock::now();
        const auto deadline = started + timeout;
        waiter_count_.fetch_add(1, std::memory_order_relaxed);
        WaiterLease waiter(waiter_count_);

        for (;;) {
            const uint64_t current = current_phase();
            if (exact && current > phase) {
                return missed("requested phase was already missed");
            }
            if ((!exact && current >= phase) || (exact && current == phase)) {
                stats_.record_receive(elapsed(started));
                return CommResult::success();
            }
            if (is_closed()) {
                return CommResult::failure(CommErrorCode::Closed,
                                           "phase gate is closed");
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                stats_.record_timeout();
                emit_event(CommEventKind::Timeout, "phase wait timed out");
                return CommResult::failure(CommErrorCode::Timeout,
                                           "phase wait timed out");
            }
            std::this_thread::yield();
        }
    }

    class WaiterLease {
    public:
        explicit WaiterLease(std::atomic<uint64_t>& count) noexcept : count_(count) {}
        ~WaiterLease() { count_.fetch_sub(1, std::memory_order_relaxed); }
        WaiterLease(const WaiterLease&) = delete;
        WaiterLease& operator=(const WaiterLease&) = delete;

    private:
        std::atomic<uint64_t>& count_;
    };

    static std::chrono::nanoseconds elapsed(
        std::chrono::steady_clock::time_point start) noexcept {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start);
    }

    CommResult missed_noalloc() {
        stats_.record_missed_phase();
        emit_event(CommEventKind::MissedPhase, "phase was missed");
        return CommResult::failure(CommErrorCode::MissedPhase);
    }

    CommResult missed(const char* message) {
        stats_.record_missed_phase();
        emit_event(CommEventKind::MissedPhase, "phase was missed");
        return CommResult::failure(CommErrorCode::MissedPhase, message);
    }

    CommResult closed_noalloc() {
        stats_.record_closed_send();
        emit_event(CommEventKind::ClosedSend,
                   "phase gate operation rejected after close");
        return CommResult::failure(CommErrorCode::Closed);
    }

    void emit_event(CommEventKind kind, const char* message) const noexcept {
        if (!callback_.configured()) return;
        callback_.emit(CommEvent{kind, name_, message, current_phase()});
    }

    std::string name_;
    CoreHandle core_;
    std::atomic<uint64_t> waiter_count_{0};
    detail::AtomicCommStats stats_;
    detail::CallbackSlot callback_;
};

class Sequencer {
public:
    explicit Sequencer(std::string name = {})
        : name_(std::move(name)), stats_(true) {
        if (!is_synchronization_lock_free()) {
            throw std::runtime_error(
                "Sequencer requires lock-free pointer and 64-bit atomics");
        }
    }

    uint64_t next_ticket() noexcept {
        if (is_closed()) return 0;
        uint64_t current = next_ticket_.load(std::memory_order_relaxed);
        for (;;) {
            if (is_closed() || current == kTicketMask) return 0;
            if (next_ticket_.compare_exchange_weak(
                    current, current + 1,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
                if (is_closed()) return 0;
                return current + 1;
            }
        }
    }

    CommResult publish(uint64_t ticket) {
        if (ticket == 0 || ticket > kTicketMask) {
            return missed_noalloc();
        }

        uint64_t state = published_state_.load(std::memory_order_acquire);
        for (;;) {
            if ((state & kTicketClosedBit) != 0) {
                return closed_noalloc();
            }
            if (ticket <= (state & kTicketMask)) {
                return missed_noalloc();
            }

            // Reserve future generated tickets before publishing the watermark.
            // Once the publication CAS succeeds, next_ticket() cannot return an
            // already-published value.
            raise_next_ticket(ticket);
            if (published_state_.compare_exchange_weak(
                    state, ticket,
                    std::memory_order_release,
                    std::memory_order_acquire)) {
                stats_.record_send();
                return CommResult::success();
            }
        }
    }

    bool is_published(uint64_t ticket) const noexcept {
        return ticket != 0 && published_ticket() >= ticket;
    }

    template <class Rep, class Period>
    CommResult wait_until_published(uint64_t ticket,
                                    std::chrono::duration<Rep, Period> timeout) {
        if (ticket == 0 || ticket > kTicketMask) {
            return CommResult::failure(CommErrorCode::InvalidArgument);
        }

        const auto started = std::chrono::steady_clock::now();
        const auto deadline = started + timeout;
        waiter_count_.fetch_add(1, std::memory_order_relaxed);
        WaiterLease waiter(waiter_count_);

        for (;;) {
            const uint64_t current = published_ticket();
            if (current == ticket) {
                stats_.record_receive(elapsed(started));
                return CommResult::success();
            }
            if (current > ticket) return missed("ticket was already missed");
            if (is_closed()) {
                return CommResult::failure(CommErrorCode::Closed,
                                           "sequencer is closed");
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                stats_.record_timeout();
                emit_event(CommEventKind::Timeout, "sequencer wait timed out");
                return CommResult::failure(CommErrorCode::Timeout,
                                           "sequencer wait timed out");
            }
            std::this_thread::yield();
        }
    }

    CommResult close() {
        uint64_t state = published_state_.load(std::memory_order_acquire);
        for (;;) {
            if ((state & kTicketClosedBit) != 0) {
                return CommResult::failure(CommErrorCode::Closed);
            }
            if (published_state_.compare_exchange_weak(
                    state, state | kTicketClosedBit,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return CommResult::success();
            }
        }
    }

    bool is_closed() const noexcept {
        return (published_state_.load(std::memory_order_acquire) &
                kTicketClosedBit) != 0;
    }

    uint64_t published_ticket() const noexcept {
        return published_state_.load(std::memory_order_acquire) & kTicketMask;
    }

    CommStats stats() const noexcept {
        return stats_.snapshot(0, 0, published_ticket(),
                               waiter_count_.load(std::memory_order_relaxed));
    }

    void set_event_callback(CommEventCallback callback) {
        callback_.set(std::move(callback));
    }

    bool is_synchronization_lock_free() const noexcept {
        return next_ticket_.is_lock_free() && published_state_.is_lock_free() &&
               waiter_count_.is_lock_free() && stats_.is_lock_free() &&
               callback_.is_lock_free();
    }

    bool is_lock_free() const noexcept {
        return is_synchronization_lock_free();
    }

private:
    static constexpr uint64_t kTicketClosedBit = uint64_t{1} << 63U;
    static constexpr uint64_t kTicketMask = kTicketClosedBit - 1U;

    class WaiterLease {
    public:
        explicit WaiterLease(std::atomic<uint64_t>& count) noexcept : count_(count) {}
        ~WaiterLease() { count_.fetch_sub(1, std::memory_order_relaxed); }

    private:
        std::atomic<uint64_t>& count_;
    };

    static std::chrono::nanoseconds elapsed(
        std::chrono::steady_clock::time_point start) noexcept {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start);
    }

    void raise_next_ticket(uint64_t ticket) noexcept {
        uint64_t current = next_ticket_.load(std::memory_order_relaxed);
        while (current < ticket &&
               !next_ticket_.compare_exchange_weak(
                   current, ticket,
                   std::memory_order_relaxed,
                   std::memory_order_relaxed)) {
        }
    }

    CommResult missed_noalloc() {
        stats_.record_missed_phase();
        emit_event(CommEventKind::MissedPhase, "ticket was missed");
        return CommResult::failure(CommErrorCode::MissedPhase);
    }

    CommResult missed(const char* message) {
        stats_.record_missed_phase();
        emit_event(CommEventKind::MissedPhase, "ticket was missed");
        return CommResult::failure(CommErrorCode::MissedPhase, message);
    }

    CommResult closed_noalloc() {
        stats_.record_closed_send();
        emit_event(CommEventKind::ClosedSend,
                   "sequencer operation rejected after close");
        return CommResult::failure(CommErrorCode::Closed);
    }

    void emit_event(CommEventKind kind, const char* message) const noexcept {
        if (!callback_.configured()) return;
        callback_.emit(CommEvent{kind, name_, message, published_ticket()});
    }

    std::string name_;
    alignas(64) std::atomic<uint64_t> next_ticket_{0};
    std::atomic<uint64_t> published_state_{0};
    std::atomic<uint64_t> waiter_count_{0};
    detail::AtomicCommStats stats_;
    detail::CallbackSlot callback_;
};

} // namespace kairo::comm
