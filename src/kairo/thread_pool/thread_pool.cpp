#include "thread_pool.hpp"
#include "../task/task.hpp"
#include <stdexcept>
#include <algorithm>
#include <cstdio>
#include <limits>
#include <random>
#include <new>
#include <utility>
#include <vector>

namespace kairo {

// thread_local 变量会在首次使用时自动初始化
thread_local ThreadPool* ThreadPool::current_worker_pool_ = nullptr;

ThreadPool::ThreadPool() : stop_(false), initialized_(false) {
}

ThreadPool::~ThreadPool() {
    // CR-014: 析构排空不外泄异常——shutdown 链路（dispatch/resize/stop）
    // 中任何异常穿出析构函数都是 std::terminate。排空失败最多滞留任务。
    try {
        shutdown(true);
    } catch (...) {
        // 析构不外泄异常。
    }
}

bool ThreadPool::initialize(const ThreadPoolConfig& config) {
    std::unique_lock<std::mutex> lock(mutex_);

    if (initialized_.load()) {
        return false;  // 已经初始化过
    }

    // 验证配置
    if (config.min_threads == 0 || config.min_threads > config.max_threads) {
        return false;
    }

    config_ = config;
    constexpr int64_t kMaxTaskTimeoutMs =
        std::numeric_limits<int64_t>::max() / 1'000'000;
    if (config_.task_timeout_ms > kMaxTaskTimeoutMs) {
        // Saturate instead of allowing the later milliseconds-to-nanoseconds
        // conversion to overflow and make a far-future timeout appear expired.
        config_.task_timeout_ms = kMaxTaskTimeoutMs;
    }

    try {
        stop_.store(false);
        resize_monitor_stop_.store(false);

        // 初始化负载均衡器
        load_balancer_ = std::make_unique<LoadBalancer>(config_.min_threads);

        // 设置负载均衡策略
        if (config_.enable_work_stealing) {
            load_balancer_->set_strategy(LoadBalancer::Strategy::LEAST_TASKS);
        } else {
            load_balancer_->set_strategy(LoadBalancer::Strategy::ROUND_ROBIN);
        }

        // 初始化工作线程本地队列
        // P-260617-002 / PA-9: 全程持 local_queues_mutex_ 的 unique_lock
        // 发布（此刻尚无 reader，加锁是为了与后续所有访问保持同一配对语义）。
        {
            std::unique_lock<std::shared_mutex> lq_lock(local_queues_mutex_);
            auto new_queues = std::make_unique<std::vector<WorkerQueueImpl>>();
            new_queues->reserve(config_.min_threads);
            for (size_t i = 0; i < config_.min_threads; ++i) {
                if (worker_queue_create_hook_for_test_) {
                    worker_queue_create_hook_for_test_(i);
                }
                new_queues->emplace_back(config_.queue_capacity);
            }
            local_queues_ = std::move(new_queues);
        }

        // 初始化任务分发器（TaskDispatcher 是模板类，需要显式指定实例化类型）
        // P-260617-002: 传入 local_queues_mutex_ 指针,dispatcher 内部 dispatch
        // 路径会持 shared_lock，与 resize 路径的 unique_lock 配对防 UAF。
        // PA-2: 同时传入驻停代次计数，dispatcher 的每个"任务变得可执行"
        // 终态（成功搬运 / 回灌）都会递增并 notify。
        {
            std::lock_guard<std::mutex> dispatcher_lock(dispatcher_mutex_);
            dispatcher_ = std::make_unique<TaskDispatcher<WorkerQueueImpl>>(
                *load_balancer_, scheduler_, &local_queues_, &local_queues_mutex_,
                &wake_seq_, config_.enable_work_stealing
            );
        }

        // 初始化动态扩缩容控制器
        resizer_ = std::make_unique<ThreadPoolResizer>(*this, config_);

        // 创建工作线程
        workers_.reserve(config_.min_threads);
        worker_ids_.reserve(config_.min_threads);
        for (size_t i = 0; i < config_.min_threads; ++i) {
            worker_ids_.push_back(i);
            create_worker_thread(i);
        }

        // 启动监控线程（用于动态扩缩容）
        if (config_.max_threads > config_.min_threads) {
            resize_monitor_thread_ = std::thread(&ThreadPool::resize_monitor_thread, this);
        }
    } catch (...) {
        lock.unlock();
        rollback_initialization_failure();
        return false;
    }

    initialized_.store(true, std::memory_order_release);
    condition_.notify_all();
    return true;
}

void ThreadPool::rollback_initialization_failure() {
    initialized_.store(false);
    stop_.store(true);
    resize_monitor_stop_.store(true, std::memory_order_release);
    resize_monitor_cv_.notify_all();
    condition_.notify_all();
    signal_work_added(true);

    if (resize_monitor_thread_.joinable()) {
        resize_monitor_thread_.join();
    }

    for (size_t i = 0; i < workers_.size(); ++i) {
        auto& worker = workers_[i];
        if (worker.joinable()) {
            worker.join();
        }
    }

    workers_.clear();
    worker_ids_.clear();
    {
        std::lock_guard<std::mutex> dispatcher_lock(dispatcher_mutex_);
        dispatcher_.reset();
    }
    resizer_.reset();
    load_balancer_.reset();
    scheduler_.clear();

    {
        std::lock_guard<std::mutex> exit_lock(exit_threads_mutex_);
        exit_threads_.clear();
        exit_threads_count_.store(0, std::memory_order_release);
    }

    {
        std::unique_lock<std::shared_mutex> lq_lock(local_queues_mutex_);
        local_queues_.reset();
    }

    resize_monitor_stop_.store(false);
    stop_.store(false);
    notify_completion_waiters();
}

void ThreadPool::worker_thread(size_t worker_id) {
    if (worker_entry_hook_for_test_) {
        worker_entry_hook_for_test_(worker_id);
    }

    struct WorkerContextGuard {
        explicit WorkerContextGuard(ThreadPool* pool)
            : previous(ThreadPool::current_worker_pool_) {
            ThreadPool::current_worker_pool_ = pool;
        }
        ~WorkerContextGuard() {
            ThreadPool::current_worker_pool_ = previous;
        }
        ThreadPool* previous;
    } worker_context(this);

    // Workers are created before initialize() can publish a usable pool.
    // Keep them out of the scheduling path until that publication succeeds;
    // an initialization rollback instead sets stop_ and releases them to exit.
    {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [this]() {
            return initialized_.load(std::memory_order_acquire) ||
                   stop_.load(std::memory_order_acquire);
        });

        if (stop_.load(std::memory_order_acquire) &&
            !initialized_.load(std::memory_order_acquire)) {
            return;
        }
    }

