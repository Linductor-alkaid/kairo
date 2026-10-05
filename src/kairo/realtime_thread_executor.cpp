#include "realtime_thread_executor.hpp"
#include <kairo/comm/realtime_memory.hpp>
#include "util/timer_period_guard.hpp"
#include <stdexcept>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <optional>

// Round-robin CPU hint: incremented each time a new RT thread auto-selects its core.
// Wraps at hw_concurrency so threads spread across all available CPUs.
static std::atomic<unsigned> g_next_rt_cpu_hint{0};

namespace kairo {

namespace {

void report_cycle_manager_exception(const std::string& executor_name,
                                    const char* operation,
                                    std::atomic<uint64_t>& error_count,
                                    util::ExceptionHandler& exception_handler) {
    error_count.fetch_add(1, std::memory_order_relaxed);
    const auto exception = std::current_exception();
    try {
        if (exception) {
            std::rethrow_exception(exception);
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "RealtimeThreadExecutor '%s': ICycleManager::%s threw: %s\n",
                     executor_name.c_str(), operation, error.what());
    } catch (...) {
        std::fprintf(stderr, "RealtimeThreadExecutor '%s': ICycleManager::%s threw an unknown exception\n",
                     executor_name.c_str(), operation);
    }
    exception_handler.handle_task_exception(executor_name, exception);
}

} // namespace

RealtimeThreadExecutor::RealtimeThreadExecutor(const std::string& name,
                                             const RealtimeThreadConfig& config,
                                             bool enable_stats,
                                             size_t queue_capacity)
    : name_(name)
    , config_(config)
    , lockfree_queue_(queue_capacity, 1, enable_stats)
    , task_pool_(queue_capacity)
    , enable_stats_(enable_stats)
{
    // 验证配置
    if (config_.cycle_period_ns <= 0) {
        throw std::invalid_argument("cycle_period_ns must be greater than 0");
    }
    if (config_.thread_name.empty()) {
        throw std::invalid_argument("thread_name must not be empty");
    }
}

RealtimeThreadExecutor::~RealtimeThreadExecutor() {
    stop();
}

bool RealtimeThreadExecutor::start() {
    // stop_mutex_ serializes lifecycle state. It is deliberately released
    // before any ICycleManager call below, because callbacks may re-enter stop.
    std::unique_lock<std::mutex> lifecycle_lock(stop_mutex_);
    if (stopping_.load(std::memory_order_acquire)) {
        return false;
    }
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return false;
    }

    // P-002: (re)open the admission gate. stop_and_join() closes it, so a
    // restart after a completed stop must accept submissions again. The
    // active count is preserved (a completed stop waited it down to zero).
    push_gate_.fetch_and(kPushGateActiveMask, std::memory_order_acq_rel);

