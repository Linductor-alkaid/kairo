// Lock-free queue implementation.
#pragma once

#include <atomic>
#include <cstddef>
#include <limits>
#include <vector>
#include <type_traits>
#include <memory>
#include <stdexcept>
#include <thread>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <emmintrin.h>
#define PAUSE_INSTRUCTION() _mm_pause()
#elif defined(__aarch64__) || defined(__arm__)
#define PAUSE_INSTRUCTION() __asm__ volatile("yield" ::: "memory")
#else
#define PAUSE_INSTRUCTION() do {} while(0)
#endif

namespace kairo {
namespace util {

enum class LockFreeQueueFailReason : uint8_t {
    None,
    QueueFull,
    Contention,
    ReservationCancelled,
};

/**
 * @brief 无锁队列性能统计
 */
struct LockFreeQueueStats {
    uint64_t total_pushes = 0;
    // All failed underlying push attempts. In a quiescent snapshot, this
    // equals the sum of the three per-reason rejection counters below.
    uint64_t failed_pushes = 0;
    uint64_t queue_full_rejections = 0;
    uint64_t contention_rejection = 0;
    uint64_t reservation_cancelled_rejections = 0;
    uint64_t total_pops = 0;
    uint64_t empty_pops = 0;
    uint64_t batch_pushes = 0;
    uint64_t batch_pops = 0;
    uint64_t current_size = 0;
    uint64_t peak_size = 0;
    // Currently stalled reservations. This is an instantaneous sample.
    uint64_t reserved_count = 0;
    // Cumulative successful slot reservations. Together with total_pushes and
    // cancelled_reservation_count, this supports reservation conservation.
    uint64_t reservation_count = 0;
    uint64_t ready_count = 0;
    uint64_t cancelled_reservation_count = 0;
    // The configured number of consumer yields before cancellation recovery.
    uint64_t reservation_wait_yields = 0;
    // Classification of the most recent producer push failure.
    LockFreeQueueFailReason fail_reason = LockFreeQueueFailReason::None;
};

/**
 * @brief 无锁队列（MPMC - 多生产者多消费者）
 *
 * 使用序列号跟踪每个槽位状态，保证线程安全。生产侧经 enqueue_pos_ CAS
 * 预留槽位；消费侧（PA-7 起）经 dequeue_pos_ CAS 认领前沿，pop()/
 * pop_batch() 可被多线程并发调用，恰好一次交付。
 *
 * @tparam T 队列元素类型，必须是可平凡复制的（trivially copyable）
 */
template<typename T>
class LockFreeQueue {
    static_assert(std::is_trivially_copyable_v<T>,
                  "LockFreeQueue requires trivially copyable type");

public:
    // Cap multiplier growth so the largest internal backoff window
    // (16 * kMaxBackoffMultiplier) remains bounded and cannot wrap size_t.
    static constexpr size_t kMaxBackoffMultiplier = 1u << 20;
    static constexpr size_t kDefaultReservationWaitYields = 64;

    explicit LockFreeQueue(size_t capacity, size_t backoff_multiplier = 1, bool enable_stats = false,
                           size_t reservation_wait_yields = kDefaultReservationWaitYields)
        : capacity_(round_to_power_of_two(capacity))
        , mask_(capacity_ - 1)
        , buffer_(capacity_)
        , sequences_(capacity_)
        , states_(capacity_)
        , enqueue_pos_(0)
        , dequeue_pos_(0)
        , reserved_approx_(0)
        , ready_approx_(0)
        , backoff_multiplier_(normalize_backoff_multiplier(backoff_multiplier))
        , reservation_wait_yields_(reservation_wait_yields)
        , stats_enabled_(enable_stats) {
        // 初始化序列号
        for (size_t i = 0; i < capacity_; ++i) {
            sequences_[i].store(i, std::memory_order_relaxed);
            states_[i].store(state_tag(i, SlotState::Free), std::memory_order_relaxed);
        }
    }