    // PA-4: Task husk 在整个 worker 生命周期复用，每次执行 move-out 后
    // 只剩可析构空壳，无逐任务分配。
    Task task;

    while (true) {
        if ((stop_.load(std::memory_order_acquire) &&
             !initialized_.load(std::memory_order_acquire)) ||
            should_exit(worker_id)) {
            break;
        }

        // PA-2: 代次必须在完整空扫描【之前】采样。推导：
        //  - bump 发生在采样之后 -> wait 阻塞前的原子复核发现值变化，
        //    立即返回重扫；
        //  - bump 发生在采样之前 -> 入队又 happened-before bump（程序序），
        //    bump happened-before 采样（原子全序），采样 sequenced-before
        //    扫描 -> 扫描透过队列锁必然看到该任务，不会驻停。
        // 两个方向都不存在丢失唤醒窗口。wait 允许虚假唤醒，醒来重扫即可。
        const uint32_t seen = wake_seq_.load(std::memory_order_acquire);

        bool has_task = false;

        // 1. 优先从本地队列获取任务
        // P-260617-002: 持 shared_lock(local_queues_mutex_)，与 resize/shutdown
        // 路径的 unique_lock 配对，防止 vector reallocation 期间悬空访问。
        {
            std::shared_lock<std::shared_mutex> lq_lock(local_queues_mutex_);
            if (local_queues_ && worker_id < local_queues_->size() &&
                (*local_queues_)[worker_id].pop(task)) {
                has_task = true;
            }
        }
        // 2. 如果本地队列为空，尝试工作窃取
        // try_steal_task 内部已自行持 shared_lock(local_queues_mutex_)，
        // 这里的 shared_lock 已释放，无嵌套问题。
        if (!has_task && config_.enable_work_stealing) {
            has_task = try_steal_task(worker_id, task);
        }

        // 3. 直接从全局调度器出队执行（PA-2: 原先只在持 mutex_ 的等待
        // 谓词内做，现已移出一切临界区）
        if (!has_task) {
            has_task = scheduler_.dequeue(task);
        }

        if (has_task) {
            // PA-1: 接力唤醒。notify_one 语义下被唤醒的只有本 worker，
            // 同伴仍在驻停；拿到任务意味着队列刚产出工作，极可能还有
            // 剩余。立即再唤醒一个同伴，并行度按 1->2->4... 指数恢复，
            // 比旧 notify_all 的全群惊群温和，又避免了单 notify_one 把
            // 多 worker 执行串行化（满载下唤醒延迟逐任务叠加）。
            signal_work_added(false);

            // 执行任务（即使 stop_ 为 true，也要执行已获取的任务以排空队列）
            // 检查任务是否已取消
            if (!is_task_cancelled(task)) {
                // P-001 (2026-06-22): RAII guard guarantees active_threads_
                // is decremented even if monitor_ callbacks throw inside
                // execute_task. Previously a fetch_add/fetch_sub pair leaked
                // the decrement on monitor exception, killing the worker
                // and hanging wait_for_completion().
                ThreadPool::ActiveCounter active_guard(*this);
                execute_task(task);
            } else {
                // 任务被取消，也需要更新统计信息
                completed_tasks_.fetch_add(1, std::memory_order_relaxed);
                notify_completion_waiters();
            }

            // 更新负载信息
            // P-260617-002: size() 必须持 shared_lock 访问 local_queues_
            if (load_balancer_) {
                std::shared_lock<std::shared_mutex> lq_lock(local_queues_mutex_);
                if (local_queues_ && worker_id < local_queues_->size()) {
                    size_t queue_size = (*local_queues_)[worker_id].size();
                    load_balancer_->update_load(worker_id, queue_size, 0);
                }
            }
        } else {
            // 完整扫描确认无任务。
            if (stop_.load(std::memory_order_acquire)) {
                // PA-2 退出守门（严格排空语义）。stop_ 下的退出必须排除
                // “任务正处于 dispatch 搬运途中”的窗口：空扫描发生的瞬间，
                // 任务可能已离开 scheduler 又尚未落地 local 队列（在
                // dispatch_batch 的出队/推送缓冲之间），此刻退出会把任务
                // 永久滞留在无 worker 执行的队列里。
                //
                // dispatcher_mutex_ 与 dispatch_batch 的搬运全程互斥：
                // 持锁下复核代次仍等于扫描前的采样值，即扫描开始以来
                // 没有任何任务位置变化（每次入队/搬运/迁移都 bump 代次），
                // 空扫描的结论仍然成立——此刻任务集合封闭且为空（新提交
                // 已被 stop_ 拒绝）。锁释放后的 dispatch 只会从空 scheduler
                // 搬出 0 个任务。代次有变则 continue 重扫。
                std::lock_guard<std::mutex> dlock(dispatcher_mutex_);
                if (wake_seq_.load(std::memory_order_acquire) == seen) {
                    break;
                }
                continue;
            }
            if (should_exit(worker_id)) {
                // 缩容退出不要求排空：resize_local_queues 已把被移除
                // worker 的本地队列迁回 scheduler，剩余消费由保留的
                // worker 负责。
                break;
            }

            // PA-2: 代次驻停（futex）。醒来后回到循环顶部重新采样+扫描。
            wake_seq_.wait(seen, std::memory_order_acquire);
            continue;
        }

        // 检查是否需要退出（缩容时）
        if (should_exit(worker_id)) {
            break;
        }

        // 触发任务分发（从全局调度器分发到本地队列）
        // P-260617-002: dispatcher 内部 dispatch_batch 自身已持 shared_lock，
        // 此处不能再加 shared_lock（std::shared_mutex 不可重入 -> UB）。
        // PA-1: 成功搬运的唤醒由 dispatch_batch 内部完成（PA-36: 不再
        // 由调用方重复 notify_all）。
        if (!stop_.load(std::memory_order_acquire)) {
            // CR-023: dispatch 链路（scheduler enqueue 的 make_unique、lockfree
            // push 的 Task 拷贝）在资源耗尽时可抛 bad_alloc——异常穿出 worker
            // 线程即 std::terminate。吞掉并跳过本轮分发（剩余任务下一轮重试；
            // 单个 Task 在 OOM 下泄漏属可接受降级）。
            try {
                (void)dispatch_pending_tasks(5);  // 批量分发，减少锁竞争
            } catch (...) {
                exception_handler_.handle_task_exception(
                    "ThreadPool::worker_thread dispatch", std::current_exception());
            }
        }
    }
}

