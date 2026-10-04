#pragma once

#include <kairo/comm/lockfree_core.hpp>
#include <kairo/comm/types.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <thread>
#include <string>
#include <type_traits>
#include <utility>

namespace kairo::comm {

/**
 * A preallocated MPSC queue with one logical consumer.
 *
 * Producers construct an item in a privately claimed node before publishing
 * the node to an atomic stack. The consumer detaches and reverses complete
 * batches, preserving FIFO publication order without exposing a reserved but
 * unfinished head slot. Queue storage is fully allocated during construction;
 * payload operations and configured event callbacks remain caller-controlled.
 */
template <class T>
class BoundedQueue {
public:
    struct Item {
        T value;
        std::chrono::steady_clock::time_point enqueued_at;
    };

    BoundedQueue(size_t capacity,
                 DropPolicy drop_policy,
                 bool enable_stats,
                 std::string component_name,
                 std::string event_prefix)
        : capacity_(validate_capacity(capacity)),
          drop_policy_(drop_policy),
          component_name_(std::move(component_name)),
          event_prefix_(std::move(event_prefix)),
          nodes_(std::make_unique<Node[]>(capacity_)),
          stats_(enable_stats) {
        if (!is_synchronization_lock_free()) {
            throw std::runtime_error(
                "communication queue requires lock-free synchronization atomics; "
                "payload operations and event callbacks are outside this guarantee");
        }
    }

    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;

    ~BoundedQueue() {
        for (size_t index = 0; index < capacity_; ++index) {
            if (nodes_[index].state.load(std::memory_order_relaxed) == kReady) {
                nodes_[index].item()->~Item();
            }
        }
    }

    template <class U>
    bool enqueue(U&& value,
                 std::optional<CommEvent>& event,
                 bool record_full_rejection = true) {
        const EnqueueAdmission admission = begin_enqueue();
        if (admission != EnqueueAdmission::Admitted) {
            if (admission == EnqueueAdmission::Closed) {
                record_closed_send(event);
            } else if (record_full_rejection) {
                record_drop(event);
            }
            return false;
        }
        EnqueueLease lease(*this);

        Node* node = acquire_node();
        bool displaced = false;
        // CR-040: KeepLatest/DropOldest 的置换路径与在途消费者（ConsumerLease
        // 占用）及并发生产者（promotion 要求 active==1）存在固有竞争窗口。
        // 此前任一步失败即丢弃"新值"——与策略承诺（保最新/弃最旧）正好相反。
        // 改为有界自旋重试：消费/认领窗口只有几条指令，竞争在几十次让步内
        // 必然化解；重试耗尽才按满队丢弃（此时为 best-effort 降级，头文件
        // 契约注释与 channels.md 已说明）。EnqueueLease 的 promotion/consumer
        // 认领是粘性的，重试不会重复登记也不会泄漏。
        if (node == nullptr && drop_policy_ != DropPolicy::RejectNewest) {
            constexpr int kDisplaceRetryAttempts = 64;
            for (int attempt = 0; attempt < kDisplaceRetryAttempts; ++attempt) {
                if (drop_policy_ == DropPolicy::KeepLatest &&
                    !lease.try_promote_to_replacement()) {
                    std::this_thread::yield();
                    continue;
                }
                if (!lease.try_acquire_consumer()) {
                    std::this_thread::yield();
                    continue;
                }

                // A consumer may have released capacity after the first scan.
                node = acquire_node();
                if (node == nullptr) {
                    node = drop_policy_ == DropPolicy::KeepLatest
                               ? recycle_all_for_write()
                               : recycle_oldest_for_write();
                    displaced = node != nullptr;
                }
                if (node != nullptr) {
                    break;
                }
                std::this_thread::yield();
            }
        }

        if (node == nullptr) {
            if (record_full_rejection) record_drop(event);
            return false;
        }

        try {
            ::new (static_cast<void*>(node->storage))
                Item{std::forward<U>(value), std::chrono::steady_clock::now()};
        } catch (...) {
            node->state.store(kFree, std::memory_order_release);
            throw;
        }

        const uint64_t depth = depth_.fetch_add(1, std::memory_order_relaxed) + 1;
        node->state.store(kReady, std::memory_order_release);
        publish_node(node);
        stats_.record_send(depth);
        if (displaced) {
            if (drop_policy_ == DropPolicy::KeepLatest) {
                record_overwrite(event);
            } else {
                record_drop(event);
            }
        }
        return true;
    }