    // 创建实时线程
    auto thread_entry = [this]() {
        {
            std::lock_guard<std::mutex> lock(stop_mutex_);
            worker_id_ = std::this_thread::get_id();
        }
#ifdef _WIN32
        std::optional<util::TimerPeriodGuard> timer_period_guard;
        // 在Windows上提高定时器精度（对于短周期很重要）
        // 将定时器精度设置为1ms（默认是15.6ms）
        // 注意：这会增加系统功耗，但提高定时精度
        if (config_.cycle_period_ns < 20000000) {  // 如果周期小于20ms
            timer_period_guard.emplace(1);
        }
#endif
        
        // 自适应 priority: 用户未显式设 (== 0) 时, 按周期建议
        // 线程体内经 set_current_thread_* 自应用：不用 thread_.native_handle()
        // （避免与主线程的 thread_ move-assign 产生 data race），也不把
        // Win32 伪 HANDLE / pthread_self 硬塞进 std::thread::native_handle_type
        // （MinGW posix 线程模型下二者类型不相容，无法转换）。
        if (config_.thread_priority == 0 && config_.cycle_period_ns > 0) {
#ifndef __ANDROID__
            int auto_priority = 0;
            if (config_.cycle_period_ns <= 1'000'000) {        // <= 1ms
                auto_priority = 80;  // 硬实时, 短周期
            } else if (config_.cycle_period_ns <= 10'000'000) {  // <= 10ms
                auto_priority = 50;  // 软实时, 中周期
            }  // > 10ms 保持 0 (普通调度够用)
            if (auto_priority > 0) {
                priority_applied_.store(
                    util::set_current_thread_priority(auto_priority),
                    std::memory_order_release);
            }
#else
            // Android 普通 App 通常没有 CAP_SYS_NICE，自动申请 SCHED_FIFO 只会
            // 得到 EPERM。保持默认普通调度；确有权限的用户可显式设置
            // RealtimeThreadConfig::thread_priority，仍按 best-effort 尝试。
#endif
        } else if (config_.thread_priority != 0) {
            // 用户显式设了, 尊重覆盖
            priority_applied_.store(
                util::set_current_thread_priority(config_.thread_priority),
                std::memory_order_release);
        }

        // CPU 亲和性: 空 = 自适应 sentinel, 用 round-robin 跨核分布; 显式设值尊重覆盖
        if (config_.cpu_affinity.empty()) {
            auto allowed_cpus = util::get_current_thread_affinity();
            if (allowed_cpus.size() >= 2) {
                // P-005 round-robin: 在当前 cpuset 允许的 CPU 集合内轮询。
                // 容器/CI runner 可能只允许非 0 CPU；按 hardware_concurrency()
                // 生成 0..N-1 会被 pthread_setaffinity_np(EINVAL) 静默拒绝。
                const unsigned hint =
                    g_next_rt_cpu_hint.fetch_add(1, std::memory_order_relaxed);
                const int cpu = allowed_cpus[hint % allowed_cpus.size()];
                cpu_affinity_applied_.store(
                    util::set_current_thread_affinity({cpu}),
                    std::memory_order_release);
            }
        } else {
            cpu_affinity_applied_.store(
                util::set_current_thread_affinity(config_.cpu_affinity),
                std::memory_order_release);
        }

        // mlockall 是进程级操作，默认关闭，必须由调用方显式接受其资源影响。
        // 通过引用计数租约持有：本线程退出（执行器停止）时释放，多个执行器
        // 并发启停时只有最后一个停止的执行器才 munlockall。
        util::ProcessMemoryLockLease memory_lock_lease;
        if (config_.enable_process_memory_lock) {
            memory_lock_lease = util::ProcessMemoryLockLease::try_acquire();
            process_memory_lock_applied_.store(
                memory_lock_lease.holds_lock(), std::memory_order_release);
            process_memory_lock_errno_.store(
                memory_lock_lease.error_code(), std::memory_order_release);
        }

        // 设置线程名，便于 top/htop/perf 调试
        if (!config_.thread_name.empty()) {
            util::set_current_thread_name(config_.thread_name);
        }

        // 降低 timer slack，减少定时唤醒抖动（默认 1ns；0 为显式 opt-out，保留内核默认）
        if (config_.timer_slack_ns > 0) {
            timer_slack_applied_.store(
                util::set_current_thread_timer_slack_ns(config_.timer_slack_ns),
                std::memory_order_release);
        }

        // 如果提供了外部周期管理器，使用它进行精确周期控制
        if (config_.cycle_manager) {
            cycle_manager_active_.store(true, std::memory_order_release);
            // 注册周期任务：回调函数是 cycle_loop()
            bool registered = false;
            try {
                registered = config_.cycle_manager->register_cycle(
                    name_, config_.cycle_period_ns, [this]() { cycle_loop(); });
            } catch (...) {
                report_cycle_manager_exception(name_, "register_cycle",
                                               cycle_manager_error_count_, exception_handler_);
            }
            if (!registered) {
                cycle_manager_active_.store(false, std::memory_order_release);
                // 注册失败，回退到内置实现
                simple_cycle_loop();
                return;
            }

            // 启动周期任务（阻塞在此，直到 stop_cycle 被调用）
            bool started = false;
            try {
                started = config_.cycle_manager->start_cycle(name_);
            } catch (...) {
                report_cycle_manager_exception(name_, "start_cycle",
                                               cycle_manager_error_count_, exception_handler_);
            }
            if (!started) {
                cycle_manager_active_.store(false, std::memory_order_release);
                // 启动失败，回退到内置实现
                simple_cycle_loop();
                return;
            }
            cycle_manager_active_.store(false, std::memory_order_release);
        } else {
            // 使用内置的简单周期实现
            simple_cycle_loop();
        }
    };

