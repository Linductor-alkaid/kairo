#pragma once

#include "types.hpp"
#include "task_cancellation.hpp"
#include "interfaces.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kairo {

/**
 * @brief 定时任务（delayed/periodic）的状态。
 *
 * 一次性 timer：Scheduled -> Completed（已派发）/ Cancelled /
 * ShutdownCancelled；派发后 timer 本身即终结，任务侧取消状态由
 * `TaskCancellationState` 承接。
 * 周期 timer：注册期间保持 Scheduled，cancel/shutdown 后终结。
 */
enum class TimerState {
    Scheduled,
    Completed,
    Cancelled,
    ShutdownCancelled,
    Failed
};

inline const char* to_string(TimerState state) noexcept {
    switch (state) {
    case TimerState::Scheduled:
        return "Scheduled";
    case TimerState::Completed:
        return "Completed";
    case TimerState::Cancelled:
        return "Cancelled";
    case TimerState::ShutdownCancelled:
        return "ShutdownCancelled";
    case TimerState::Failed:
        return "Failed";
    default:
        return "Unknown";
    }
}

/** @brief TimerHandle 控制请求的结果。 */
enum class TimerOperationResult {
    CancelledBeforeDispatch,            // 派发前取消成功（不再执行）
    CancellationRequestedAfterDispatch, // 已派发，已向任务请求排队/协作取消
    Rescheduled,
    AlreadyCancelled,
    AlreadyCompleted,
    NotFound,
    ShuttingDown,
    InvalidDuration
};

inline const char* to_string(TimerOperationResult result) noexcept {
    switch (result) {
    case TimerOperationResult::CancelledBeforeDispatch:
        return "CancelledBeforeDispatch";
    case TimerOperationResult::CancellationRequestedAfterDispatch:
        return "CancellationRequestedAfterDispatch";
    case TimerOperationResult::Rescheduled:
        return "Rescheduled";
    case TimerOperationResult::AlreadyCancelled:
        return "AlreadyCancelled";
    case TimerOperationResult::AlreadyCompleted:
        return "AlreadyCompleted";
    case TimerOperationResult::NotFound:
        return "NotFound";
    case TimerOperationResult::ShuttingDown:
        return "ShuttingDown";
    case TimerOperationResult::InvalidDuration:
        return "InvalidDuration";
    default:
        return "Unknown";
    }
}

/** @brief 单个定时任务的 best-effort 状态快照（非同步原语）。 */
struct TimerStatus {
    std::string timer_id;
    TimerState state = TimerState::Scheduled;
    bool periodic = false;
    uint64_t execution_count = 0;
    uint64_t active_callback_count = 0;
    uint64_t cancellation_count = 0;
    std::chrono::steady_clock::time_point next_execute_time{};
};

namespace detail {

class TimerScheduler;

/** 一次到期 tick 的执行计划：派发闭包 + 可选的取消状态与索引 id。 */
struct TimerTickPlan {
    std::function<void()> dispatch;
    std::shared_ptr<TaskCancellationState> cancellation;
    std::string tick_id;
};

struct TimerRecord {
    std::string timer_id;
    uint64_t generation = 0;
    TimerState state = TimerState::Scheduled;
    bool periodic = false;
    int64_t interval_ms = 0;
    std::chrono::steady_clock::time_point next_execute_time{};

    // 一次性 timer：派发闭包在到期时被移出并调用；之后不再保留 callable。
    std::function<void()> dispatch;
    std::function<void(std::exception_ptr)> on_cancelled;
    // 一次性 timer 派发后的任务取消状态（cancel 继续向排队/运行中传播）。
    std::shared_ptr<TaskCancellationState> task_state;
    std::string task_state_id;

    // 周期 timer：每个 tick 由 builder 生成新的派发闭包（可携带新取消状态）。
    std::function<TimerTickPlan()> tick_builder;
    std::vector<std::pair<std::string, std::shared_ptr<TaskCancellationState>>>
        active_ticks;