    bool push(const T& item) {
        size_t pos;
        size_t backoff = 1;
        constexpr size_t MAX_BACKOFF = 16;
        constexpr int MAX_RETRIES = 64;

        for (int retry = 0; retry < MAX_RETRIES; ++retry) {
            pos = enqueue_pos_.load(std::memory_order_acquire);

            // 检查队列是否已满（保留一个空槽位）
            size_t deq = dequeue_pos_.load(std::memory_order_acquire);
            size_t in_flight = (pos >= deq) ? (pos - deq) : 0;
            if (in_flight >= capacity_ - 1) {
                record_push_failure(LockFreeQueueFailReason::QueueFull);
                return false;
            }

            size_t index = pos & mask_;
            size_t seq = sequences_[index].load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
            SlotState state = slot_state(index);

            if (diff == 0 && slot_can_be_reserved(state)) {
                // 槽位可用，尝试预留
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    if (!begin_write(index, pos)) {
                        record_push_failure(reservation_failure_reason(index, pos));
                        return false;
                    }
                    buffer_[index] = item;
                    sequences_[index].store(pos + 1, std::memory_order_release);
                    reserved_approx_.fetch_sub(1, std::memory_order_relaxed);
                    ready_approx_.fetch_add(1, std::memory_order_relaxed);
                    states_[index].store(state_tag(pos, SlotState::Published),
                                         std::memory_order_release);
                    // 260610P013: relaxed load
                    if (stats_enabled_.load(std::memory_order_relaxed)) {
                        stats_.total_pushes.fetch_add(1, std::memory_order_relaxed);
                        update_peak_size();
                    }
                    return true;
                }
                // CAS 失败，指数退避（应用退避倍数）
                size_t scaled_backoff = backoff * backoff_multiplier_;
                for (size_t i = 0; i < scaled_backoff; ++i) {
                    PAUSE_INSTRUCTION();
                }
                backoff = backoff < MAX_BACKOFF ? backoff * 2 : MAX_BACKOFF;
            } else if (diff < 0) {
                record_push_failure(LockFreeQueueFailReason::QueueFull);
                return false;
            }
            // diff > 0: 其他线程正在操作，重试
        }
        record_push_failure(LockFreeQueueFailReason::Contention);
        return false;
    }

    bool pop(T& item) {
        return pop_impl(&item, 1, false) == 1;
    }

    /**
     * @brief 批量入队。
     *
     * 当 count > 0 时，items 必须指向至少 count 个元素；空指针视为失败。
     */
    bool push_batch(const T* items, size_t count, size_t& pushed) {
        pushed = 0;
        if (count == 0) return true;
        if (items == nullptr) return false;

        for (; pushed < count; ++pushed) {
            if (!push(items[pushed])) break;
        }
        if (pushed == 0) return false;
        if (stats_enabled_.load(std::memory_order_relaxed)) {
            stats_.batch_pushes.fetch_add(1, std::memory_order_relaxed);
        }
        return true;
    }

    /**
     * @brief 精确批量入队，只有全部 count 个元素可写入时才成功。
     *
     * 当 count > 0 时，items 必须指向至少 count 个元素；空指针视为失败。
     */
    bool push_batch_exact(const T* items, size_t count) {
        if (count == 0) return true;
        if (items == nullptr) return false;
        size_t backoff = 1;
        constexpr size_t kMaxBackoff = 16;
        for (int retry = 0; retry < 64; ++retry) {
            size_t pos = enqueue_pos_.load(std::memory_order_acquire);
            size_t deq = dequeue_pos_.load(std::memory_order_acquire);
            if (pos < deq || pos - deq > capacity_ - 1 || count > capacity_ - 1 - (pos - deq)) {
                record_push_failure(LockFreeQueueFailReason::QueueFull);
                return false;
            }
            bool available = true;
            for (size_t i = 0; i < count; ++i) {
                const size_t index = (pos + i) & mask_;
                if (sequences_[index].load(std::memory_order_acquire) != pos + i ||
                    !slot_can_be_reserved(slot_state(index))) {
                    available = false;
                    break;
                }
            }
            if (available && enqueue_pos_.compare_exchange_weak(pos, pos + count, std::memory_order_relaxed)) {
                // Reserve in REVERSE order so the frontier slot (pos) is the
                // last one to leave Free and is immediately followed by its
                // begin_batch_write.  A consumer that observes the frontier
                // Reserved and applies the stalled-producer cancellation
                // heuristic (cancel_reservation) would otherwise spin its
                // bounded yield budget while the producer legitimately
                // reserves the other count-1 slots — under TSAN/sanitizer
                // slowdowns that budget expires mid-loop and the batch is
                // falsely killed as "stalled" (issue #187).  Reserving tail
                // first keeps the frontier's cancellable window O(1).
                for (size_t i = count; i > 0; --i) {
                    const size_t slot = i - 1;
                    const size_t index = (pos + slot) & mask_;
                    // Reserve the complete claimed range before any slot can
                    // enter the cancellable debug-hook window.  A consumer
                    // may then cancel stalled slots, but it can never advance
                    // into an unreserved Free slot in this batch.
                    if (!reserve_slot(index, pos + slot)) {
                        record_push_failure(reservation_failure_reason(index, pos + slot));
                        cancel_batch_reservations(pos, count);
                        return false;
                    }
                }
                for (size_t i = 0; i < count; ++i) {
                    const size_t index = (pos + i) & mask_;
                    if (!begin_batch_write(index, pos + i)) {
                        record_push_failure(reservation_failure_reason(index, pos + i));
                        cancel_batch_reservations(pos, count);
                        return false;
                    }
                    buffer_[index] = items[i];
                    sequences_[index].store(pos + i + 1, std::memory_order_release);
                }
                // A BatchWriting slot is deliberately invisible to consumers.
                // Only after every payload is complete can this batch become
                // observable, which makes a false return all-or-nothing.
                for (size_t i = 0; i < count; ++i) {
                    const size_t index = (pos + i) & mask_;
                    reserved_approx_.fetch_sub(1, std::memory_order_relaxed);
                    ready_approx_.fetch_add(1, std::memory_order_relaxed);
                    states_[index].store(state_tag(pos + i, SlotState::Published),
                                         std::memory_order_release);
                }
                if (stats_enabled_.load(std::memory_order_relaxed)) {
                    stats_.total_pushes.fetch_add(count, std::memory_order_relaxed);
                    stats_.batch_pushes.fetch_add(1, std::memory_order_relaxed);
                    update_peak_size();
                }
                return true;
            }
            for (size_t i = 0; i < backoff * backoff_multiplier_; ++i) PAUSE_INSTRUCTION();
            backoff = backoff < kMaxBackoff ? backoff * 2 : kMaxBackoff;
        }
        record_push_failure(LockFreeQueueFailReason::Contention);
        return false;
    }