    try {
        thread_ = thread_factory_(std::move(thread_entry));
    } catch (...) {
        running_.store(false, std::memory_order_release);
        return false;
    }

    return true;
}

void RealtimeThreadExecutor::stop() {
    (void)stop_and_join();
}

bool RealtimeThreadExecutor::stop_and_join() {
    std::thread joiner;
    bool stop_cycle = false;
    {
        // This lock protects only lifecycle state and ownership of thread_.
        // The ICycleManager callback happens after this scope.
        std::unique_lock<std::mutex> lock(stop_mutex_);
        if (std::this_thread::get_id() == worker_id_) {
            // P-002: close the admission gate even on self-stop so submissions
            // past the stop request reject atomically; the external
            // stop_and_join() that must follow is idempotent.
            push_gate_.fetch_or(kPushGateClosedBit, std::memory_order_acq_rel);
            self_stop_requested_.store(true, std::memory_order_release);
            stopping_.store(true, std::memory_order_release);
            running_.store(false, std::memory_order_release);
            return false;
        }

        // A concurrent external caller must wait for the thread owner to join,
        // drain accepted pushes, and publish stop completion.  In particular,
        // it must not clear stopping_ while that finalization is still active.
        if (stop_finalization_in_progress_) {
            stop_completion_cv_.wait(lock, [this] {
                return !stop_finalization_in_progress_;
            });
            return true;
        }

        // P-002: close the admission gate atomically before flipping the
        // lifecycle flags. A producer either registered before this RMW — and
        // is waited for after the join — or rejects without touching the
        // object pool or queue, so no task can appear after the final drain.
        push_gate_.fetch_or(kPushGateClosedBit, std::memory_order_acq_rel);
        stopping_.store(true, std::memory_order_release);
        running_.store(false, std::memory_order_release);
        stop_cycle = config_.cycle_manager &&
                     cycle_manager_active_.load(std::memory_order_acquire);
        if (thread_.joinable()) {
            stop_finalization_in_progress_ = true;
            joiner = std::move(thread_);
        }
    }

    if (stop_cycle) {
        try {
            config_.cycle_manager->stop_cycle(name_);
        } catch (...) {
            report_cycle_manager_exception(name_, "stop_cycle",
                                           cycle_manager_error_count_, exception_handler_);
        }
    }

    const bool joined_thread = joiner.joinable();
    if (joined_thread) {
        joiner.join();
    }

    if (!joined_thread) {
        std::lock_guard<std::mutex> lock(stop_mutex_);
        // P-002: even without a worker thread to join, wait out producers that
        // registered before the gate closed so stop_and_join() keeps its
        // "no producer is inside push_task() on return" contract.
        while ((push_gate_.load(std::memory_order_acquire) & kPushGateActiveMask) != 0) {
            std::this_thread::yield();
        }
        stopping_.store(false, std::memory_order_release);
        return true;
    }
    std::lock_guard<std::mutex> lock(stop_mutex_);
    while ((push_gate_.load(std::memory_order_acquire) & kPushGateActiveMask) != 0) {
        std::this_thread::yield();
    }
    drain_stopped_queue();
    cycle_manager_active_.store(false, std::memory_order_release);
    stopping_.store(false, std::memory_order_release);
    stop_finalization_in_progress_ = false;
    stop_completion_cv_.notify_all();
    return true;
}

bool RealtimeThreadExecutor::enter_push() {
    // P-002: admission is a single RMW — the closed check and the active
    // count increment cannot be interleaved with stop_and_join()'s close, so
    // a producer that wins this CAS is guaranteed to be waited for.
    uint32_t state = push_gate_.load(std::memory_order_relaxed);
    while (true) {
        if (state & kPushGateClosedBit) {
            return false;
        }
        if ((state & kPushGateActiveMask) == kPushGateActiveMask) {
            // Overflow guard: refuse rather than let the +1 spill into the
            // closed bit. 2^31 concurrent producers is unreachable in
            // practice; this only keeps the encoding sound.
            return false;
        }
        if (push_gate_.compare_exchange_weak(
                state, static_cast<uint32_t>(state + 1),
                std::memory_order_acq_rel, std::memory_order_relaxed)) {
            if (admission_registered_hook_) {
                admission_registered_hook_(admission_registered_context_);
            }
            return true;
        }
    }
}