    // get_periodic_status() 查询用的周期任务状态。
    PeriodicTaskStatus periodic_status;
};

struct TimerHeapEntry {
    std::chrono::steady_clock::time_point deadline;
    std::string timer_id;
    uint64_t generation = 0;
};

struct TimerHeapComparator {
    bool operator()(const TimerHeapEntry& lhs, const TimerHeapEntry& rhs) const {
        return lhs.deadline > rhs.deadline;  // 最早到期的在顶部
    }
};

/**
 * @brief facade 定时器注册表与调度线程（内部管道类型）。
 *
 * 拥有 delayed/periodic 的全部定时状态：registry + generation heap。
 * Executor 通过 shared_ptr 持有；TimerHandle 只持有 weak_ptr 锚点，
 * Executor 析构后句柄操作安全失效（返回 NotFound），不会悬垂访问。
 *
 * 设计约束（见 docs/design/task_cancellation_and_timers.md §7）：
 * - reschedule 在锁内递增 generation，旧 heap entry 弹出时按 generation
 *   判定 stale 丢弃；stale 积累超过阈值时按 registry 重建 heap；
 * - 新增更早到期、重排、cancel、shutdown 都唤醒调度线程；
 * - 一次性 timer 的到期线性化点是"弹出当前 generation 并移出 dispatch"，
 *   此前 cancel 走 CancelledBeforeDispatch，此后经任务取消状态继续传播；
 * - 终态 record 只保留 id/状态/计数等有界元数据（FIFO 淘汰），不保留
 *   callable；
 * - 周期 tick 的构建与 active 登记在同一锁内原子完成，cancel 不会漏掉
 *   已登记但未派发的 tick。
 */
class TimerScheduler {
public:
    using TickBuilderFactory = std::function<TimerTickPlan()>;
    using CancelledCallback = std::function<void(std::exception_ptr)>;
    // (task_state_id, state)：向已派发任务传播取消（计数/图/sink 由宿主处理）。
    using TaskCancelHook =
        std::function<void(const std::string&, const std::shared_ptr<TaskCancellationState>&)>;

    static constexpr size_t kTerminalRetention = 1024;

    TimerScheduler() = default;

    TimerScheduler(const TimerScheduler&) = delete;
    TimerScheduler& operator=(const TimerScheduler&) = delete;

    ~TimerScheduler() {
        try {
            stop();
        } catch (...) {
        }
    }

    void set_thread_factory_for_test(
        std::function<std::thread(std::function<void()>)> factory) {
        std::lock_guard<std::mutex> lock(mutex_);
        thread_factory_for_test_ = std::move(factory);
    }

    void set_task_cancel_hook(TaskCancelHook hook) {
        std::lock_guard<std::mutex> lock(mutex_);
        task_cancel_hook_ = std::move(hook);
    }