void ThreadPool::execute_task(const Task& task) {
    // P-001 (2026-06-22): catch-all guards against exceptions escaping
    // monitor_->record_task_start/complete callbacks, which previously
    // propagated out of execute_task, killed the worker thread, and
    // leaked the active_threads_ decrement (now fixed by ActiveCounter
    // RAII in worker_thread).
    //
    // start_time is declared BEFORE the try so the catch-all can still
    // reference it when computing execution_time_ns for the recovery
    // update_statistics() call below.
    auto start_time = std::chrono::steady_clock::now();
    // CR-020: 任务体是否已开始执行。monitor 回调抛异常时（任务未开始），
    // submit 包装层的 promise 永不会被结算 —— future.get() 永久挂起；
    // catch 侧据此决定是否代为触发 on_timeout 结算。
    bool task_started = false;

    try {
        auto* monitor = monitor_.load(std::memory_order_acquire);
        if (monitor && monitor->is_enabled()) {
            monitor->record_task_start(task.task_id, "default");
        }

    // P024 soft timeout: check elapsed time BEFORE execution.
    // If elapsed >= timeout at execution start, skip the task entirely and
    // record it as timed out.  This is a pre-execution check only — C++ has
    // no safe mechanism to forcefully kill a running thread, so in-progress
    // tasks are never interrupted.
    bool timed_out = false;
    if (task.timeout_ms > 0 && task.submit_time_ns > 0) {
        int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count();
        int64_t timeout_ns = task.timeout_ms * 1'000'000;
        if ((now_ns - task.submit_time_ns) >= timeout_ns) {
            timed_out = true;
        }
    }

    bool success = false;

    if (!timed_out) {
        task_started = true;
        try {
            // 执行任务
            if (task.function) {
                task.function();
                success = true;
            }
        } catch (...) {
            // 捕获所有异常，通过ExceptionHandler处理
            exception_handler_.handle_task_exception("ThreadPool", std::current_exception());
            success = false;
        }
    } else {
        // 软超时：跳过执行，记录超时计数
        timeout_count_.fetch_add(1, std::memory_order_relaxed);
        auto timeout_exception = std::make_exception_ptr(TimedOutException(
            "Task timed out after " + std::to_string(task.timeout_ms) + "ms"));
        if (task.on_timeout) {
            try {
                task.on_timeout(timeout_exception);
            } catch (...) {
                exception_handler_.handle_task_exception(
                    "ThreadPool::execute_task timeout callback",
                    std::current_exception());
            }
        }
        if (monitor && monitor->is_enabled()) {
            monitor->record_task_timeout(task.task_id);
        }
        // 标记 timed_out 用于 update_statistics: completed++ 但不 failed++。
        // wait_for_completion() 以 completed 覆盖全部已结束任务，failed 是其子集。
    }

    auto end_time = std::chrono::steady_clock::now();
    int64_t execution_time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        end_time - start_time
    ).count();

    if (!timed_out && monitor && monitor->is_enabled()) {
        monitor->record_task_complete(task.task_id, success, execution_time_ns);
    }
    update_statistics(execution_time_ns, success, timed_out);
    } catch (...) {
        // Monitor callbacks are user-supplied; their exceptions must not
        // kill the worker. Report via the configured exception handler so
        // operators see them, then return normally — ActiveCounter in
        // worker_thread will still decrement active_threads_.
        //
        // CRITICAL: also fire update_statistics here. Otherwise
        // completed_tasks_ never increments and wait_for_completion()
        // hangs forever (or up to 300s) waiting for total == completed.
        exception_handler_.handle_task_exception("ThreadPool::execute_task",
                                                 std::current_exception());
        // CR-020: 异常发生在任务开始执行之前（record_task_start 抛出）时，
        // 任务包装层的 promise 无人结算，future.get() 永久挂起。镜像软超时
        // 分支触发 on_timeout，把异常交付给包装层——其 promise_ready CAS
        // 保证只结算一次。任务已开始（record_task_complete/update_statistics
        // 抛出）时 future 已由包装层自己结算，不重复触发。
        if (!task_started && task.on_timeout) {
            try {
                task.on_timeout(std::current_exception());
            } catch (...) {
                exception_handler_.handle_task_exception(
                    "ThreadPool::execute_task timeout callback",
                    std::current_exception());
            }
        }
        // Best-effort stats: counted as completed but NOT failed (a
        // monitor exception is not a task failure — the user code
        // didn't even run). This matches the soft-timeout branch
        // (timed_out=true) which also counts as completed without
        // incrementing failed_.
        int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now() - start_time).count();
        update_statistics(ns, /*success=*/false, /*timed_out=*/true);
    }
}