void RealtimeThreadExecutor::leave_push() {
    push_gate_.fetch_sub(1, std::memory_order_release);
}

void RealtimeThreadExecutor::set_admission_registered_hook_for_test(
    AdmissionRegisteredHook hook, void* context) {
    admission_registered_hook_ = hook;
    admission_registered_context_ = context;
}

void RealtimeThreadExecutor::drain_stopped_queue() {
    // stop() prevents any future process_tasks() pass. Return queued-but-never-run
    // wrappers to the pool before task_pool_ is destroyed, and count them as drops.
    TaskWrapper* task_wrapper = nullptr;
    uint64_t drained_count = 0;
    while (lockfree_queue_.pop(task_wrapper)) {
        if (task_wrapper) {
            task_wrapper->func = nullptr;
            task_pool_.release(task_wrapper);
            ++drained_count;
        }
    }
    if (drained_count > 0) {
        dropped_task_count_.fetch_add(drained_count, std::memory_order_relaxed);
        rejected_not_running_count_.fetch_add(drained_count, std::memory_order_relaxed);
    }
}

std::string RealtimeThreadExecutor::get_name() const {
    return name_;
}


ExecutorResult RealtimeThreadExecutor::push_task(std::function<void()> task) {
    // P-001 (260615): 失败路径全部计入 dropped_task_count_ —
    //   (1) 执行器未运行 (stop() 后不再接受任务)
    //   (2) task 为空 (无效输入)
    //   (3) 对象池耗尽 (task_pool_.acquire() == nullptr)
    //   (4) 队列满 (lockfree_queue_.push() == false)
    // 此计数器独立于 enable_stats, 是背压可见性的核心契约.
    if (!task) {
        dropped_task_count_.fetch_add(1, std::memory_order_relaxed);
        rejected_empty_task_count_.fetch_add(1, std::memory_order_relaxed);
        return ExecutorResult::failure(
            ExecutorErrorCode::InvalidConfig, "realtime push: null task");
    }

    // P-002: atomic admission — the closed check and registration are a
    // single RMW. Either the producer counts itself before stop_and_join()
    // closes the gate (and stop waits for it before the final drain), or it
    // returns false here without touching the object pool or queue. This
    // closes the old window where stop observed in_flight_pushes_ == 0 while
    // a producer sat before its fetch_add.
    if (!enter_push()) {
        dropped_task_count_.fetch_add(1, std::memory_order_relaxed);
        rejected_not_running_count_.fetch_add(1, std::memory_order_relaxed);
        return ExecutorResult::failure(
            ExecutorErrorCode::BackendUnavailable,
            "realtime push: executor not running");
    }
    struct InFlightPushGuard {
        RealtimeThreadExecutor& executor;

        ~InFlightPushGuard() {
            executor.leave_push();
        }
    } in_flight_guard{*this};

    // Register before observing running_. stop() flips running_ to false, then
    // waits for all registered producers before draining the single-consumer
    // queue, so an accepted task cannot appear after the final drain.
    if (!running_.load(std::memory_order_acquire)) {
        dropped_task_count_.fetch_add(1, std::memory_order_relaxed);
        rejected_not_running_count_.fetch_add(1, std::memory_order_relaxed);
        return ExecutorResult::failure(
            ExecutorErrorCode::BackendUnavailable,
            "realtime push: executor stopped");
    }

    // 从对象池获取任务对象
    TaskWrapper* task_wrapper = task_pool_.acquire();
    if (!task_wrapper) {
        // 对象池耗尽: 任务被静默丢弃
        dropped_task_count_.fetch_add(1, std::memory_order_relaxed);
        pool_exhausted_count_.fetch_add(1, std::memory_order_relaxed);
        return ExecutorResult::failure(
            ExecutorErrorCode::BackendUnavailable,
            "realtime push: task pool exhausted");
    }
    task_wrapper->func = std::move(task);

    // 推送到无锁队列
    if (!lockfree_queue_.push(task_wrapper)) {
        // 队列满: 释放回对象池, 任务被静默丢弃
        task_pool_.release(task_wrapper);
        dropped_task_count_.fetch_add(1, std::memory_order_relaxed);
        queue_full_count_.fetch_add(1, std::memory_order_relaxed);
        return ExecutorResult::failure(
            ExecutorErrorCode::BackendUnavailable, "realtime push: queue full");
    }
    return ExecutorResult::success();
}