    /** 当前调度线程是否在运行（快速提示；权威判断在锁内）。 */
    bool running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    /**
     * @brief 启动新一代调度线程。
     *
     * 线程创建失败直接抛出；此时未发布任何新状态，无需回滚。
     * stop() 之后可以再次 start()（新一代）。
     */
    void start() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_.load(std::memory_order_acquire)) {
            return;
        }
        auto generation = std::make_shared<GenerationState>();
        auto entry = [this, generation]() { timer_thread_loop(generation); };
        std::thread thread = thread_factory_for_test_
                                 ? thread_factory_for_test_(std::move(entry))
                                 : std::thread(std::move(entry));
        generation_ = std::move(generation);
        thread_ = std::move(thread);
        running_.store(true, std::memory_order_release);
    }

    /**
     * @brief 停止调度线程并清理未到期任务。
     *
     * 幂等。一次性 Scheduled timer 进入 ShutdownCancelled 并以
     * TaskCancelled(Shutdown) 满足 future（无 failure 事件）；periodic
     * 停止后续 tick，状态保留 is_running=false 供旧状态查询。
     */
    void stop() {
        std::thread thread_to_join;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_.load(std::memory_order_acquire)) {
                return;
            }
            running_.store(false, std::memory_order_release);
            if (generation_) {
                generation_->stop_requested.store(true, std::memory_order_release);
            }
            // CR-135: 打断调度线程的定时等待，join 不再受最长睡眠期约束
            ++schedule_epoch_;
            wake_cv_.notify_all();
            thread_to_join = std::move(thread_);
            generation_.reset();
        }

        if (thread_to_join.joinable()) {
            thread_to_join.join();
        }

        std::vector<CancelledCallback> drains;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& [id, record] : records_) {
                if (record->state != TimerState::Scheduled) {
                    continue;
                }
                record->state = TimerState::ShutdownCancelled;
                ++summary_.cancelled_count;
                if (record->periodic) {
                    record->periodic_status.is_running = false;
                    record->tick_builder = nullptr;
                } else {
                    if (record->on_cancelled) {
                        drains.push_back(std::move(record->on_cancelled));
                    }
                    record->on_cancelled = nullptr;
                    record->dispatch = nullptr;
                    terminal_order_.push_back(id);
                }
            }
            trim_terminal_locked();
        }

        for (auto& drain : drains) {
            drain(std::make_exception_ptr(TaskCancelled(
                TaskCancellationReason::Shutdown,
                "Timer stopped before delayed task execution")));
        }
    }

    /**
     * @brief 登记一次性 timer；返回 timer id（空表示调度线程已停止）。
     *
     * task_state 可为空（无协作取消状态的一次性任务）。
     */
    std::string schedule_once(
        int64_t delay_ms,
        std::string timer_id,
        std::shared_ptr<TaskCancellationState> task_state,
        std::string task_state_id,
        std::function<void()> dispatch,
        CancelledCallback on_cancelled) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_.load(std::memory_order_acquire)) {
            return {};
        }
        auto record = std::make_unique<TimerRecord>();
        record->timer_id = timer_id;
        record->interval_ms = delay_ms;
        record->next_execute_time =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(delay_ms);
        record->dispatch = std::move(dispatch);
        record->on_cancelled = std::move(on_cancelled);
        record->task_state = std::move(task_state);
        record->task_state_id = std::move(task_state_id);
        records_[timer_id] = std::move(record);
        heap_.push(TimerHeapEntry{
            records_[timer_id]->next_execute_time, timer_id, 0});
        ++summary_.pending_count;
        // CR-135: 新 deadline 可能早于线程正在睡的目标，唤醒重算
        ++schedule_epoch_;
        wake_cv_.notify_one();
        return timer_id;
    }

    /**
     * @brief 登记周期 timer；返回 timer id（空表示调度线程已停止）。
     */
    std::string schedule_periodic(int64_t period_ms,
                                  std::string timer_id,
                                  TickBuilderFactory tick_builder) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_.load(std::memory_order_acquire)) {
            return {};
        }
        auto record = std::make_unique<TimerRecord>();
        record->timer_id = timer_id;
        record->periodic = true;
        record->interval_ms = period_ms;
        record->next_execute_time =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(period_ms);
        record->tick_builder = std::move(tick_builder);
        record->periodic_status.task_id = timer_id;
        record->periodic_status.period_ms = period_ms;
        record->periodic_status.is_running = true;
        record->periodic_status.next_execute_time = record->next_execute_time;
        records_[timer_id] = std::move(record);
        heap_.push(TimerHeapEntry{
            records_[timer_id]->next_execute_time, timer_id, 0});
        ++summary_.pending_count;
        // CR-135: 同 schedule_once
        ++schedule_epoch_;
        wake_cv_.notify_one();
        return timer_id;
    }

    /**
     * @brief 请求取消。
     *
     * Scheduled：一次性 timer 立即进入 Cancelled 并以 TaskCancelled(Explicit)
     * 满足 future；周期 timer 原子阻止后续 generation，并对每个已登记的
     * active tick 传播任务取消。Completed（已派发）：经任务取消状态继续
     * 请求排队/协作取消，返回 CancellationRequestedAfterDispatch。
     */
    TimerOperationResult request_cancel(const std::string& timer_id) noexcept {
        try {
            std::shared_ptr<TaskCancellationState> task_state;
            std::string task_state_id;
            std::vector<std::pair<std::string,
                                  std::shared_ptr<TaskCancellationState>>>
                active_ticks;
            CancelledCallback on_cancelled;
            TimerOperationResult result = TimerOperationResult::NotFound;

            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = records_.find(timer_id);
                if (it == records_.end()) {
                    return TimerOperationResult::NotFound;
                }
                TimerRecord& record = *it->second;

                if (record.state == TimerState::Scheduled) {
                    record.state = TimerState::Cancelled;
                    ++summary_.cancelled_count;
                    if (record.periodic) {
                        record.periodic_status.is_running = false;
                        record.tick_builder = nullptr;
                        active_ticks = std::move(record.active_ticks);
                        record.active_ticks.clear();
                    } else {
                        on_cancelled = std::move(record.on_cancelled);
                        record.on_cancelled = nullptr;
                        record.dispatch = nullptr;
                        terminal_order_.push_back(timer_id);
                        trim_terminal_locked();
                    }
                    result = TimerOperationResult::CancelledBeforeDispatch;
                } else if (record.state == TimerState::Completed &&
                           record.task_state) {
                    task_state = record.task_state;
                    task_state_id = record.task_state_id;
                    result =
                        TimerOperationResult::CancellationRequestedAfterDispatch;
                } else if (record.state == TimerState::Cancelled ||
                           record.state == TimerState::ShutdownCancelled) {
                    return TimerOperationResult::AlreadyCancelled;
                } else {
                    return TimerOperationResult::AlreadyCompleted;
                }
            }

            // 锁外传播：避免与宿主回调（registry/图/failure 锁）形成锁序倒置。
            if (on_cancelled) {
                on_cancelled(std::make_exception_ptr(TaskCancelled(
                    TaskCancellationReason::Explicit,
                    "Delayed task cancelled before dispatch")));
            }
            if (task_state) {
                dispatch_task_cancel_hook(task_state_id, task_state);
            }
            for (auto& [tick_id, tick_state] : active_ticks) {
                dispatch_task_cancel_hook(tick_id, tick_state);
            }
            return result;
        } catch (...) {
            return TimerOperationResult::NotFound;
        }
    }

    /**
     * @brief 重排下一次到期时间。
     *
     * 仅对 Scheduled 状态生效；一次性 timer 重排下一次（唯一一次）到期，
     * 周期 timer 只改下一次到期时间、不改 period。delay_ms <= 0 返回
     * InvalidDuration。
     */
    TimerOperationResult reschedule(const std::string& timer_id,
                                    int64_t delay_ms) noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if (delay_ms <= 0) {
                return TimerOperationResult::InvalidDuration;
            }
            auto it = records_.find(timer_id);
            if (it == records_.end()) {
                return TimerOperationResult::NotFound;
            }
            TimerRecord& record = *it->second;
            if (record.state == TimerState::Cancelled ||
                record.state == TimerState::ShutdownCancelled) {
                return TimerOperationResult::AlreadyCancelled;
            }
            if (record.state != TimerState::Scheduled) {
                return TimerOperationResult::AlreadyCompleted;
            }
            record.next_execute_time =
                std::chrono::steady_clock::now() +
                std::chrono::milliseconds(delay_ms);
            ++record.generation;
            heap_.push(TimerHeapEntry{
                record.next_execute_time, timer_id, record.generation});
            if (record.periodic) {
                record.periodic_status.next_execute_time =
                    record.next_execute_time;
            }
            // CR-135: 重排到更早的 deadline 时必须打断睡眠（晚重排无害但
            // 统一 notify，免去方向判断）
            ++schedule_epoch_;
            wake_cv_.notify_one();
            return TimerOperationResult::Rescheduled;
        } catch (...) {
            return TimerOperationResult::NotFound;
        }
    }

    std::optional<TimerStatus> get_status(const std::string& timer_id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = records_.find(timer_id);
        if (it == records_.end()) {
            return std::nullopt;
        }
        const TimerRecord& record = *it->second;
        TimerStatus status;
        status.timer_id = record.timer_id;
        status.state = record.state;
        status.periodic = record.periodic;
        if (record.periodic) {
            status.execution_count = record.periodic_status.execution_count;
        } else {
            status.execution_count =
                record.state == TimerState::Completed ? 1u : 0u;
        }
        status.active_callback_count = record.active_ticks.size();
        status.next_execute_time = record.next_execute_time;
        return status;
    }

    std::optional<PeriodicTaskStatus> get_periodic_status(
        const std::string& timer_id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = records_.find(timer_id);
        if (it == records_.end() || !it->second->periodic) {
            return std::nullopt;
        }
        return it->second->periodic_status;
    }

    std::vector<PeriodicTaskStatus> get_all_periodic_status() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<PeriodicTaskStatus> statuses;
        statuses.reserve(records_.size());
        for (const auto& [id, record] : records_) {
            if (record->periodic) {
                statuses.push_back(record->periodic_status);
            }
        }
        return statuses;
    }

    /** 周期 tick 成功后由 tick wrapper 调用（兼容旧 PeriodicTaskStatus 计数）。 */
    void report_tick_success(const std::string& timer_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = records_.find(timer_id);
        if (it == records_.end()) {
            return;
        }
        auto& status = it->second->periodic_status;
        ++status.execution_count;
        status.consecutive_failure_count = 0;
        status.last_error_message.clear();
    }

    /** 周期 tick 异常后由 tick wrapper 调用。 */
    void report_tick_failure(const std::string& timer_id,
                             const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = records_.find(timer_id);
        if (it == records_.end()) {
            return;
        }
        auto& status = it->second->periodic_status;
        ++status.execution_count;
        ++status.failed_count;
        ++status.consecutive_failure_count;
        status.last_error_message = message;
        status.last_failure_time = std::chrono::steady_clock::now();
    }

    /** tick 终态后移出 active 集合。 */
    void release_tick(const std::string& timer_id,
                      const std::string& tick_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = records_.find(timer_id);
        if (it == records_.end()) {
            return;
        }
        auto& ticks = it->second->active_ticks;
        for (auto tick_it = ticks.begin(); tick_it != ticks.end(); ++tick_it) {
            if (tick_it->first == tick_id) {
                ticks.erase(tick_it);
                return;
            }
        }
    }

    TimerStatusSummary summary() const {
        std::lock_guard<std::mutex> lock(mutex_);
        TimerStatusSummary snapshot = summary_;
        snapshot.pending_count = 0;
        for (const auto& [id, record] : records_) {
            if (record->state == TimerState::Scheduled) {
                ++snapshot.pending_count;
            }
        }
        return snapshot;
    }

    size_t record_count_for_test() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return records_.size();
    }