void ThreadPool::update_statistics(int64_t execution_time_ns, bool success, bool timed_out) {
    completed_tasks_.fetch_add(1, std::memory_order_relaxed);

    if (!success && !timed_out) {
        // 软超时不计入 failed (有专门的 timeout_count); 只有异常/失败计入 failed
        failed_tasks_.fetch_add(1, std::memory_order_relaxed);
    }

    // 更新总执行时间（使用原子操作累加）
    total_execution_time_ns_.fetch_add(execution_time_ns, std::memory_order_relaxed);

    notify_completion_waiters();
}

ThreadPoolStatus ThreadPool::get_status() const {
    std::lock_guard<std::mutex> lock(mutex_);

    ThreadPoolStatus status;
    status.total_threads = workers_.size();
    status.active_threads = active_threads_.load(std::memory_order_relaxed);
    // Guard against size_t underflow: active_threads_ is a relaxed atomic
    // that may briefly exceed workers_.size() during resize() (e.g. when
    // workers are being torn down but the counter has not yet been
    // decremented). Saturate to 0 instead of wrapping to a huge value.
    status.idle_threads = (status.active_threads <= status.total_threads)
                              ? (status.total_threads - status.active_threads)
                              : 0;

    // 队列大小 = 全局调度器 + 所有本地队列
    // P-260617-002: 持 shared_lock 防止与并发 resize 数据竞争
    size_t local_queue_size = 0;
    {
        std::shared_lock<std::shared_mutex> lq_lock(local_queues_mutex_);
        if (local_queues_) {
            for (const auto& queue : *local_queues_) {
                local_queue_size += queue.size();
            }
        }
    }
    status.queue_size = scheduler_.size() + local_queue_size;

    status.total_tasks = total_tasks_.load(std::memory_order_relaxed);
    status.completed_tasks = completed_tasks_.load(std::memory_order_relaxed);
    status.failed_tasks = failed_tasks_.load(std::memory_order_relaxed);

    // 计算平均任务执行时间
    size_t completed = completed_tasks_.load(std::memory_order_relaxed);
    if (completed > 0) {
        int64_t total_time = total_execution_time_ns_.load(std::memory_order_relaxed);
        status.avg_task_time_ms = (static_cast<double>(total_time) / static_cast<double>(completed)) / 1e6;
    } else {
        status.avg_task_time_ms = 0.0;
    }

    // CPU使用率暂不实现（需要系统调用）
    status.cpu_usage_percent = 0.0;

    return status;
}

ShutdownResult ThreadPool::shutdown(bool wait_for_tasks) {
    const bool caller_is_worker = is_current_worker_thread();
    {
        std::unique_lock<std::mutex> shutdown_lock(shutdown_mutex_);
        if (caller_is_worker) {
            if (!shutdown_started_) {
                shutdown_started_ = true;
            }
            // The current task contributes to completion and owns one of the
            // worker threads, so it must never wait or join here.
            shutdown_lock.unlock();

            {
                std::lock_guard<std::mutex> lock(mutex_);
                stop_.store(true);
            }
            condition_.notify_all();
            // PA-2: 唤醒驻停中的 worker（atomic wait 驻停不再监听
            // condition_），让它们观察到 stop_ 后排空退出。
            signal_work_added(true);
            notify_completion_waiters();
            return ShutdownResult::RequestedFromWorker;
        }

        if (!shutdown_started_) {
            shutdown_started_ = true;
            shutdown_finalizer_started_ = true;
        } else if (!shutdown_finalizer_started_) {
            // A worker made the initial request. This external caller now
            // takes responsibility for the blocking completion and joins.
            shutdown_finalizer_started_ = true;
        } else if (!shutdown_complete_) {
            shutdown_cv_.wait(shutdown_lock, [this]() {
                return shutdown_complete_;
            });
            return ShutdownResult::Completed;
        } else {
            return ShutdownResult::Completed;
        }
    }

    // 先停止接收新任务，再按需等待已被接受的任务完成。
    // 如果先 wait_for_completion()，并发 submit 可以持续推进 total_tasks_，
    // 让等待目标移动，甚至在 submit-vs-shutdown 压测中表现为长时间等待。
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_.store(true);
    }
    condition_.notify_all();
    signal_work_added(true);
    notify_completion_waiters();

    if (wait_for_tasks) {
        wait_for_completion();
    }

    // 停止监控线程。停止位是 atomic，不需要拿监控条件变量的 mutex；
    // shutdown 路径不碰这把等待锁，可以避免 TSAN/libstdc++ 在并发停止时
    // 把条件变量内部锁序报告成 double-lock。
    resize_monitor_stop_.store(true, std::memory_order_release);
    resize_monitor_cv_.notify_all();
    if (resize_monitor_thread_.joinable()) {
        resize_monitor_thread_.join();
    }

    // 唤醒所有等待的线程
    condition_.notify_all();
    signal_work_added(true);

    // A concurrent resize may be joining a subset of workers. Serialize the
    // final join/clear sequence with it so each std::thread has one owner.
    std::unique_lock<std::mutex> resize_lock(resize_mutex_);

    // 等待所有工作线程退出
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    workers_.clear();
    worker_ids_.clear();
    {
        std::lock_guard<std::mutex> dispatcher_lock(dispatcher_mutex_);
        dispatcher_.reset();
    }
    // P-260617-002: shutdown 时所有 worker 已 join 完毕，无并发 reader，
    // 仍持 unique_lock 清空 local_queues_ 以与 worker 路径的 shared_lock
    // 保持配对语义（reader 一律持 shared_lock，写者一律持 unique_lock）。
    {
        std::unique_lock<std::shared_mutex> lq_lock(local_queues_mutex_);
        local_queues_.reset();
    }

    {
        std::lock_guard<std::mutex> shutdown_lock(shutdown_mutex_);
        shutdown_complete_ = true;
    }
    shutdown_cv_.notify_all();
    return ShutdownResult::Completed;
}

