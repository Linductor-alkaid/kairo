#pragma once

#include "stop_token.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace kairo {

/**
 * @brief 取消来源。
 *
 * 排队取消、shutdown 清理和依赖取消传播都使用同一个 `TaskCancelled` 异常，
 * 通过 reason 区分来源。取消不属于 failure 体系，不触发 failure callback。
 */
enum class TaskCancellationReason {
    Explicit,            // 显式 request_task_cancel() / TimerHandle::cancel()
    Shutdown,            // facade 定时器停止时清理未到期的 delayed 任务
    DependencyCancelled  // 依赖被取消，依赖方不再执行
};

inline const char* to_string(TaskCancellationReason reason) noexcept {
    switch (reason) {
    case TaskCancellationReason::Explicit:
        return "Explicit";
    case TaskCancellationReason::Shutdown:
        return "Shutdown";
    case TaskCancellationReason::DependencyCancelled:
        return "DependencyCancelled";
    default:
        return "Unknown";
    }
}

/**
 * @brief 取消终态异常。
 *
 * 所有被取消任务的 future 都以该异常就绪，`future.get()` 不会永久等待。
 * 该异常不进入 `FailureKind` / `ExecutorFailureStatus` 统计。
 */
class TaskCancelled : public std::runtime_error {
public:
    TaskCancelled(TaskCancellationReason reason, std::string message)
        : std::runtime_error(std::move(message))
        , reason_(reason) {}

    TaskCancellationReason reason() const noexcept {
        return reason_;
    }

private:
    TaskCancellationReason reason_;
};

/**
 * @brief 取消请求的观测结果。
 *
 * 重复或过期取消是可预期的并发结果，不是初始化类错误，因此不复用
 * `ExecutorResult`。
 */
enum class TaskCancellationResult {
    RequestedBeforeStart,       // 任务尚未开始，已按排队取消终止
    RequestedRunning,           // 任务运行中，已置位协作停止 token（首次）
    AlreadyRequested,           // 运行中取消已在此前请求过
    AlreadyCompleted,           // 任务已达终态（含已取消）
    NotFound,                   // 句柄无效或从未纳入取消 registry
    ShuttingDown                // facade 正在关闭
};

struct TaskCancellationResponse {
    TaskCancellationResult result = TaskCancellationResult::NotFound;

    /**
     * @brief 请求是否被接受（含幂等重复请求）。
     *
     * 接受不等于任务已经停止：`RequestedRunning` 只表示停止 token 已置位，
     * 任务通过协作轮询自行退出。
     */
    bool accepted() const noexcept {
        return result == TaskCancellationResult::RequestedBeforeStart ||
               result == TaskCancellationResult::RequestedRunning ||
               result == TaskCancellationResult::AlreadyRequested;
    }
};

inline const char* to_string(TaskCancellationResult result) noexcept {
    switch (result) {
    case TaskCancellationResult::RequestedBeforeStart:
        return "RequestedBeforeStart";
    case TaskCancellationResult::RequestedRunning:
        return "RequestedRunning";
    case TaskCancellationResult::AlreadyRequested:
        return "AlreadyRequested";
    case TaskCancellationResult::AlreadyCompleted:
        return "AlreadyCompleted";
    case TaskCancellationResult::NotFound:
        return "NotFound";
    case TaskCancellationResult::ShuttingDown:
        return "ShuttingDown";
    default:
        return "Unknown";
    }
}

/**
 * @brief 取消生命周期独立计数快照。
 *
 * 独立于 `ExecutorFailureStatus`：取消是正常生命周期事件，不并入
 * failure 计数。各字段只在对应事件的首次发生时递增（每个任务至多
 * 贡献一次 request_count / queued_cancelled_count / running_request_count）。
 */
struct CancellationStatus {
    uint64_t request_count = 0;                 // 首次被接受的取消请求数
    uint64_t queued_cancelled_count = 0;        // 未调用 callable 即终止的任务数
    uint64_t running_request_count = 0;         // 运行中收到停止请求的任务数
    uint64_t completed_after_request_count = 0; // 收到请求后仍正常完成的任务数
};