    std::optional<Item> try_pop() {
        ConsumerLease consumer(*this);
        if (!consumer) return std::nullopt;

        Node* node = pop_node();
        if (node == nullptr) return std::nullopt;

        Item* source = node->item();
        const auto enqueued_at = source->enqueued_at;
        std::optional<Item> result;
        try {
            result.emplace(std::move(*source));
        } catch (...) {
            release_node(node);
            stats_.record_receive(elapsed(enqueued_at));
            throw;
        }
        release_node(node);
        stats_.record_receive(elapsed(enqueued_at));
        return result;
    }

    void close() noexcept {
        lifecycle_.fetch_or(kClosedBit, std::memory_order_acq_rel);
    }

    bool is_closed() const noexcept {
        return (lifecycle_.load(std::memory_order_acquire) & kClosedBit) != 0;
    }

    bool has_active_enqueues() const noexcept {
        return (lifecycle_.load(std::memory_order_acquire) & kInFlightMask) != 0;
    }

    bool is_drained() const noexcept {
        const uint64_t before = lifecycle_.load(std::memory_order_acquire);
        if ((before & kClosedBit) == 0 || (before & kInFlightMask) != 0) {
            return false;
        }
        if (depth_.load(std::memory_order_acquire) != 0) return false;

        // No producer can enter after close. Rechecking lifecycle pairs with
        // the final producer release chain before accepting the empty depth.
        const uint64_t after = lifecycle_.load(std::memory_order_acquire);
        return (after & kClosedBit) != 0 && (after & kInFlightMask) == 0 &&
               depth_.load(std::memory_order_acquire) == 0;
    }

    bool empty() const noexcept { return size() == 0; }
    size_t size() const noexcept { return depth_.load(std::memory_order_acquire); }
    size_t capacity() const noexcept { return capacity_; }

    CommStats stats() const noexcept {
        const uint64_t depth = size();
        const uint64_t sent = stats_.sent_count();
        const CommStats base = stats_.snapshot(depth, capacity_, sent, depth);
        CommStats result = base;
        result.producer_lag = sent >= result.received_count
                                  ? sent - result.received_count
                                  : 0;
        return result;
    }

    void record_timeout(std::optional<CommEvent>& event) {
        stats_.record_timeout();
        event = make_event(CommEventKind::Timeout, " operation timed out");
    }

    void record_handler_exception(std::optional<CommEvent>& event) {
        stats_.record_handler_exception();
        event = make_event(CommEventKind::HandlerException, " handler threw");
    }

    void set_event_callback(CommEventCallback callback) {
        callback_.set(std::move(callback));
    }

    void emit(const std::optional<CommEvent>& event) const noexcept {
        callback_.emit(event);
    }

    bool has_event_callback() const noexcept { return callback_.configured(); }

    bool is_synchronization_lock_free() const noexcept {
        const bool node_state_lock_free = nodes_[0].state.is_lock_free();
        return ready_head_.is_lock_free() && lifecycle_.is_lock_free() &&
               allocation_hint_.is_lock_free() && depth_.is_lock_free() &&
               consumer_busy_.is_lock_free() && node_state_lock_free &&
               stats_.is_lock_free() && callback_.is_lock_free();
    }

    bool is_lock_free() const noexcept {
        return is_synchronization_lock_free();
    }

private:
    static constexpr uint8_t kFree = 0;
    static constexpr uint8_t kWriting = 1;
    static constexpr uint8_t kReady = 2;
    static constexpr uint64_t kClosedBit = uint64_t{1} << 63U;
    static constexpr uint64_t kReplacementBit = uint64_t{1} << 62U;
    static constexpr uint64_t kActiveMask = kReplacementBit - 1U;
    static constexpr uint64_t kInFlightMask = kReplacementBit | kActiveMask;

    enum class EnqueueAdmission {
        Admitted,
        Closed,
        Contended
    };

    struct Node {
        std::atomic<uint8_t> state{kFree};
        Node* next = nullptr;
        alignas(Item) std::byte storage[sizeof(Item)];