private:
    struct GenerationState {
        std::atomic<bool> stop_requested{false};
    };

    void trim_terminal_locked() {
        while (terminal_order_.size() > kTerminalRetention) {
            const std::string victim = terminal_order_.front();
            terminal_order_.pop_front();
            auto it = records_.find(victim);
            if (it != records_.end() &&
                it->second->state != TimerState::Scheduled) {
                records_.erase(it);
            }
        }
    }

    /** stale entry 超过阈值时按 registry 重建 heap，防止无界增长。 */
    void compact_heap_locked() {
        std::priority_queue<TimerHeapEntry, std::vector<TimerHeapEntry>,
                            TimerHeapComparator>
            rebuilt;
        for (const auto& [id, record] : records_) {
            if (record->state != TimerState::Scheduled) {
                continue;
            }
            rebuilt.push(TimerHeapEntry{
                record->next_execute_time, id, record->generation});
        }
        heap_ = std::move(rebuilt);
        stale_entries_ = 0;
    }

    void dispatch_task_cancel_hook(
        const std::string& task_state_id,
        const std::shared_ptr<TaskCancellationState>& state) {
        TaskCancelHook hook;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            hook = task_cancel_hook_;
        }
        if (hook && state) {
            try {
                hook(task_state_id, state);
            } catch (...) {
                // 取消传播不得成为新的失败源。
            }
        }
    }

    void timer_thread_loop(std::shared_ptr<GenerationState> generation) {
        using clock = std::chrono::steady_clock;

        while (!generation->stop_requested.load(std::memory_order_acquire)) {
            const auto now = clock::now();
            auto wake_at = now + std::chrono::milliseconds(kIdleWaitMs);

            std::vector<std::function<void()>> due_dispatches;
            std::vector<std::string> due_tick_timer_ids;

            {
                std::unique_lock<std::mutex> lock(mutex_);

                // CR-135: popped > 0 时本迭代必须直接进入派发阶段——等待
                // 与派发同处一个锁作用域，若先等待，已弹出的到期闭包会被
                // 拖到下一个期限才执行（复测 RescheduleChangesNextExpiryOnly
                // 实证：timer 30ms 弹出、闭包 80ms 才跑）。仅当本轮无到期
                // 项才事件驱动地睡向 heap 顶 / 兜底期限。
                size_t popped = 0;
                while (!heap_.empty() && heap_.top().deadline <= now) {
                    ++popped;
                    TimerHeapEntry entry = heap_.top();
                    heap_.pop();
                    auto it = records_.find(entry.timer_id);
                    if (it == records_.end() ||
                        it->second->generation != entry.generation ||
                        it->second->state != TimerState::Scheduled) {
                        if (++stale_entries_ > kHeapCompactThreshold) {
                            compact_heap_locked();
                        }
                        continue;
                    }
                    TimerRecord& record = *it->second;
                    if (record.periodic) {
                        // CR-135: 理想网格锚定——下次期限 = 本次"应到期时刻" +
                        // 周期，而非实际唤醒时刻 + 周期。唤醒过冲（futex/调度
                        // ~0.1-0.3ms）不再逐周期累积为漂移；错过多个周期的场景
                        // 按"错过不追补"契约跳到未来第一个格点，不爆发补发。
                        auto next = entry.deadline +
                                    std::chrono::milliseconds(record.interval_ms);
                        while (next <= now) {
                            next += std::chrono::milliseconds(record.interval_ms);
                        }
                        record.next_execute_time = next;
                        ++record.generation;
                        heap_.push(TimerHeapEntry{record.next_execute_time,
                                                  entry.timer_id,
                                                  record.generation});
                        record.periodic_status.next_execute_time =
                            record.next_execute_time;
                        // tick 构建延后到第二阶段，与状态检查原子完成。
                        due_tick_timer_ids.push_back(entry.timer_id);
                    } else {
                        record.state = TimerState::Completed;
                        if (summary_.pending_count > 0) {
                            --summary_.pending_count;
                        }
                        ++summary_.executed_count;
                        due_dispatches.push_back(std::move(record.dispatch));
                        record.dispatch = nullptr;
                        record.on_cancelled = nullptr;
                        terminal_order_.push_back(entry.timer_id);
                        trim_terminal_locked();
                    }
                }

                // CR-135（事件驱动唤醒）：精确睡到 heap 顶 deadline（堆空为
                // kIdleWaitMs 兜底），任何会引入更早 deadline 或要求停止的
                // 变更（schedule_once/periodic、reschedule、stop）bump
                // schedule_epoch_ 并 notify，立即打断睡眠回到循环顶重算。
                // 请求取消不 notify：取消只会让睡眠目标变 stale，线程醒来
                // 发现后重算即可（既有 lazy-deletion 契约）。
                //
                // 刻意用显式 wait_until(system_clock) 而非 wait_for(duration)
                // 或 wait_until(steady_clock)：GCC ≥ 10 的 libstdc++ 把这两者
                // 都映射到 pthread_cond_clockwait（CLOCK_MONOTONIC），而 gcc-11
                // 时代的 libtsan 未拦截该原语，会把等待期间的解锁从影子状态里
                // 漏掉，醒来重锁时误报 "double lock of a mutex"（gcc PR101978
                // / google/sanitizers#1259，CI 的 TSAN job 正是 gcc-11）。
                // system_clock 重载在所有 libstdc++ 版本都走
                // pthread_cond_timedwait，TSAN 正常拦截。代价：实时钟步进
                //（NTP）可能使某一次睡眠偏早/偏晚一个步进量——循环顶部用
                // steady 重算，一步内自愈，对 ms 粒度 API 无感。
                if (popped == 0) {
                    if (!heap_.empty() && heap_.top().deadline < wake_at) {
                        wake_at = heap_.top().deadline;
                    }
                    const auto slice = wake_at - clock::now();
                    if (slice > clock::duration::zero()) {
                        const uint64_t seen_epoch = schedule_epoch_;
                        const auto wake_deadline =
                            std::chrono::system_clock::now() + slice;
                        wake_cv_.wait_until(lock, wake_deadline, [&] {
                            return schedule_epoch_ != seen_epoch ||
                                   generation->stop_requested.load(
                                       std::memory_order_acquire);
                        });
                    }
                }
            }

            // CR-050: 调度线程是进程级单点——任何一次派发/构建抛出
            // （facade 每 tick 都做堆分配，OOM 或闭包异常是现实输入）穿出
            // 线程函数即 std::terminate，整个进程为丢一个 tick 陪葬。三个
            // 外呼点各自隔离：失败 = 丢当前这一个 tick，调度线程存活。
            // 一次性 timer：直接派发（callable 已移出，取消不再可达）。
            for (auto& dispatch : due_dispatches) {
                if (dispatch) {
                    try {
                        dispatch();
                    } catch (...) {
                        // 丢当前派发；生命周期统计已在出堆时结清。
                    }
                }
            }

            // 周期 tick：构建与 active 登记在同一锁内原子完成，之后锁外派发。
            for (const auto& timer_id : due_tick_timer_ids) {
                TimerTickPlan plan;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    auto it = records_.find(timer_id);
                    if (it == records_.end() ||
                        it->second->state != TimerState::Scheduled ||
                        !it->second->tick_builder) {
                        continue;
                    }
                    try {
                        plan = it->second->tick_builder();
                    } catch (...) {
                        // tick 构建失败（闭包拷贝/分配）：跳过本 tick，
                        // 周期 record 的下一轮重排已在此前出堆时完成，
                        // 调度线程与后续 tick 不受影响。
                        continue;
                    }
                    if (!plan.dispatch) {
                        continue;
                    }
                    if (plan.cancellation) {
                        it->second->active_ticks.emplace_back(plan.tick_id,
                                                              plan.cancellation);
                    }
                    ++summary_.executed_count;
                }
                try {
                    plan.dispatch();
                } catch (...) {
                    // 派发失败（提交到执行器被拒等）：丢当前 tick。
                }
            }
        }
    }

    static constexpr int kIdleWaitMs = 100;
    static constexpr size_t kHeapCompactThreshold = 128;

    mutable std::mutex mutex_;
    std::atomic<bool> running_{false};
    std::shared_ptr<GenerationState> generation_;  // guarded by mutex_
    std::thread thread_;                           // guarded by mutex_
    std::function<std::thread(std::function<void()>)> thread_factory_for_test_;
    TaskCancelHook task_cancel_hook_;
    // CR-135 事件驱动唤醒：调度线程在 mutex_ 上 wait_for 到 heap 顶 deadline；
    // 会引入更早 deadline 或要求停止的变更点（schedule_once / schedule_periodic /
    // reschedule / stop）bump schedule_epoch_ 并 notify。跨 start/stop 世代单调，
    // pred 以"seen != current"判定。由 mutex_ 保护。
    std::condition_variable wake_cv_;
    uint64_t schedule_epoch_{0};

    std::unordered_map<std::string, std::unique_ptr<TimerRecord>> records_;
    std::priority_queue<TimerHeapEntry, std::vector<TimerHeapEntry>,
                        TimerHeapComparator>
        heap_;
    std::deque<std::string> terminal_order_;
    size_t stale_entries_ = 0;
    TimerStatusSummary summary_;
};

} // namespace detail