    /**
     * @brief 批量出队。
     *
     * 当 max_count > 0 时，items 必须指向至少 max_count 个元素；空指针返回 0。
     */
    size_t pop_batch(T* items, size_t max_count) {
        if (max_count > 0 && items == nullptr) return 0;
        return pop_impl(items, max_count, true);
    }

    /**
     * @brief 判断队列是否为空（消费者就绪检查，近似值）
     *
     * 实现基于 sequences_ 视角：检查下一个待消费槽位的序列号。
     * 在 push 完成后（sequences_[i] 已 release store 新值）且该槽位尚未被 pop 消费时，
     * 此方法返回 false；在 pop 完成（sequences_[i] 被回填为 pos+capacity）后返回 true。
     *
     * **与 size() 的语义差异（260611P007 文档化）**：
     *   - `empty()` 是消费者就绪检查（用于 pop 前判空），基于 sequences_ 视角
     *   - `size()`  是容量统计（基于 enqueue_pos_ - dequeue_pos_ 计数）
     *   - 两者均为近似值，在并发下可能短暂给出不一致结论（empty() 为 true 时 size() 仍 > 0，
     *     或反之），不可用于精确同步。若需严格判空，应使用 `pop()` 的返回值而非 `empty()`。
     */
    bool empty() const {
        size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
        size_t index = pos & mask_;
        size_t seq = sequences_[index].load(std::memory_order_acquire);
        return seq != pos + 1;
    }

    /**
     * @brief 返回队列中当前的元素数量（近似值）
     *
     * 本方法读取 ready_approx_，它在发布元素时递增、在消费者移除元素时递减。
     * 与独立读取 enqueue_pos_ 和 dequeue_pos_ 并相减不同，单个原子快照不会
     * 因两个位置计数器的并发推进而产生暂时的负值或超过容量的值。
     *
     * 注意:此方法返回的是某瞬时快照,不是精确值,仅供监控/统计用途,
     * 不可作为容量判定依据。尚未发布的预留槽位不计入 size()；可通过
     * get_stats().reserved_count 观察。
     *
     * 在弱序架构 (ARM/POWER) 上的端到端正确性需要 CI 验证,本机 x86_64
     * TSO 模型下能掩盖此问题。
     *
     * **与 empty() 的语义差异（260611P007 文档化）**：本方法基于 ready_approx_
     * 计数，而 `empty()` 基于 sequences_（消费者就绪视角）。两者均为近似值，
     * 并发下可能短暂不一致。LockFreeTaskExecutor::pending_count() 本质上即调用本方法，
     * 故 pending_count() 也是近似值，不可用于精确同步。
     */
    size_t size() const {
        return ready_approx_.load(std::memory_order_relaxed);
    }