/**
 * @brief 每任务独立的取消控制状态（内部管道类型）。
 *
 * 公开只是因为 facade 头文件的提交闭包需要完整类型；不承诺 ABI/源码稳定。
 * scheduler、本地队列、steal、执行包装和 handle registry 之间只共享
 * `std::shared_ptr<TaskCancellationState>`，各任务副本不会复制出独立的
 * 取消标志。
 *
 * 线程协议（单一原子 phase 提供线性化点，无额外锁）：
 * - `Pending`/`Queued` --CAS--> `Running`：worker 赢得开始执行；
 * - `Pending`/`Queued` --CAS--> `Cancelled`：取消赢得排队仲裁，promise 由
 *   completion sink 立即、且只满足一次；
 * - `Pending`/`Queued` --CAS--> `TimedOut`：queued soft timeout 赢得仲裁；
 * - `Running` --CAS--> `Succeeded`/`Failed`：worker 完成时终态；
 * - 终态只能到达一次，cancel、soft timeout、提交拒绝与 worker 完成通过
 *   同一 phase 原子量仲裁，promise 不会被满足两次。
 */
class TaskCancellationState {
public:
    enum class Phase {
        Pending,
        Queued,
        Running,
        Succeeded,
        Failed,
        TimedOut,
        Rejected,
        Cancelled
    };

    TaskCancellationState() = default;

    TaskCancellationState(const TaskCancellationState&) = delete;
    TaskCancellationState& operator=(const TaskCancellationState&) = delete;

    StopSource& stop_source() noexcept {
        return stop_source_;
    }

    StopToken stop_token() const {
        return stop_source_.get_token();
    }

    Phase phase() const noexcept {
        return phase_.load(std::memory_order_acquire);
    }

    static bool is_terminal_phase(Phase phase) noexcept {
        switch (phase) {
        case Phase::Succeeded:
        case Phase::Failed:
        case Phase::TimedOut:
        case Phase::Rejected:
        case Phase::Cancelled:
            return true;
        default:
            return false;
        }
    }

    bool terminal() const noexcept {
        return is_terminal_phase(phase());
    }

    /** 运行中协作停止请求是否已发出（含排队取消路径的置位）。 */
    bool cancel_requested() const noexcept {
        return cancel_requested_.load(std::memory_order_acquire);
    }

    /** worker 声明开始执行；返回 false 表示取消或超时已先赢仲裁。 */
    bool try_begin_execution() noexcept {
        return cas_from_pending_or_queued(Phase::Running);
    }

    /** 取消方赢得排队仲裁（任务未开始即终止）。 */
    bool try_cancel_before_start() noexcept {
        return cas_from_pending_or_queued(Phase::Cancelled);
    }

    /** queued soft timeout 赢得仲裁（execute_task 跳过执行前调用）。 */
    bool try_timeout_before_start() noexcept {
        return cas_from_pending_or_queued(Phase::TimedOut);
    }

    /** 提交被拒绝（执行器停止/registry 耗尽等，任务永不运行）。 */
    bool try_reject() noexcept {
        return cas_from_pending_or_queued(Phase::Rejected);
    }

    /** worker 正常/异常完成时的终态转换（仅 Running 可达）。 */
    bool try_finish_running(Phase terminal_phase) noexcept {
        Phase expected = Phase::Running;
        return phase_.compare_exchange_strong(
            expected, terminal_phase,
            std::memory_order_acq_rel, std::memory_order_acquire);
    }

    /**
     * @brief 首次把状态标记为"已请求取消"并返回是否为首次。
     *
     * 排队取消与运行中请求都调用它；返回 true 的一方负责递增首次请求
     * 计数，重复请求保持幂等。
     */
    bool mark_cancel_requested_once() noexcept {
        bool expected = false;
        return cancel_requested_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel, std::memory_order_acquire);
    }

    /**
     * @brief 设置取消完成 sink（发布前调用一次）。
     *
     * sink 负责在取消赢得终态后立即满足 promise，使 future 不依赖 worker
     * 何时再次取到已取消的队列节点。sink 只能携带 promise 状态，不得持有
     * 用户 callable 或业务 payload。state 通过 shared_ptr 发布后不得再修改。
     */
    void set_completion_sink(std::function<void(std::exception_ptr)> sink) {
        completion_sink_ = std::move(sink);
    }

    /**
     * @brief 由赢得 Cancelled 终态的一方调用一次 sink。
     *
     * sink 自身异常被隔离（catch-all），不得破坏取消状态机。
     */
    void notify_cancelled(std::exception_ptr exception) noexcept {
        if (notified_.test_and_set(std::memory_order_acq_rel)) {
            return;
        }
        if (completion_sink_) {
            try {
                completion_sink_(std::move(exception));
            } catch (...) {
                // 取消通知不得成为新的失败源。
            }
        }
    }