bool ThreadPool::resize_local_queues(size_t new_num_queues) {
    if (new_num_queues == 0) {
        return false;
    }

    auto new_queues = std::make_unique<std::vector<WorkerQueueImpl>>();
    new_queues->reserve(new_num_queues);
    for (size_t i = 0; i < new_num_queues; ++i) {
        new_queues->emplace_back(config_.queue_capacity);
    }

    std::unique_lock<std::shared_mutex> lq_lock(local_queues_mutex_);
    if (local_queues_) {
        for (auto& queue : *local_queues_) {
            Task task;
            while (queue.pop(task)) {
                scheduler_.enqueue(std::move(task));
            }
        }
    }

    local_queues_ = std::move(new_queues);

    if (load_balancer_) {
        load_balancer_->resize(new_num_queues);
    }

    lq_lock.unlock();
    // PA-2: 迁移回 scheduler 的任务是"变得可执行"的事件，唤醒全部驻停
    // worker（resize 语境，保守 notify_all）。
    signal_work_added(true);
    notify_completion_waiters();
    return true;
}

bool ThreadPool::resize(size_t new_size) {
    std::unique_lock<std::mutex> resize_lock(resize_mutex_);

    size_t current_size = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialized_.load(std::memory_order_acquire) ||
            stop_.load(std::memory_order_acquire) ||
            new_size < config_.min_threads || new_size > config_.max_threads) {
            return false;
        }
        current_size = workers_.size();
    }

    if (new_size == current_size) {
        return false;
    }

    // Publish the replacement queue set before changing worker membership.
    // resize_local_queues() returns queued work to scheduler_ first, so a
    // worker selected for removal cannot strand tasks in its old local queue.
    if (!resize_local_queues(new_size)) {
        return false;
    }

    if (new_size > current_size) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_.load(std::memory_order_acquire) ||
            !initialized_.load(std::memory_order_acquire)) {
            return false;
        }
        workers_.reserve(new_size);
        worker_ids_.reserve(new_size);
        for (size_t worker_id = current_size; worker_id < new_size; ++worker_id) {
            worker_ids_.push_back(worker_id);
            create_worker_thread(worker_id);
        }
        condition_.notify_all();
        return true;
    }

    std::vector<size_t> removed_ids;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (workers_.size() != current_size || worker_ids_.size() != current_size) {
            return false;
        }
        removed_ids.assign(worker_ids_.begin() + static_cast<std::ptrdiff_t>(new_size),
                           worker_ids_.end());
    }

    {
        std::lock_guard<std::mutex> exit_lock(exit_threads_mutex_);
        exit_threads_.insert(exit_threads_.end(), removed_ids.begin(), removed_ids.end());
        // PA-17: 锁内同步维护计数，释放 should_exit 的原子快速路径。
        exit_threads_count_.store(exit_threads_.size(), std::memory_order_release);
    }
    condition_.notify_all();
    // PA-2: 被标记退出的 worker 可能正驻停在 wake_seq_ 上，必须经代次
    // 计数唤醒才能观察到 exit_threads_ 变化。
    signal_work_added(true);

    // Do not hold mutex_ while joining: an idle worker needs it to wake and
    // evaluate should_exit().  We retain resize_mutex_ for the full protocol.
    for (size_t i = new_size; i < current_size; ++i) {
        if (workers_[i].joinable()) {
            workers_[i].join();
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        workers_.resize(new_size);
        worker_ids_.resize(new_size);
    }
    {
        std::lock_guard<std::mutex> exit_lock(exit_threads_mutex_);
        for (size_t worker_id : removed_ids) {
            const auto it = std::find(exit_threads_.begin(), exit_threads_.end(), worker_id);
            if (it != exit_threads_.end()) {
                exit_threads_.erase(it);
            }
        }
        exit_threads_count_.store(exit_threads_.size(), std::memory_order_release);
    }
    notify_completion_waiters();
    return true;
}

namespace {
// CR-109: completion 等待的兜底轮询分片。正常推进由任务终态通知驱动；
// 分片只约束非通知驱动边角（stop 后残留任务搬运 / 活性兜底）的响应上界。
constexpr std::chrono::milliseconds kCompletionPollSlice{50};
}

void ThreadPool::wait_for_completion() {
    (void)try_wait_for_completion(kDefaultWaitForCompletionTimeout);
}

bool ThreadPool::try_wait_for_completion(std::chrono::milliseconds timeout) {
    // CR-109: 等待者注册（驱动 CR-108 的门控通知）+ 通知驱动的谓词等待。
    // 旧实现固定 10ms 盲轮询：300s 等待 = 3 万次 is_completion_ready()
    // 全量扫队列（scheduler + 全部本地队列的三把锁）。现在正常推进由每个
    // 任务终态的 notify 唤醒，兜底分片只服务两个非通知驱动的边角：
    //  1. shutdown 后 worker 已观察到 stop_，残留 scheduler 任务只能由
    //     等待方 dispatch 推进（入场 + 每个兜底分片到期各搬运一次）；
    //  2. 活性兜底（时钟异常/唤醒丢失）。
    struct WaiterGuard {
        std::atomic<size_t>& count;
        ~WaiterGuard() { count.fetch_sub(1, std::memory_order_release); }
    };
    completion_waiters_.fetch_add(1, std::memory_order_acq_rel);
    WaiterGuard waiter_guard{completion_waiters_};

    std::unique_lock<std::mutex> lock(completion_mutex_);
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    // 入场先搬运一次（原语义：残留任务的推进责任在等待方）。
    // shutdown(true) 停止接单后，被接受的任务可能仍滞留全局 scheduler。
    lock.unlock();
    (void)dispatch_pending_tasks(64);
    lock.lock();

    while (!is_completion_ready()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return false;
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const auto wait_slice =
            std::min(remaining, kCompletionPollSlice);

        if (completion_cv_.wait_until(lock, now + wait_slice) ==
            std::cv_status::timeout) {
            // 兜底分片到期（非通知驱动）：搬运一次 + 外层重查。
            lock.unlock();
            (void)dispatch_pending_tasks(64);
            lock.lock();
        }
    }

    return true;
}