/**
 * @brief 可复制的定时任务控制句柄。
 *
 * - 复制只共享同一控制锚点；析构不取消；
 * - cancel()/reschedule_after() 是非阻塞请求；
 * - 句柄只持有 id 与可失效锚点：Executor 析构后操作返回 NotFound，
 *   不悬垂访问；句柄不延长 callable 或业务对象的生命周期。
 * 需要"析构即取消"的 RAII 语义时使用 ScopedTimerHandle。
 */
class TimerHandle {
public:
    TimerHandle() = default;

    explicit TimerHandle(
        std::string timer_id,
        std::weak_ptr<detail::TimerScheduler> anchor) noexcept
        : timer_id_(std::move(timer_id))
        , anchor_(std::move(anchor)) {}

    bool valid() const noexcept {
        return !timer_id_.empty();
    }

    explicit operator bool() const noexcept {
        return valid();
    }

    const std::string& id() const noexcept {
        return timer_id_;
    }

    TimerOperationResult cancel() noexcept;

    TimerOperationResult reschedule_after(int64_t delay_ms) noexcept;

    /** best-effort 状态快照；句柄过期或 Executor 已销毁时为空。 */
    std::optional<TimerStatus> status() const;

private:
    std::string timer_id_;
    std::weak_ptr<detail::TimerScheduler> anchor_;
};