private:
    bool cas_from_pending_or_queued(Phase to) noexcept {
        Phase expected = Phase::Pending;
        if (phase_.compare_exchange_strong(
                expected, to,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            return true;
        }
        expected = Phase::Queued;
        return phase_.compare_exchange_strong(
            expected, to,
            std::memory_order_acq_rel, std::memory_order_acquire);
    }

    StopSource stop_source_;
    std::atomic<Phase> phase_{Phase::Pending};
    std::atomic<bool> cancel_requested_{false};
    std::atomic_flag notified_{};
    std::function<void(std::exception_ptr)> completion_sink_;
};

/**
 * @brief facade 的按句柄取消索引（内部管道类型）。
 *
 * active entry 在任务终态后立即移出；tombstone 只保留 task id 的有界
 * FIFO 集合用于区分 `AlreadyCompleted` 与 `NotFound`，不保留 callable、
 * promise 或业务 payload。active 容量耗尽时新的可取消提交被明确拒绝，
 * 由 facade 走既有 SubmitRejected 诊断路径。
 */
class TaskCancellationRegistry {
public:
    static constexpr size_t kDefaultCapacity = 65536;

    void set_capacity(size_t capacity) {
        std::lock_guard<std::mutex> lock(mutex_);
        capacity_ = capacity;
        trim_tombstones_locked();
    }

    size_t capacity() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return capacity_;
    }

    size_t active_size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return active_.size();
    }

    /** 注册新 state；active 容量耗尽或 task_id 已被占用时返回 false（可
     *  观察拒绝）。CR-034：此前重复 id 会静默覆盖旧表项，旧 state 与
     *  registry 脱钩后按 id 取消永远打不到它（其任务从此不可取消）。
     *  facade 的 id 由进程级原子计数器生成、保证唯一，重复只可能来自
     *  调用方自行构造——按拒绝处理。 */
    bool register_state(const std::string& task_id,
                        std::shared_ptr<TaskCancellationState> state) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_.size() >= capacity_) {
            return false;
        }
        if (active_.find(task_id) != active_.end()) {
            return false;
        }
        active_[task_id] = std::move(state);
        return true;
    }

    enum class LookupResult {
        Active,
        Terminal,
        NotFound
    };

    LookupResult find(const std::string& task_id,
                      std::shared_ptr<TaskCancellationState>& out) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = active_.find(task_id);
        if (it != active_.end()) {
            out = it->second;
            return LookupResult::Active;
        }
        if (tombstones_.find(task_id) != tombstones_.end()) {
            return LookupResult::Terminal;
        }
        return LookupResult::NotFound;
    }

    /** 任务终态后移出 active entry（幂等）。 */
    void finalize(const std::string& task_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_.erase(task_id) == 0) {
            return;
        }
        if (tombstones_.insert(task_id).second) {
            tombstone_order_.push_back(task_id);
        }
        trim_tombstones_locked();
    }

    CancellationStatus status() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return status_;
    }

    void on_first_request(bool queued_cancel) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++status_.request_count;
        if (queued_cancel) {
            ++status_.queued_cancelled_count;
        } else {
            ++status_.running_request_count;
        }
    }

    void on_completed_after_request() {
        std::lock_guard<std::mutex> lock(mutex_);
        ++status_.completed_after_request_count;
    }

private:
    void trim_tombstones_locked() {
        while (tombstone_order_.size() > capacity_) {
            tombstones_.erase(tombstone_order_.front());
            tombstone_order_.pop_front();
        }
    }

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<TaskCancellationState>> active_;
    std::unordered_set<std::string> tombstones_;
    std::deque<std::string> tombstone_order_;
    size_t capacity_ = kDefaultCapacity;
    CancellationStatus status_;
};

} // namespace kairo