        Item* item() noexcept {
            return std::launder(reinterpret_cast<Item*>(storage));
        }
    };

    class EnqueueLease {
    public:
        explicit EnqueueLease(BoundedQueue& queue) noexcept : queue_(queue) {}

        ~EnqueueLease() {
            if (owns_consumer_) queue_.release_consumer();
            queue_.end_enqueue(replacement_);
        }

        bool try_promote_to_replacement() noexcept {
            if (replacement_) return true;
            replacement_ = queue_.try_promote_to_replacement();
            return replacement_;
        }

        bool try_acquire_consumer() noexcept {
            owns_consumer_ = queue_.try_claim_consumer();
            return owns_consumer_;
        }

        EnqueueLease(const EnqueueLease&) = delete;
        EnqueueLease& operator=(const EnqueueLease&) = delete;

    private:
        BoundedQueue& queue_;
        bool owns_consumer_ = false;
        bool replacement_ = false;
    };

    class ConsumerLease {
    public:
        explicit ConsumerLease(BoundedQueue& queue) noexcept : queue_(queue) {
            bool expected = false;
            acquired_ = queue_.consumer_busy_.compare_exchange_strong(
                expected, true, std::memory_order_acquire, std::memory_order_relaxed);
        }
        ~ConsumerLease() {
            if (acquired_) queue_.consumer_busy_.store(false, std::memory_order_release);
        }
        explicit operator bool() const noexcept { return acquired_; }
        ConsumerLease(const ConsumerLease&) = delete;
        ConsumerLease& operator=(const ConsumerLease&) = delete;

    private:
        BoundedQueue& queue_;
        bool acquired_ = false;
    };

    static size_t validate_capacity(size_t capacity) noexcept {
        return capacity == 0 ? 1 : capacity;
    }

    static std::chrono::nanoseconds elapsed(
        std::chrono::steady_clock::time_point start) noexcept {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start);
    }

    EnqueueAdmission begin_enqueue() noexcept {
        uint64_t state = lifecycle_.load(std::memory_order_acquire);
        while ((state & (kClosedBit | kReplacementBit)) == 0) {
            if ((state & kActiveMask) == kActiveMask) {
                return EnqueueAdmission::Contended;
            }
            if (lifecycle_.compare_exchange_weak(state, state + 1,
                                                 std::memory_order_acquire,
                                                 std::memory_order_relaxed)) {
                return EnqueueAdmission::Admitted;
            }
        }
        return (state & kClosedBit) != 0
                   ? EnqueueAdmission::Closed
                   : EnqueueAdmission::Contended;
    }