void RealtimeThreadExecutor::simple_cycle_loop() {
    // 计算下一个周期时间点
    auto next_cycle_time = std::chrono::steady_clock::now();
    const auto period_ns = std::chrono::nanoseconds(config_.cycle_period_ns);

    while (running_.load(std::memory_order_acquire)) {
        // 记录周期开始时间
        auto cycle_start = std::chrono::steady_clock::now();

        // 执行周期回调函数
        if (config_.cycle_callback) {
            comm::RealtimeAllocationGuard allocation_guard(
                name_, "cycle_callback", comm::RealtimeAllocationViolationPolicy::RecordOnly,
                config_.enable_allocation_guard);
            try {
                config_.cycle_callback();
            } catch (...) {
                // 捕获周期回调中的异常，防止影响周期执行
                exception_handler_.handle_task_exception(name_, std::current_exception());
            }
        }

        // 处理无锁队列中的任务
        if (running_.load(std::memory_order_acquire)) {
            process_tasks();
        }

        // 记录周期结束时间
        auto cycle_end = std::chrono::steady_clock::now();
        auto cycle_time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            cycle_end - cycle_start
        ).count();

        // 更新统计信息
        update_statistics(cycle_time_ns);

        // 计算下一个周期时间点
        next_cycle_time += period_ns;

        // P-260618-004: skip-late. If a callback ran long and next_cycle_time
        // has fallen into the past, sleeping until it would return immediately
        // and we'd burn through every missed phase with zero sleep. Instead,
        // re-phase to "now + period" so the thread guarantees at least one
        // real sleep and a fresh period. Documented behavior is "skip
        // missed phases" rather than "catch up" — the latter produced
        // jitter storms under load.
        {
            const auto now = std::chrono::steady_clock::now();
            if (now > next_cycle_time) {
                next_cycle_time = now + period_ns;
            }
        }

        // 等待下一个周期（使用 sleep_until 实现精确周期控制）
        std::this_thread::sleep_until(next_cycle_time);
    }
}

void RealtimeThreadExecutor::cycle_loop() {
    // 记录周期开始时间
    auto cycle_start = std::chrono::steady_clock::now();

    // 执行周期回调函数
    if (config_.cycle_callback) {
        comm::RealtimeAllocationGuard allocation_guard(
            name_, "cycle_callback", comm::RealtimeAllocationViolationPolicy::RecordOnly,
            config_.enable_allocation_guard);
        try {
            config_.cycle_callback();
        } catch (...) {
            exception_handler_.handle_task_exception(name_, std::current_exception());
        }
    }

    // 处理无锁队列中的任务
    if (running_.load(std::memory_order_acquire)) {
        process_tasks();
    }

    // 记录周期结束时间并更新统计信息
    auto cycle_end = std::chrono::steady_clock::now();
    auto cycle_time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        cycle_end - cycle_start
    ).count();
    update_statistics(cycle_time_ns);

    // 注意：不在这里执行 sleep，周期管理器负责等待下一个周期
}

void RealtimeThreadExecutor::process_tasks() {
    TaskWrapper* task_wrapper = nullptr;

    // P-260618-002: 单周期任务预算. 每周期最多处理 max_tasks_per_cycle 个任务,
    // 避免生产速率短暂超过消费速率时单周期一口气耗尽整条队列, 打破"周期确定性"契约
    // (cycle_time 爆涨 / cycle_timeout_count 尖刺). 剩余任务自然滚到下一周期处理
    // (MPSC 无锁队列, 无需额外锁). max_tasks_per_cycle == 0 表示不限 (保留旧行为,
    // 向后兼容) — 此时循环条件第一段恒真, 仅靠 pop() 返回 false 退出.
    const uint64_t budget = config_.max_tasks_per_cycle;
    for (uint64_t processed = 0; budget == 0 || processed < budget; ++processed) {
        if (!lockfree_queue_.pop(task_wrapper)) {
            break;  // 队列为空
        }

        if (task_wrapper && task_wrapper->func) {
            try {
                task_wrapper->func();
            } catch (...) {
                exception_handler_.handle_task_exception(name_, std::current_exception());
            }
        }

        // 释放回对象池
        task_pool_.release(task_wrapper);
        if (!running_.load(std::memory_order_acquire)) {
            break;
        }
    }
}

