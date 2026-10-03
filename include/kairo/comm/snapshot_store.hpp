#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

namespace kairo::comm::detail {

/**
 * Fixed-storage multi-writer/multi-reader latest-value exchange.
 *
 * Slot ownership and reader pins share one atomic word. A writer can only
 * mutate a slot after changing an unpinned state to Writing; a reader copies
 * T only while its pin is present. This keeps ordinary non-atomic T accesses
 * data-race-free without a seqlock. try_publish() is lock-free at the
 * synchronization level and may reject when every version slot is busy;
 * publish_wait() is a non-realtime compatibility adapter.
 */
template <class T, size_t SlotCount = 4>
class SnapshotStore {
    static_assert(SlotCount >= 2 && SlotCount < 255);

public:
    struct ReadResult {
        std::optional<T> value;
        uint64_t sequence = 0;
        std::chrono::steady_clock::time_point timestamp{};
    };

    SnapshotStore() {
        require_lock_free();
    }

    explicit SnapshotStore(T initial) : SnapshotStore() {
        Slot& slot = slots_[0];
        slot.value.emplace(std::move(initial));
        slot.sequence = 0;
        slot.timestamp = std::chrono::steady_clock::now();
        slot.state.store(kReadyBit, std::memory_order_release);
        current_.store(pack(0, 0), std::memory_order_release);
    }

    SnapshotStore(const SnapshotStore&) = delete;
    SnapshotStore& operator=(const SnapshotStore&) = delete;

    template <class U>
    bool try_publish(U&& value, uint64_t& sequence) {
        const size_t index = claim_slot();
        if (index == kNoSlot) return false;
        Slot& slot = slots_[index];

        const uint64_t next = reserve_sequence();
        if (next == 0) {
            slot.state.store(kFree, std::memory_order_release);
            return false;
        }

        try {
            slot.value.emplace(std::forward<U>(value));
            slot.timestamp = std::chrono::steady_clock::now();
        } catch (...) {
            slot.state.store(kFree, std::memory_order_release);
            throw;
        }

        slot.sequence = next;
        slot.state.store(kReadyBit, std::memory_order_release);
        publish_candidate(index, next);
        sequence = next;
        return true;
    }

    template <class U>
    uint64_t publish_wait(U&& value) {
        // Keep a stable value across retries. Moving occurs only after a slot
        // is successfully claimed, so a capacity rejection does not consume it.
        for (;;) {
            uint64_t sequence = 0;
            if (try_publish(std::forward<U>(value), sequence)) return sequence;
            if (next_sequence_.load(std::memory_order_relaxed) >= kMaxSequence) {
                throw std::overflow_error("snapshot sequence space is exhausted");
            }
            std::this_thread::yield();
        }
    }

    bool try_load(ReadResult& result) const {
        for (size_t attempt = 0; attempt < SlotCount; ++attempt) {
            const uint64_t current = current_.load(std::memory_order_acquire);
            const uint8_t index = unpack_index(current);
            if (index == kNoIndex) return false;

            const uint64_t sequence = unpack_sequence(current);
            Slot& slot = slots_[index];
            uint64_t state = slot.state.load(std::memory_order_acquire);
            if ((state & kReadyBit) == 0 || (state & kReaderMask) == kReaderMask ||
                !slot.state.compare_exchange_strong(
                    state, state + 1,
                    std::memory_order_acquire,
                    std::memory_order_relaxed)) {
                continue;
            }

            ReaderPin pin(slot.state);
            if (current_.load(std::memory_order_acquire) != current ||
                slot.sequence != sequence) {
                continue;
            }
            result.value.emplace(*slot.value);
            result.sequence = sequence;
            result.timestamp = slot.timestamp;
            return true;
        }
        return false;
    }

    bool has_value() const noexcept {
        return unpack_index(current_.load(std::memory_order_acquire)) != kNoIndex;
    }

    uint64_t sequence() const noexcept {
        return unpack_sequence(current_.load(std::memory_order_acquire));
    }