    bool try_promote_to_replacement() noexcept {
        uint64_t state = lifecycle_.load(std::memory_order_relaxed);
        if ((state & kActiveMask) != 1 || (state & kReplacementBit) != 0) {
            return false;
        }

        const uint64_t before = state;
        if (lifecycle_.compare_exchange_strong(
                state, (state - 1) | kReplacementBit,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            return true;
        }

        // close() may set its monotonic bit after this sender was admitted.
        // Preserve that bit and retry once so close cannot revoke admission.
        if ((state ^ before) == kClosedBit &&
            (state & kClosedBit) != 0 &&
            (state & kActiveMask) == 1) {
            return lifecycle_.compare_exchange_strong(
                state, (state - 1) | kReplacementBit,
                std::memory_order_acq_rel,
                std::memory_order_relaxed);
        }
        return false;
    }

    void end_enqueue(bool replacement) noexcept {
        // acq_rel RMWs form a release sequence across concurrent producers.
        // Observing the final zero in is_drained() therefore covers every
        // admitted producer that completed before it.
        if (replacement) {
            lifecycle_.fetch_and(~kReplacementBit, std::memory_order_acq_rel);
        } else {
            lifecycle_.fetch_sub(1, std::memory_order_acq_rel);
        }
    }

    bool try_claim_consumer() noexcept {
        bool expected = false;
        return consumer_busy_.compare_exchange_strong(
            expected, true, std::memory_order_acquire, std::memory_order_relaxed);
    }

    void release_consumer() noexcept {
        consumer_busy_.store(false, std::memory_order_release);
    }

    Node* acquire_node() noexcept {
        const uint64_t start = allocation_hint_.fetch_add(1, std::memory_order_relaxed);
        for (size_t attempt = 0; attempt < capacity_; ++attempt) {
            const size_t offset = static_cast<size_t>((start + attempt) % capacity_);
            uint8_t expected = kFree;
            if (nodes_[offset].state.compare_exchange_strong(
                    expected, kWriting,
                    std::memory_order_acquire,
                    std::memory_order_relaxed)) {
                return &nodes_[offset];
            }
        }
        return nullptr;
    }

    // Nodes are never freed or relocated. A reused pointer is therefore a valid
    // current head: producers only link to it and never pop through an old next.
    // Acquire on the successful RMW also carries earlier producers into the
    // release observed by the consumer batch exchange.
    void publish_node(Node* node) noexcept {
        Node* head = ready_head_.load(std::memory_order_relaxed);
        do {
            node->next = head;
        } while (!ready_head_.compare_exchange_weak(
            head, node,
            std::memory_order_acq_rel,
            std::memory_order_relaxed));
    }

    void refill_consumer_list() noexcept {
        Node* current = ready_head_.exchange(nullptr, std::memory_order_acquire);
        Node* reversed = nullptr;
        while (current != nullptr) {
            Node* next = current->next;
            current->next = reversed;
            reversed = current;
            current = next;
        }
        consumer_head_ = reversed;
    }

    Node* pop_node() noexcept {
        if (consumer_head_ == nullptr) refill_consumer_list();
        if (consumer_head_ == nullptr) return nullptr;
        Node* node = consumer_head_;
        consumer_head_ = node->next;
        return node;
    }

    void release_node(Node* node) noexcept {
        node->item()->~Item();
        depth_.fetch_sub(1, std::memory_order_relaxed);
        node->state.store(kFree, std::memory_order_release);
    }

    Node* recycle_oldest_for_write() noexcept {
        Node* node = pop_node();
        if (node == nullptr) return nullptr;

        // Keep ownership private: publishing kFree here would let another
        // producer steal the node after this send already displaced its value.
        node->item()->~Item();
        depth_.fetch_sub(1, std::memory_order_relaxed);
        node->state.store(kWriting, std::memory_order_release);
        return node;
    }

    Node* recycle_all_for_write() noexcept {
        Node* replacement = pop_node();
        if (replacement == nullptr) return nullptr;

        replacement->item()->~Item();
        depth_.fetch_sub(1, std::memory_order_relaxed);
        replacement->state.store(kWriting, std::memory_order_release);

        // Replacement admission excludes producers, so this loop visits at
        // most the fixed-capacity snapshot present at promotion time.
        for (size_t discarded = 1; discarded < capacity_; ++discarded) {
            Node* node = pop_node();
            if (node == nullptr) break;
            release_node(node);
        }
        return replacement;
    }

    void record_drop(std::optional<CommEvent>& event) {
        stats_.record_drop();
        event = make_event(CommEventKind::Dropped, " message dropped");
    }

    void record_overwrite(std::optional<CommEvent>& event) {
        stats_.record_overwrite();
        event = make_event(CommEventKind::Overwritten, " messages overwritten");
    }

    void record_closed_send(std::optional<CommEvent>& event) {
        stats_.record_closed_send();
        event = make_event(CommEventKind::ClosedSend, " send rejected after close");
    }

    std::optional<CommEvent> make_event(CommEventKind kind, const char* suffix) const {
        if (!callback_.configured()) return std::nullopt;
        return CommEvent{kind, component_name_, event_prefix_ + suffix, stats_.sent_count()};
    }

    const size_t capacity_;
    const DropPolicy drop_policy_;
    const std::string component_name_;
    const std::string event_prefix_;
    std::unique_ptr<Node[]> nodes_;

    alignas(64) std::atomic<Node*> ready_head_{nullptr};
    std::atomic<uint64_t> lifecycle_{0};
    std::atomic<uint64_t> allocation_hint_{0};
    std::atomic<uint64_t> depth_{0};

    alignas(64) std::atomic<bool> consumer_busy_{false};
    Node* consumer_head_ = nullptr;

    detail::AtomicCommStats stats_;
    detail::CallbackSlot callback_;
};

} // namespace kairo::comm