bool ThreadPool::is_completion_ready() const {
    // 等待所有任务完成需要同时满足：
    // 1. 全局调度器 + 所有本地队列为空（没有待执行的任务）
    // 2. 没有活跃线程（没有正在执行的任务）
    // 3. 所有已提交的任务都已完成（total == completed）。
    // failed_tasks_ 是 completed_tasks_ 的失败子集，不再参与完成等式。
    bool scheduler_empty = scheduler_.empty();

    size_t local_queue_total = 0;
    {
        std::shared_lock<std::shared_mutex> lq_lock(local_queues_mutex_);
        if (local_queues_) {
            for (const auto& queue : *local_queues_) {
                local_queue_total += queue.size();
            }
        }
    }

    size_t total = total_tasks_.load(std::memory_order_acquire);
    size_t completed = completed_tasks_.load(std::memory_order_acquire);
    size_t active = active_threads_.load(std::memory_order_acquire);

    return scheduler_empty &&
           local_queue_total == 0 &&
           active == 0 &&
           total == completed;
}

void ThreadPool::notify_completion_waiters() {
    // CR-108: 等待者门控——无等待者时 notify_all 是纯调度噪声，有等待者时
    // 才惊群。等待者计数由 try_wait_for_completion 的 RAII 守卫维护。
    if (completion_waiters_.load(std::memory_order_relaxed) != 0) {
        completion_cv_.notify_all();
    }
}

void ThreadPool::signal_work_added(bool wake_all) {
    // PA-1/PA-2: 提交/搬运路径的唤醒不再经过 mutex_ + condition_。
    // 旧 notify_workers_after_queue_change 每次提交要第二次获取全局
    // mutex_ 再 notify_all（惊群）；现在唤醒 = 一次代次 RMW + futex
    // notify，单任务 notify_one、批次/停止/缩容 notify_all。
    wake_seq_.fetch_add(1, std::memory_order_release);
    if (wake_all) {
        wake_seq_.notify_all();
    } else {
        wake_seq_.notify_one();
    }
}

size_t ThreadPool::dispatch_pending_tasks(size_t max_tasks) {
    std::lock_guard<std::mutex> lock(dispatcher_mutex_);
    if (!dispatcher_) {
        return 0;
    }
    return dispatcher_->dispatch_batch(max_tasks);
}

bool ThreadPool::is_stopped() const {
    return stop_.load(std::memory_order_relaxed);
}

bool ThreadPool::is_current_worker_thread() const noexcept {
    return current_worker_pool_ == this;
}

void ThreadPool::set_task_monitor(monitor::TaskMonitor* m) {
    monitor_.store(m, std::memory_order_release);
}

bool ThreadPool::try_steal_task(size_t worker_id, Task& task) {
    // P-260617-002: 公开入口持 shared_lock，内部实现 try_steal_task_impl
    // 假设调用方已持 shared_lock，避免 worker_thread 谓词中重入 shared_lock
    // (std::shared_mutex 不可重入 -> UB)。
    std::shared_lock<std::shared_mutex> lock(local_queues_mutex_);
    return try_steal_task_impl(worker_id, task);
}

bool ThreadPool::try_steal_task_impl(size_t worker_id, Task& task) {
    // P-260617-002: 调用方必须已持 shared_lock(local_queues_mutex_)。
    // 内部不再获取该锁。worker_thread 谓词中已持 shared_lock 时调用此函数
    // 不会重入，避免 std::shared_mutex 重入 UB。
    if (!local_queues_ || local_queues_->size() <= 1) {
        return false;  // 只有一个线程，无法窃取
    }

    // PA-13: 无分配的最高负载 victim 选择。原实现每次窃取尝试经
    // get_all_loads() 拷贝整个负载 vector、再构造 pair vector 并 sort
    // （2 次堆分配 + O(n log n)），仅为了优先偷负载最高者。
    // 最高负载者尝试失败后落到下方随机扫描，仍会遍历全部其他队列，
    // 窃取成功率不受影响。
    if (load_balancer_) {
        size_t victim = load_balancer_->highest_load_victim(
            worker_id, local_queues_->size());
        if (victim != static_cast<size_t>(-1) &&
            (*local_queues_)[victim].steal(task)) {
            size_t queue_size = (*local_queues_)[victim].size();
            load_balancer_->update_load(victim, queue_size, 0);
            return true;
        }
    }

    // 回退到随机策略（如果无法获取负载信息或所有线程负载相同）
    // 使用 thread_local 随机数生成器（首次使用时会自动初始化）
    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<size_t> dist(0, local_queues_->size() - 1);
    size_t start_index = dist(rng);

    // 尝试从其他线程窃取任务
    for (size_t i = 0; i < local_queues_->size(); ++i) {
        size_t target_id = (start_index + i) % local_queues_->size();

        // 跳过自己
        if (target_id == worker_id) {
            continue;
        }

        // 尝试窃取
        if ((*local_queues_)[target_id].steal(task)) {
            // 更新目标线程的负载信息
            if (load_balancer_) {
                size_t queue_size = (*local_queues_)[target_id].size();
                load_balancer_->update_load(target_id, queue_size, 0);
            }
            return true;
        }
    }

    return false;
}