void RealtimeThreadExecutor::update_statistics(int64_t cycle_time_ns) {
    // 增加周期计数
    cycle_count_.fetch_add(1, std::memory_order_relaxed);

    // 更新平均周期时间（整数 EMA，alpha = 1/8，避免 RT 路径中的浮点原子）
    int64_t old_avg = avg_cycle_time_ns_.load(std::memory_order_relaxed);
    int64_t new_avg;
    
    if (old_avg == 0) {
        // 第一次更新，直接使用当前值
        new_avg = cycle_time_ns;
    } else {
        // 指数移动平均
        new_avg = old_avg + ((cycle_time_ns - old_avg) >> 3);
    }
    
    avg_cycle_time_ns_.store(new_avg, std::memory_order_relaxed);

    // 更新最大周期时间（使用 CAS 操作）
    int64_t old_max = max_cycle_time_ns_.load(std::memory_order_relaxed);
    int64_t new_max = cycle_time_ns;
    
    while (new_max > old_max) {
        if (max_cycle_time_ns_.compare_exchange_weak(old_max, new_max, 
                                                      std::memory_order_relaxed,
                                                      std::memory_order_relaxed)) {
            break;
        }
        // CAS 失败，重新读取 old_max
        old_max = max_cycle_time_ns_.load(std::memory_order_relaxed);
        new_max = cycle_time_ns;
    }

    // 检查是否超时（周期执行时间 > 周期时间）
    if (cycle_time_ns > config_.cycle_period_ns) {
        cycle_timeout_count_.fetch_add(1, std::memory_order_relaxed);
    }
}

RealtimeExecutorStatus RealtimeThreadExecutor::get_status() const {
    RealtimeExecutorStatus status;
    status.name = name_;
    status.is_running = running_.load(std::memory_order_acquire);
    status.cycle_period_ns = config_.cycle_period_ns;
    status.cycle_count = cycle_count_.load(std::memory_order_acquire);
    status.cycle_timeout_count = cycle_timeout_count_.load(std::memory_order_acquire);
    status.avg_cycle_time_ns = static_cast<double>(avg_cycle_time_ns_.load(std::memory_order_acquire));
    status.max_cycle_time_ns = static_cast<double>(max_cycle_time_ns_.load(std::memory_order_acquire));
    status.priority_applied = priority_applied_.load(std::memory_order_acquire);
    status.cpu_affinity_applied = cpu_affinity_applied_.load(std::memory_order_acquire);
    status.process_memory_lock_applied =
        process_memory_lock_applied_.load(std::memory_order_acquire);
    status.process_memory_lock_errno =
        process_memory_lock_errno_.load(std::memory_order_acquire);
    status.timer_slack_applied = timer_slack_applied_.load(std::memory_order_acquire);
    // P-001 (260615): 背压可见性
    status.dropped_task_count = dropped_task_count_.load(std::memory_order_acquire);
    status.rejected_not_running_count =
        rejected_not_running_count_.load(std::memory_order_acquire);
    status.rejected_empty_task_count =
        rejected_empty_task_count_.load(std::memory_order_acquire);
    status.pool_exhausted_count = pool_exhausted_count_.load(std::memory_order_acquire);
    status.queue_full_count = queue_full_count_.load(std::memory_order_acquire);
    status.cycle_manager_error_count =
        cycle_manager_error_count_.load(std::memory_order_acquire);
    // failed_pushes / peak_queue_size 仅在 enable_stats=true 时有意义;
    // LockFreeQueue 内部在 stats 关闭时 get_stats() 返回零结构.
    if (enable_stats_) {
        auto qstats = lockfree_queue_.get_stats();
        status.failed_pushes = qstats.failed_pushes;
        status.peak_queue_size = qstats.peak_size;
    }
    // queue_capacity 永远等于构造时的固定容量; 暴露以方便比率分析
    // (dropped / capacity 间接表达"队列满导致丢弃"的最小次数, 因对象池
    //  容量 == 队列容量, 池耗尽与队列满通常同时发生).
    status.queue_capacity = lockfree_queue_.capacity();
    return status;
}

} // namespace kairo