    bool is_synchronization_lock_free() const noexcept {
        if (!current_.is_lock_free() || !next_sequence_.is_lock_free() ||
            !claim_hint_.is_lock_free()) {
            return false;
        }
        for (const Slot& slot : slots_) {
            if (!slot.state.is_lock_free()) return false;
        }
        return true;
    }

private:
    static constexpr uint64_t kWritingBit = uint64_t{1} << 63U;
    static constexpr uint64_t kReadyBit = uint64_t{1} << 62U;
    static constexpr uint64_t kReaderMask = kReadyBit - 1U;
    static constexpr uint64_t kFree = 0;
    static constexpr uint8_t kNoIndex = 0xffU;
    static constexpr size_t kNoSlot = std::numeric_limits<size_t>::max();
    static constexpr uint64_t kMaxSequence = (uint64_t{1} << 56U) - 1U;

    struct Slot {
        mutable std::atomic<uint64_t> state{kFree};
        std::optional<T> value;
        uint64_t sequence = 0;
        std::chrono::steady_clock::time_point timestamp{};
    };

    class ReaderPin {
    public:
        explicit ReaderPin(std::atomic<uint64_t>& state) noexcept : state_(state) {}
        ~ReaderPin() { state_.fetch_sub(1, std::memory_order_release); }
        ReaderPin(const ReaderPin&) = delete;
        ReaderPin& operator=(const ReaderPin&) = delete;

    private:
        std::atomic<uint64_t>& state_;
    };

    static uint64_t pack(uint64_t sequence, uint8_t index) noexcept {
        return sequence << 8U | index;
    }

    static uint8_t unpack_index(uint64_t value) noexcept {
        return static_cast<uint8_t>(value & 0xffU);
    }

    static uint64_t unpack_sequence(uint64_t value) noexcept {
        return value >> 8U;
    }

    void require_lock_free() const {
        if (!is_synchronization_lock_free()) {
            throw std::runtime_error(
                "snapshot store requires lock-free 64-bit atomics");
        }
    }

    size_t claim_slot() {
        const size_t start = static_cast<size_t>(
            claim_hint_.fetch_add(1, std::memory_order_relaxed) % SlotCount);
        for (size_t attempt = 0; attempt < SlotCount; ++attempt) {
            const size_t index = (start + attempt) % SlotCount;
            Slot& slot = slots_[index];
            uint64_t state = slot.state.load(std::memory_order_acquire);

            if (state == kFree) {
                if (slot.state.compare_exchange_strong(
                        state, kWritingBit,
                        std::memory_order_acquire,
                        std::memory_order_relaxed)) {
                    return index;
                }
                continue;
            }
            if (state != kReadyBit ||
                !slot.state.compare_exchange_strong(
                    state, kWritingBit,
                    std::memory_order_acquire,
                    std::memory_order_relaxed)) {
                continue;
            }

            // Only inspect ordinary slot fields after taking exclusive writer
            // ownership. Readers cannot pin a Writing slot.
            const uint64_t current = current_.load(std::memory_order_acquire);
            const bool is_current =
                unpack_index(current) == index &&
                unpack_sequence(current) == slot.sequence;
            const bool candidate_still_pending =
                unpack_sequence(current) < slot.sequence;
            if (is_current || candidate_still_pending) {
                slot.state.store(kReadyBit, std::memory_order_release);
                continue;
            }

            slot.value.reset();
            return index;
        }
        return kNoSlot;
    }

    uint64_t reserve_sequence() noexcept {
        uint64_t current = next_sequence_.load(std::memory_order_relaxed);
        for (;;) {
            if (current >= kMaxSequence) return 0;
            if (next_sequence_.compare_exchange_strong(
                    current, current + 1,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
                return current + 1;
            }
        }
    }

    void publish_candidate(size_t index, uint64_t sequence) noexcept {
        uint64_t current = current_.load(std::memory_order_acquire);
        const uint64_t candidate = pack(sequence, static_cast<uint8_t>(index));
        while (unpack_sequence(current) < sequence &&
               !current_.compare_exchange_strong(
                   current, candidate,
                   std::memory_order_release,
                   std::memory_order_acquire)) {
            // A failed strong CAS means another publisher advanced current_.
        }
    }

    mutable Slot slots_[SlotCount];
    alignas(64) std::atomic<uint64_t> current_{pack(0, kNoIndex)};
    std::atomic<uint64_t> next_sequence_{0};
    std::atomic<uint64_t> claim_hint_{0};
};

} // namespace kairo::comm::detail