bool ThreadPool::should_exit(size_t worker_id) const {
    // PA-17: 稳态快速路径。缩容窗口之外 exit_threads_ 恒空，一次原子
    // load 即可返回；此前每个 worker 每次主循环迭代 4 个调用点全部
    // 获取 exit_threads_mutex_ 并线性扫描。
    if (exit_threads_count_.load(std::memory_order_acquire) == 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(exit_threads_mutex_);
    return std::find(exit_threads_.begin(), exit_threads_.end(), worker_id)
           != exit_threads_.end();
}

void ThreadPool::resize_monitor_thread() {
    while (true) {
        std::unique_lock<std::mutex> lock(resize_monitor_mutex_);
        if (resize_monitor_cv_.wait_for(lock, std::chrono::seconds(1), [this]() {
                return resize_monitor_stop_.load(std::memory_order_acquire);
            })) {
            break;
        }
        lock.unlock();

        // CR-021: 本线程体内任何异常（如 resize_local_queues 的分配失败、
        // create_worker_thread 的 std::system_error）都绝不能穿出线程函数
        // ——异常逃逸即 std::terminate，整个进程被一个监控周期拖死。
        // 吞掉并跳过本周期，下一秒重试。
        try {
            // 更新线程池状态信息
            ThreadPoolStatus status = get_status();

            // 计算平均等待时间（简化实现，使用队列大小估算）
            double avg_wait_time_ms = 0.0;
            if (status.queue_size > 0 && status.total_threads > 0) {
                // 假设每个任务平均执行时间，估算等待时间
                avg_wait_time_ms = (static_cast<double>(status.queue_size) * status.avg_task_time_ms)
                                   / static_cast<double>(status.total_threads);
            }

            if (resizer_) {
                resizer_->update_status(
                    status.queue_size,
                    status.active_threads,
                    status.total_threads,
                    avg_wait_time_ms
                );

                // 检查并执行扩缩容
                resizer_->check_and_resize();
            }
        } catch (...) {
            // 监控周期失败不致命：记录并放弃本周期。
            exception_handler_.handle_task_exception(
                "ThreadPool::resize_monitor_thread", std::current_exception());
        }
    }
}

void ThreadPool::create_worker_thread(size_t worker_id) {
    if (worker_thread_start_hook_for_test_) {
        worker_thread_start_hook_for_test_(worker_id);
    }

    workers_.emplace_back(&ThreadPool::worker_thread, this, worker_id);

    // 设置线程优先级
    if (config_.thread_priority != 0) {
        util::set_thread_priority(workers_.back().native_handle(),
                                 config_.thread_priority);
    }

    // 设置CPU亲和性
    if (!config_.cpu_affinity.empty()) {
        int cpu_id = config_.cpu_affinity[worker_id % config_.cpu_affinity.size()];
        util::set_cpu_affinity(workers_.back().native_handle(), {cpu_id});
    }
}

bool ThreadPool::try_submit(std::function<void()> task) {
    return try_submit(std::move(task), {});
}

bool ThreadPool::try_submit(std::function<void()> task,
                            std::function<void(std::exception_ptr)> on_timeout) {
    if (!task) {
        if (on_timeout) {
            on_timeout(std::make_exception_ptr(std::invalid_argument("empty task")));
        }
        return false;
    }

    Task executor_task;
    executor_task.task_id = generate_task_id();
    executor_task.priority = TaskPriority::NORMAL;
    executor_task.function = std::move(task);
    executor_task.on_timeout = std::move(on_timeout);
    executor_task.submit_time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
    // CR-022: timeout_ms 在锁内从 config_ 读取（见下方临界区）。

    // PA-4: monitor 需要 task_id 时先拷出，入队即可移动消耗整个 Task。
    auto* monitor = monitor_.load(std::memory_order_acquire);
    const bool monitor_on = (monitor && monitor->is_enabled());
    std::string monitor_id;
    if (monitor_on) {
        monitor_id = executor_task.task_id;
    }

    // Keep mutex_ scoped to the ThreadPool state change. Dispatching may take
    // local queue / load-balancer / scheduler locks, so doing it after releasing
    // mutex_ avoids lock-order inversions with workers and shutdown.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // CR-022: 初始化失败回滚会把 stop_ 复位为 false，但 dispatcher 为
        // null、无 worker——此时接受提交只会让任务滞留 scheduler、future
        // 永挂、wait_for_completion 挂到超时。与 stop_ 一并校验 initialized_。
        // timeout_ms 读 config_ 也必须在锁内，否则与 initialize 写 config_
        // 构成数据竞争（UB）。
        if (!initialized_.load(std::memory_order_acquire) ||
            stop_.load(std::memory_order_acquire)) {
            return false;
        }
        executor_task.timeout_ms = config_.task_timeout_ms;

        // NN-12: queued 诊断必须在 enqueue 之前补记。worker 不持有 mutex_，
        // enqueue 后可在提交线程补记前取走任务并执行——record_task_start
        // 查不到条目成为 no-op，条目以 Queued 状态在任务已运行时才创建，
        // 快照 state_counts 缺 Running（CI 实测 test_executor_snapshot 因
        // map::at abort）。入队前补记建立 queued→start→complete 严格顺序。
        // 锁序 pool.mutex_ → monitor.mutex_（monitor 回调不反向取 pool 锁）。
        if (monitor_on) {
            try {
                monitor->record_task_queued(monitor_id, "default", "default");
            } catch (...) {
                // Diagnostics must never turn an accepted task into a rejection.
            }
        }

        scheduler_.enqueue(std::move(executor_task));
        total_tasks_.fetch_add(1, std::memory_order_relaxed);
    }

    // PA-1: 入队即唤醒一个 worker。任务在 scheduler，任一被唤醒 worker
    // 的出队扫描（local -> steal -> scheduler）必然拿到；提交路径不再
    // 内联 dispatch(1) —— 满载下该内联派发与 worker 的自由扫描互相
    // 抢 dispatcher/scheduler/queue 锁，锁交接的调度延迟逐任务叠加。
    signal_work_added(false);

    return true;
}

bool ThreadPool::try_submit_priority(int priority, std::function<void()> task) {
    return try_submit_priority(priority, std::move(task), {});
}

bool ThreadPool::try_submit_priority(
    int priority,
    std::function<void()> task,
    std::function<void(std::exception_ptr)> on_timeout) {
    return try_submit_priority(priority, std::move(task), std::move(on_timeout),
                               TaskSchedulingMeta{});
}