inline TimerOperationResult TimerHandle::cancel() noexcept {
    try {
        if (auto scheduler = anchor_.lock()) {
            return scheduler->request_cancel(timer_id_);
        }
    } catch (...) {
    }
    return TimerOperationResult::NotFound;
}

inline TimerOperationResult TimerHandle::reschedule_after(
    int64_t delay_ms) noexcept {
    try {
        if (auto scheduler = anchor_.lock()) {
            return scheduler->reschedule(timer_id_, delay_ms);
        }
    } catch (...) {
    }
    return TimerOperationResult::NotFound;
}

inline std::optional<TimerStatus> TimerHandle::status() const {
    try {
        if (auto scheduler = anchor_.lock()) {
            return scheduler->get_status(timer_id_);
        }
    } catch (...) {
    }
    return std::nullopt;
}

/**
 * @brief move-only 的 RAII 定时句柄：唯一拥有者析构即请求一次非阻塞取消。
 *
 * 析构不等待正在运行的 callback，也不保证 callback 不再访问业务对象；
 * 不提供外部序列化上下文（strand）上的销毁语义（T2/S2 之前）。
 */
class ScopedTimerHandle {
public:
    ScopedTimerHandle() = default;

    explicit ScopedTimerHandle(TimerHandle handle) noexcept
        : handle_(std::move(handle)) {}