    size_t capacity() const {
        return capacity_;
    }

    void enable_stats(bool enable) {
        // 260610P013: relaxed store
        stats_enabled_.store(enable, std::memory_order_relaxed);
    }

    LockFreeQueueStats get_stats() const {
        // 260610P013: relaxed load
        if (!stats_enabled_.load(std::memory_order_relaxed)) return {};
        LockFreeQueueStats result;
        result.total_pushes = stats_.total_pushes.load(std::memory_order_relaxed);
        result.failed_pushes = stats_.failed_pushes.load(std::memory_order_relaxed);
        result.queue_full_rejections =
            stats_.queue_full_rejections.load(std::memory_order_relaxed);
        result.contention_rejection = stats_.contention_rejection.load(std::memory_order_relaxed);
        result.reservation_cancelled_rejections =
            stats_.reservation_cancelled_rejections.load(std::memory_order_relaxed);
        result.total_pops = stats_.total_pops.load(std::memory_order_relaxed);
        result.empty_pops = stats_.empty_pops.load(std::memory_order_relaxed);
        result.batch_pushes = stats_.batch_pushes.load(std::memory_order_relaxed);
        result.batch_pops = stats_.batch_pops.load(std::memory_order_relaxed);
        result.current_size = size();
        result.peak_size = stats_.peak_size.load(std::memory_order_relaxed);
        result.reservation_count = stats_.reservation_count.load(std::memory_order_relaxed);
        result.reserved_count = reserved_approx_.load(std::memory_order_relaxed);
        result.ready_count = ready_approx_.load(std::memory_order_relaxed);
        result.cancelled_reservation_count =
            stats_.cancelled_reservation_count.load(std::memory_order_relaxed);
        result.reservation_wait_yields = reservation_wait_yields_;
        result.fail_reason = static_cast<LockFreeQueueFailReason>(
            stats_.fail_reason.load(std::memory_order_relaxed));
        return result;
    }

    /**
     * @brief Returns a full per-slot diagnostic snapshot.
     *
     * O(capacity), do not call in hot paths. This is a non-synchronized
     * diagnostic snapshot and may observe concurrent state transitions.
     */
    [[nodiscard]] LockFreeQueueStats expensive_diagnostic_snapshot() const {
        LockFreeQueueStats result = get_stats();
        if (!stats_enabled_.load(std::memory_order_relaxed)) return result;
        result.reserved_count = 0;
        result.ready_count = 0;
        for (size_t i = 0; i < capacity_; ++i) {
            SlotState state = slot_state(i);
            if (state == SlotState::Reserved || state == SlotState::Writing) {
                ++result.reserved_count;
            } else if (state == SlotState::Published) {
                ++result.ready_count;
            }
        }
        return result;
    }