bool ThreadPool::try_submit_priority(
    int priority,
    std::function<void()> task,
    std::function<void(std::exception_ptr)> on_timeout,
    const TaskSchedulingMeta& meta) {
    TaskPriority task_priority = TaskPriority::NORMAL;
    if (priority <= 0) {
        task_priority = TaskPriority::LOW;
    } else if (priority == 1) {
        task_priority = TaskPriority::NORMAL;
    } else if (priority == 2) {
        task_priority = TaskPriority::HIGH;
    } else {
        task_priority = TaskPriority::CRITICAL;
    }

    if (!task) {
        if (on_timeout) {
            on_timeout(std::make_exception_ptr(std::invalid_argument("empty task")));
        }
        return false;
    }

    Task executor_task;
    executor_task.task_id = generate_task_id();
    executor_task.priority = task_priority;
    executor_task.function = std::move(task);
    executor_task.on_timeout = std::move(on_timeout);
    executor_task.submit_time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
    executor_task.deadline_ns = meta.deadline_ns;  // 0.6.0: EDF 排序输入
    executor_task.qos = meta.qos;
    // CR-022: timeout_ms 在锁内从 config_ 读取（见下方临界区）。

    auto* monitor = monitor_.load(std::memory_order_acquire);
    const bool monitor_on = (monitor && monitor->is_enabled());
    std::string monitor_id;
    if (monitor_on) {
        monitor_id = executor_task.task_id;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        // CR-022: 同 try_submit —— 校验 initialized_，timeout_ms 锁内读取。
        if (!initialized_.load(std::memory_order_acquire) ||
            stop_.load(std::memory_order_acquire)) {
            return false;
        }
        executor_task.timeout_ms = config_.task_timeout_ms;

        // NN-12: 同 try_submit —— queued 诊断必须在 enqueue 之前补记。
        if (monitor_on) {
            try {
                monitor->record_task_queued(monitor_id, "default", "default");
            } catch (...) {
                // Diagnostics must never turn an accepted task into a rejection.
            }
        }

        scheduler_.enqueue(std::move(executor_task));
        total_tasks_.fetch_add(1, std::memory_order_relaxed);
    }

    // PA-1: 同 try_submit —— 入队后唤醒一个 worker，不内联派发。
    signal_work_added(false);

    return true;
}

void ThreadPool::submit_batch(std::vector<std::function<void()>> tasks) {
    (void)try_submit_batch(std::move(tasks));
}

bool ThreadPool::try_submit_batch(std::vector<std::function<void()>> tasks) {
    return try_submit_batch(std::move(tasks), {});
}

bool ThreadPool::try_submit_batch(
    std::vector<std::function<void()>> tasks,
    std::vector<std::function<void(std::exception_ptr)>> on_timeout_handlers) {
    if (tasks.empty()) {
        return false;
    }

    for (const auto& task : tasks) {
        if (!task) {
            auto exception = std::make_exception_ptr(std::invalid_argument("empty task"));
            for (auto& on_timeout : on_timeout_handlers) {
                if (on_timeout) {
                    on_timeout(exception);
                }
            }
            return false;
        }
    }

    // PA-4: 批量构造 Task（Task 含 atomic 不可移动构造，经 unique_ptr
    // 传递所有权），锁内一次 enqueue_batch 整体入队（每个优先级队列
    // 仅加锁一次，直接接管所有权）。
    const size_t batch_size = tasks.size();
    std::vector<std::unique_ptr<Task>> batched;
    batched.reserve(batch_size);

    auto* monitor = monitor_.load(std::memory_order_acquire);
    const bool monitor_on = (monitor && monitor->is_enabled());
    std::vector<std::string> monitor_ids;
    if (monitor_on) {
        monitor_ids.reserve(batch_size);
    }

    const int64_t submit_time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();

    for (size_t i = 0; i < batch_size; ++i) {
        auto executor_task = std::make_unique<Task>();
        executor_task->task_id = generate_task_id();
        executor_task->priority = TaskPriority::NORMAL;
        executor_task->function = std::move(tasks[i]);
        if (i < on_timeout_handlers.size()) {
            executor_task->on_timeout = std::move(on_timeout_handlers[i]);
        }
        // CR-025: 批内时间戳逐个递增。此前整批共享同一时间戳，同优先级
        // 比较器对批内任务恒等价，堆弹出序与提交序完全无关（实测 200/200
        // 轮乱序）。+i 的纳秒级偏移对时间语义无影响，但使比较器满足
        // priority_scheduler.hpp 承诺的同优先级 FIFO。
        executor_task->submit_time_ns = submit_time_ns + static_cast<int64_t>(i);
        // CR-022: timeout_ms 在锁内从 config_ 读取（见下方临界区）。
        if (monitor_on) {
            monitor_ids.push_back(executor_task->task_id);
        }
        batched.push_back(std::move(executor_task));
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);

        // CR-022: 同 try_submit —— 校验 initialized_，timeout_ms 锁内读取。
        if (!initialized_.load(std::memory_order_acquire) ||
            stop_.load(std::memory_order_acquire)) {
            return false;  // 线程池未初始化或已停止，拒绝任务
        }
        for (auto& executor_task : batched) {
            executor_task->timeout_ms = config_.task_timeout_ms;
        }

        // NN-12: 撤销 PA-14 的"全局锁外补记"——worker 不持有 mutex_，锁外
        // 补记可与取任务执行乱序（queued 晚于 start/complete），快照状态错、
        // 甚至 complete 先擦除后补记导致诊断表滞留僵尸条目。入队前整批补记
        // 恢复 queued→start→complete 顺序（锁序 pool.mutex_ → monitor.mutex_）。
        if (monitor_on) {
            for (const auto& id : monitor_ids) {
                try {
                    monitor->record_task_queued(id, "default", "default");
                } catch (...) {
                    // Diagnostics must never turn an accepted batch into a rejection.
                }
            }
        }

        scheduler_.enqueue_batch(batched.data(), batch_size);
        total_tasks_.fetch_add(batch_size, std::memory_order_relaxed);
    }

    // PA-1: 批次语义唤醒全部。
    signal_work_added(true);

    // 批量分发（dispatch_batch 内部按搬运结果唤醒）
    dispatch_pending_tasks(batch_size);

    return true;
}

} // namespace kairo