    ScopedTimerHandle(const ScopedTimerHandle&) = delete;
    ScopedTimerHandle& operator=(const ScopedTimerHandle&) = delete;

    ScopedTimerHandle(ScopedTimerHandle&& other) noexcept
        : handle_(std::move(other.handle_)) {
        other.handle_ = TimerHandle{};
    }

    ScopedTimerHandle& operator=(ScopedTimerHandle&& other) noexcept {
        if (this != &other) {
            cancel_owned();
            handle_ = std::move(other.handle_);
            other.handle_ = TimerHandle{};
        }
        return *this;
    }

    ~ScopedTimerHandle() {
        cancel_owned();
    }

    bool valid() const noexcept {
        return handle_.valid();
    }

    const TimerHandle& handle() const noexcept {
        return handle_;
    }

    TimerOperationResult cancel() noexcept {
        return handle_.cancel();
    }

    TimerOperationResult reschedule_after(int64_t delay_ms) noexcept {
        return handle_.reschedule_after(delay_ms);
    }

    std::optional<TimerStatus> status() const {
        return handle_.status();
    }

private:
    void cancel_owned() noexcept {
        if (handle_.valid()) {
            (void)handle_.cancel();
        }
    }

    TimerHandle handle_;
};

template <typename T>
struct TimerSubmission {
    TimerHandle handle;
    std::future<T> future;
};

} // namespace kairo