    // 260611P004: helpers for callers that perform a logical "batch"
    // operation by composing per-item push/pop (e.g. the exception-safe
    // push_tasks_batch path in LockFreeTaskExecutor). This keeps the
    // stats signal meaningful regardless of which underlying entry
    // point was used.
    void record_batch_push() {
        if (stats_enabled_.load(std::memory_order_relaxed)) {
            stats_.batch_pushes.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void record_batch_pop() {
        if (stats_enabled_.load(std::memory_order_relaxed)) {
            stats_.batch_pops.fetch_add(1, std::memory_order_relaxed);
        }
    }

private:
    enum class SlotState : uint8_t {
        Free,
        Reserved,
        Writing,
        BatchWriting,
        Published,
        Cancelled,
    };

private:

    static bool slot_can_be_reserved(SlotState state) {
        return state == SlotState::Free;
    }

    static constexpr size_t kStateBits = 3;
    static constexpr size_t kStateMask = (size_t{1} << kStateBits) - 1;

    static size_t state_tag(size_t position, SlotState state) {
        return (position << kStateBits) | static_cast<size_t>(state);
    }

    SlotState slot_state(size_t index) const {
        return static_cast<SlotState>(states_[index].load(std::memory_order_acquire) & kStateMask);
    }

    bool reserve_slot(size_t index, size_t position) {
        size_t expected = state_tag(position, SlotState::Free);
        if (!states_[index].compare_exchange_strong(expected, state_tag(position, SlotState::Reserved),
                                                    std::memory_order_acq_rel,
                                                    std::memory_order_acquire)) {
            return false;
        }
        reserved_approx_.fetch_add(1, std::memory_order_relaxed);
        if (stats_enabled_.load(std::memory_order_relaxed)) {
            stats_.reservation_count.fetch_add(1, std::memory_order_relaxed);
        }
        return true;
    }

    bool begin_write(size_t index, size_t position) {
        // A normal producer has no interruptible work between reserving a
        // slot and writing its payload.  Mark that path as Writing in the
        // reservation CAS itself so a fast consumer cannot mistake a
        // scheduled-out producer for a stalled hook and cancel a valid push.
        // The hook-enabled path intentionally remains cancellable for the
        // diagnostic/recovery contract exercised by the stalled-producer
        // tests.
        // CR-121: 无 hook 时（常态）仅一次标志位 load，不触碰 shared_ptr 原子读
        const bool hook_enabled =
            before_publish_hook_active_.load(std::memory_order_acquire);
        const SlotState initial_state =
            hook_enabled ? SlotState::Reserved : SlotState::Writing;
        size_t expected = state_tag(position, SlotState::Free);
        if (!states_[index].compare_exchange_strong(expected, state_tag(position, initial_state),
                                                    std::memory_order_acq_rel,
                                                    std::memory_order_acquire)) {
            return false;
        }
        reserved_approx_.fetch_add(1, std::memory_order_relaxed);
        if (stats_enabled_.load(std::memory_order_relaxed)) {
            stats_.reservation_count.fetch_add(1, std::memory_order_relaxed);
        }

        if (initial_state == SlotState::Writing) {
            return true;
        }

        const auto hook_state = std::atomic_load_explicit(
            &before_publish_hook_, std::memory_order_acquire);
        // hook_enabled 已确认非空；防御性判空保持与原子读的内存序一致性
        if (hook_state && hook_state->hook) {
            hook_state->hook(hook_state->context);
        }
        expected = state_tag(position, SlotState::Reserved);
        return states_[index].compare_exchange_strong(expected, state_tag(position, SlotState::Writing),
                                                       std::memory_order_acq_rel,
                                                       std::memory_order_acquire);
    }

    bool begin_batch_write(size_t index, size_t position) {
        // CR-121: 同 begin_write——常态零 shared_ptr 原子读
        if (before_publish_hook_active_.load(std::memory_order_acquire)) {
            const auto hook_state = std::atomic_load_explicit(&before_publish_hook_, std::memory_order_acquire);
            if (hook_state && hook_state->hook != nullptr) {
                hook_state->hook(hook_state->context);
            }
        }
        size_t expected = state_tag(position, SlotState::Reserved);
        return states_[index].compare_exchange_strong(expected, state_tag(position, SlotState::BatchWriting),
                                                       std::memory_order_acq_rel,
                                                       std::memory_order_acquire);
    }

    void cancel_batch_reservations(size_t position, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            const size_t slot_position = position + i;
            const size_t index = slot_position & mask_;
            size_t expected = state_tag(slot_position, SlotState::Reserved);
            if (!states_[index].compare_exchange_strong(expected, state_tag(slot_position, SlotState::Cancelled),
                                                        std::memory_order_acq_rel,
                                                        std::memory_order_acquire)) {
                expected = state_tag(slot_position, SlotState::BatchWriting);
                if (!states_[index].compare_exchange_strong(expected, state_tag(slot_position, SlotState::Cancelled),
                                                            std::memory_order_acq_rel,
                                                            std::memory_order_acquire)) {
                    continue;
                }
            }
            reserved_approx_.fetch_sub(1, std::memory_order_relaxed);
            if (stats_enabled_.load(std::memory_order_relaxed)) {
                stats_.cancelled_reservation_count.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    LockFreeQueueFailReason reservation_failure_reason(size_t index, size_t pos) const {
        return sequences_[index].load(std::memory_order_acquire) != pos ||
                slot_state(index) == SlotState::Cancelled
            ? LockFreeQueueFailReason::ReservationCancelled
            : LockFreeQueueFailReason::Contention;
    }

    // claimed_work: the number of slots the owning producer claimed in one
    // enqueue_pos_ CAS (1 for single push, the batch size for push_batch_exact).
    // A batch producer legitimately holds every claimed slot in Reserved while
    // it advances through the reserve/write loops; the stall budget must scale
    // with that work or a healthy producer preempted on a loaded/sanitized
    // host is falsely cancelled mid-batch (issue #187 residual, seen on
    // 2-vCPU gcc-11 Release TSAN runners). The multiplier is capped so a
    // genuinely stalled producer with a huge claim still recovers in bounded
    // time (64 * 1024 yields worst case, sub-second).
    bool cancel_reservation(size_t index, size_t position, size_t claimed_work = 1) {
        static constexpr size_t kMaxClaimMultiplier = 1024;
        const size_t stall_yields =
            reservation_wait_yields_ * std::min(claimed_work, kMaxClaimMultiplier);
        for (size_t spin = 0; spin < stall_yields; ++spin) {
            SlotState state = slot_state(index);
            if (state == SlotState::Published) return false;
            if (state == SlotState::Writing) return false;
            // A BatchWriting slot belongs to a batch producer that already
            // passed its cancellable hook window: the write itself is
            // non-interruptible, so the owner is progressing by definition.
            // Without this escape, a scan-ahead cancellation aimed at a
            // mid-batch slot spins the full budget while the producer
            // legitimately advances toward that slot, then kills the batch
            // (issue #187).  A genuinely stalled batch producer blocks in
            // its hook BEFORE the BatchWriting CAS, staying Reserved and
            // therefore still cancellable.
            if (state == SlotState::BatchWriting) return false;
            // A reserved slot normally belongs to a producer that is ready to
            // publish.  Pure CPU pauses can repeatedly run the consumer on a
            // saturated or single-core host and cancel that valid producer
            // before it receives a time slice.  Yielding keeps the recovery
            // path for genuinely stalled producers without rejecting normal
            // submissions due to scheduler timing.
            std::this_thread::yield();
            PAUSE_INSTRUCTION();
        }
        size_t expected = state_tag(position, SlotState::Reserved);
        while (expected == state_tag(position, SlotState::Reserved)) {
            if (states_[index].compare_exchange_weak(expected, state_tag(position, SlotState::Cancelled),
                                                     std::memory_order_acq_rel,
                                                     std::memory_order_acquire)) {
                reserved_approx_.fetch_sub(1, std::memory_order_relaxed);
                if (stats_enabled_.load(std::memory_order_relaxed)) {
                    stats_.cancelled_reservation_count.fetch_add(1, std::memory_order_relaxed);
                }
                return true;
            }
        }
        return false;
    }

    // PA-7: multi-consumer consume side. The queue used to advance
    // dequeue_pos_ with a blind load/store, which restricted pop() to a
    // single consumer (LockFreeWorkerQueue serialized owner pop and thief
    // steal behind a mutex). Consumers now CLAIM the frontier with a CAS:
    // exactly one consumer can move dequeue_pos_ from pos to pos + run, so
    // pop()/pop_batch() are safe to call concurrently and the mutex is gone.
    //
    // Claim protocol: verify the longest run of consecutively Published
    // slots starting at the frontier, claim the whole run with ONE CAS, then
    // consume it. Published slots never regress to a non-consumable state
    // except by a consumer (producers only move slots toward Published), so
    // a verified run stays valid across the CAS. Losing consumers simply
    // reload the frontier. Producers cannot enter a claimed slot for the
    // next lap until the claimant's sequence/state release stores land
    // (reservation requires sequences_[i] == next-lap position).
    size_t pop_impl(T* items, size_t max_count, bool batch) {
        size_t popped = 0;
        const size_t enqueued = enqueue_pos_.load(std::memory_order_acquire);

        while (popped < max_count) {
            size_t pos = dequeue_pos_.load(std::memory_order_acquire);
            if (pos >= enqueued) break;
            const size_t index = pos & mask_;
            SlotState state = slot_state(index);
            if (state == SlotState::Published &&
                sequences_[index].load(std::memory_order_acquire) == pos + 1) {
                const size_t run_limit =
                    std::min(max_count - popped, enqueued - pos);
                size_t run = 1;
                while (run < run_limit) {
                    const size_t run_index = (pos + run) & mask_;
                    if (slot_state(run_index) != SlotState::Published ||
                        sequences_[run_index].load(std::memory_order_acquire) !=
                            pos + run + 1) {
                        break;
                    }
                    ++run;
                }
                if (!dequeue_pos_.compare_exchange_weak(
                        pos, pos + run,
                        std::memory_order_acq_rel, std::memory_order_acquire)) {
                    continue;  // frontier moved under us; reload and retry
                }
                // The claimed range [pos, pos + run) is exclusively ours:
                // other consumers can no longer reach these positions.
                for (size_t k = 0; k < run; ++k) {
                    const size_t claim_index = (pos + k) & mask_;
                    items[popped++] = buffer_[claim_index];
                    sequences_[claim_index].store(pos + k + capacity_,
                                                  std::memory_order_release);
                    states_[claim_index].store(
                        state_tag(pos + k + capacity_, SlotState::Free),
                        std::memory_order_release);
                    ready_approx_.fetch_sub(1, std::memory_order_relaxed);
                }
                continue;
            }
            if (state == SlotState::Reserved) {
                // claimed_work = enqueued - pos: the frontier producer claimed
                // the whole [pos, enqueued) range with one CAS (see
                // cancel_reservation for why the stall budget scales with it).
                if (cancel_reservation(index, pos, static_cast<size_t>(enqueued - pos))) {
                    // Whoever wins the frontier CAS performs the cleanup;
                    // cancel_reservation itself is CAS-guarded, so at most one
                    // consumer reaches this branch with a cancelled slot.
                    if (dequeue_pos_.compare_exchange_weak(
                            pos, pos + 1,
                            std::memory_order_acq_rel, std::memory_order_acquire)) {
                        sequences_[index].store(pos + capacity_,
                                                std::memory_order_release);
                        states_[index].store(
                            state_tag(pos + capacity_, SlotState::Free),
                            std::memory_order_release);
                    }
                    continue;
                }
                state = slot_state(index);
                if (state == SlotState::Published) continue;
            }
            if (state == SlotState::BatchWriting) {
                // A batch has written an earlier payload but has not committed
                // the range. Look ahead for a cancellable reservation so a
                // producer blocked in a later before-publish hook can fail
                // without exposing this prefix.
                for (size_t scan = pos + 1; scan < enqueued; ++scan) {
                    const size_t scan_index = scan & mask_;
                    if (slot_state(scan_index) == SlotState::Reserved) {
                        (void)cancel_reservation(scan_index, scan,
                                                 static_cast<size_t>(enqueued - pos));
                        break;
                    }
                }
                break;
            }
            if (state == SlotState::Cancelled) {
                if (dequeue_pos_.compare_exchange_weak(
                        pos, pos + 1,
                        std::memory_order_acq_rel, std::memory_order_acquire)) {
                    sequences_[index].store(pos + capacity_,
                                            std::memory_order_release);
                    states_[index].store(state_tag(pos + capacity_, SlotState::Free),
                                         std::memory_order_release);
                }
                continue;
            }
            if (state == SlotState::Free) break;
            // A producer has begun the non-interruptible data-write window.
            break;
        }
        if (popped > 0) {
            if (stats_enabled_.load(std::memory_order_relaxed)) {
                stats_.total_pops.fetch_add(popped, std::memory_order_relaxed);
                if (batch) stats_.batch_pops.fetch_add(1, std::memory_order_relaxed);
            }
        } else if (stats_enabled_.load(std::memory_order_relaxed)) {
            stats_.empty_pops.fetch_add(1, std::memory_order_relaxed);
        }
        return popped;
    }

    void record_push_failure(LockFreeQueueFailReason reason) {
        if (stats_enabled_.load(std::memory_order_relaxed)) {
            stats_.failed_pushes.fetch_add(1, std::memory_order_relaxed);
            switch (reason) {
            case LockFreeQueueFailReason::QueueFull:
                stats_.queue_full_rejections.fetch_add(1, std::memory_order_relaxed);
                break;
            case LockFreeQueueFailReason::Contention:
                stats_.contention_rejection.fetch_add(1, std::memory_order_relaxed);
                break;
            case LockFreeQueueFailReason::ReservationCancelled:
                stats_.reservation_cancelled_rejections.fetch_add(1,
                                                                   std::memory_order_relaxed);
                break;
            case LockFreeQueueFailReason::None:
                break;
            default:
                break;
            }
            stats_.fail_reason.store(static_cast<uint8_t>(reason), std::memory_order_relaxed);
        }
    }

public:
    using BeforePublishHook = void (*)(void*);

    // The hook and its context are published together. A producer observes
    // either the previous complete state or the replacement complete state.
    // The caller must keep the context valid until a later call has cleared
    // or replaced this hook and concurrent producers have stopped.
    void set_before_publish_hook(BeforePublishHook hook, void* context) {
        std::atomic_store_explicit(
            &before_publish_hook_,
            std::make_shared<const BeforePublishHookState>(BeforePublishHookState{hook, context}),
            std::memory_order_release);
        // CR-121: shared_ptr 发布后再置位（release），热路径读 false 时可安全
        // 跳过 shared_ptr 原子读。
        before_publish_hook_active_.store(hook != nullptr, std::memory_order_release);
    }
private:
    void update_peak_size() {
        size_t current = size();
        uint64_t peak = stats_.peak_size.load(std::memory_order_relaxed);
        while (current > peak) {
            if (stats_.peak_size.compare_exchange_weak(peak, current, std::memory_order_relaxed)) {
                break;
            }
        }
    }
    static size_t round_to_power_of_two(size_t n) {
        if (n < 2) {
            throw std::invalid_argument("LockFreeQueue capacity must be at least 2");
        }

        constexpr size_t max_power_of_two = (std::numeric_limits<size_t>::max() / 2) + 1;
        if (n > max_power_of_two) {
            throw std::invalid_argument("LockFreeQueue capacity is too large to round to a power of two");
        }

        size_t power = 1;
        while (power < n) power <<= 1;

        if (power > std::vector<T>().max_size() ||
            power > std::vector<std::atomic<size_t>>().max_size() ||
            power > std::vector<std::atomic<size_t>>().max_size()) {
            throw std::invalid_argument("LockFreeQueue capacity is too large to allocate");
        }

        return power;
    }

    static size_t normalize_backoff_multiplier(size_t multiplier) {
        if (multiplier == 0) {
            throw std::invalid_argument("LockFreeQueue: backoff_multiplier must be > 0");
        }
        return multiplier > kMaxBackoffMultiplier ? kMaxBackoffMultiplier : multiplier;
    }

    const size_t capacity_;
    const size_t mask_;
    std::vector<T> buffer_;
    std::vector<std::atomic<size_t>> sequences_;
    // State tags include the absolute position so a delayed producer cannot
    // transition a later reuse of the same ring slot after cancellation.
    std::vector<std::atomic<size_t>> states_;
    alignas(64) std::atomic<size_t> enqueue_pos_;
    alignas(64) std::atomic<size_t> dequeue_pos_;
    // Approximate slot-state counters maintained on every transition.
    alignas(64) std::atomic<size_t> reserved_approx_;
    alignas(64) std::atomic<size_t> ready_approx_;
    // Valid range: [1, kMaxBackoffMultiplier]. Constructor rejects 0 and
    // clamps larger values to keep scaled pause-loop arithmetic bounded.
    const size_t backoff_multiplier_;
    // A bounded consumer wait keeps recovery from an indefinitely stalled
    // producer explicit and observable through get_stats().
    const size_t reservation_wait_yields_;
    // 260610P013: std::atomic<bool> 替换裸 bool 字段
    // enable_stats() 可从任意线程写入,热路径 (push/pop) 频繁读取 — C++ data race (UB)
    // relaxed ordering: 统计开关不与其它内存构成 happens-before 关系
    std::atomic<bool> stats_enabled_;
    struct BeforePublishHookState {
        BeforePublishHook hook;
        void* context;
    };
    std::shared_ptr<const BeforePublishHookState> before_publish_hook_;
    // CR-121: 热路径快速开关——begin_write/begin_batch_write 每次推送原本都要
    // 原子读 shared_ptr（旧 libstdc++ 走内部锁池）。标志位由 set_before_publish_hook
    // 在 shared_ptr 发布后以 release 写入；读到 false 的推送按无 hook 推进
    // （hook 激活对并发在途推送是最终可见语义，与既有 set 时机契约一致）。
    std::atomic<bool> before_publish_hook_active_{false};

    struct Stats {
        alignas(64) std::atomic<uint64_t> total_pushes{0};
        alignas(64) std::atomic<uint64_t> failed_pushes{0};
        alignas(64) std::atomic<uint64_t> queue_full_rejections{0};
        alignas(64) std::atomic<uint64_t> total_pops{0};
        alignas(64) std::atomic<uint64_t> empty_pops{0};
        alignas(64) std::atomic<uint64_t> batch_pushes{0};
        alignas(64) std::atomic<uint64_t> batch_pops{0};
        alignas(64) std::atomic<uint64_t> peak_size{0};
        alignas(64) std::atomic<uint64_t> contention_rejection{0};
        alignas(64) std::atomic<uint64_t> reservation_cancelled_rejections{0};
        alignas(64) std::atomic<uint64_t> reservation_count{0};
        alignas(64) std::atomic<uint64_t> cancelled_reservation_count{0};
        alignas(64) std::atomic<uint8_t> fail_reason{
            static_cast<uint8_t>(LockFreeQueueFailReason::None)};
    };
    Stats stats_;
};

} // namespace util
} // namespace kairo
