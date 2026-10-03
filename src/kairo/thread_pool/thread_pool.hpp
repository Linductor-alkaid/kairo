#pragma once

#include "kairo/config.hpp"
#include "kairo/types.hpp"
#include "priority_scheduler.hpp"
#include "load_balancer.hpp"
#include "task_dispatcher.hpp"

// 使用无锁队列优化（可通过 -DUSE_LOCKFREE_WORKER_QUEUE=ON 启用）
#ifdef USE_LOCKFREE_WORKER_QUEUE
#include "lockfree_worker_queue.hpp"
#else
#include "worker_local_queue.hpp"
#endif

#include "thread_pool_resizer.hpp"
#include "../util/exception_handler.hpp"
#include "../util/thread_utils.hpp"
#include "../task/task.hpp"
#include "../monitor/task_monitor.hpp"
#include <thread>
#include <vector>
#include <memory>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <future>
#include <chrono>
#include <functional>
#include <type_traits>
#include <random>
#include <stdexcept>

namespace kairo {

#ifdef USE_LOCKFREE_WORKER_QUEUE
using WorkerQueueConcrete = LockFreeWorkerQueue;
#else
using WorkerQueueConcrete = WorkerLocalQueue;
#endif

class WorkerQueueImpl {
public:
    explicit WorkerQueueImpl(size_t capacity = 0)
        : queue_(std::make_unique<WorkerQueueConcrete>(capacity)) {}

    ~WorkerQueueImpl() = default;

    WorkerQueueImpl(const WorkerQueueImpl&) = delete;
    WorkerQueueImpl& operator=(const WorkerQueueImpl&) = delete;
    WorkerQueueImpl(WorkerQueueImpl&&) noexcept = default;
    WorkerQueueImpl& operator=(WorkerQueueImpl&&) noexcept = default;

    bool push(const Task& task) {
        return queue_->push(task);
    }

    bool push(Task&& task) {
        return queue_->push(std::move(task));
    }

    size_t push_batch(const Task* tasks, size_t n) {
        return queue_->push_batch(tasks, n);
    }

    size_t push_batch_move(std::unique_ptr<Task>* tasks, size_t n) {
        return queue_->push_batch_move(tasks, n);
    }

    bool pop(Task& task) {
        return queue_->pop(task);
    }

    bool steal(Task& task) {
        return queue_->steal(task);
    }

    size_t size() const {
        return queue_->size();
    }

    bool empty() const {
        return queue_->empty();
    }

    void clear() {
        queue_->clear();
    }

private:
    std::unique_ptr<WorkerQueueConcrete> queue_;
};

/**
 * @brief 线程池核心类
 * 
 * 管理工作线程的生命周期，从PriorityScheduler获取任务并分发给工作线程执行。
 * 支持任务提交、优先级调度、状态监控和优雅关闭。
 * 支持工作窃取、负载均衡和动态扩缩容。
 */
class ThreadPool {
public:
    /**
     * @brief 构造函数
     */
    ThreadPool();

    /**
     * @brief 析构函数
     * 
     * 自动关闭线程池并等待所有任务完成
     */
    ~ThreadPool();

    // 禁止拷贝和移动
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    /**
     * @brief 初始化线程池
     * 
     * 根据配置创建工作线程，设置线程优先级和CPU亲和性。
     * 
     * @param config 线程池配置
     * @return 如果初始化成功返回true，否则返回false
     */
    bool initialize(const ThreadPoolConfig& config);

    /**
     * @brief 提交任务（返回Future）
     * 
     * 将任务提交到线程池，使用NORMAL优先级。
     * 
     * @tparam F 可调用对象类型
     * @tparam Args 参数类型
     * @param f 可调用对象
     * @param args 参数
     * @return std::future 任务执行结果的future
     * @note 实现内部调用 try_submit，若线程池已停止则返回带 std::runtime_error("ThreadPool is stopped") 异常的 future。
     */
    template<typename F, typename... Args>
    auto submit(F&& f, Args&&... args)
        -> std::future<typename std::invoke_result<F, Args...>::type>;

    /**
     * @brief 提交任务，并报告是否被线程池接受
     *
     * @param task 任务函数
     * @return true 表示任务已入队；false 表示线程池已停止
     */
    bool try_submit(std::function<void()> task);

    bool try_submit(std::function<void()> task,
                    std::function<void(std::exception_ptr)> on_timeout);

    /**
     * @brief 提交优先级任务
     * 
     * 将任务提交到线程池，使用指定的优先级。
     * 
     * @tparam F 可调用对象类型
     * @tparam Args 参数类型
     * @param priority 优先级（0=LOW, 1=NORMAL, 2=HIGH, 3=CRITICAL）
     * @param f 可调用对象
     * @param args 参数
     * @return std::future 任务执行结果的future
     */
    template<typename F, typename... Args>
    auto submit_priority(int priority, F&& f, Args&&... args)
        -> std::future<typename std::invoke_result<F, Args...>::type>;

    /**
     * @brief 提交优先级任务，并报告是否被线程池接受
     *
     * @param priority 优先级（0=LOW, 1=NORMAL, 2=HIGH, 3=CRITICAL）
     * @param task 任务函数
     * @return true 表示任务已入队；false 表示线程池已停止
     */
    bool try_submit_priority(int priority, std::function<void()> task);

    bool try_submit_priority(int priority,
                             std::function<void()> task,
                             std::function<void(std::exception_ptr)> on_timeout);
    bool try_submit_priority(int priority,
                             std::function<void()> task,
                             std::function<void(std::exception_ptr)> on_timeout,
                             const TaskSchedulingMeta& meta);

    /**
     * @brief 批量提交任务
     *
     * 批量提交多个任务，一次获取锁，减少锁竞争开销。
     *
     * @param tasks 任务列表
     */
    void submit_batch(std::vector<std::function<void()>> tasks);

    /**
     * @brief 批量提交任务，并报告是否被线程池接受
     *
     * @param tasks 任务列表
     * @return true 表示任务已入队；false 表示线程池已停止或任务为空
     */
    bool try_submit_batch(std::vector<std::function<void()>> tasks);

    bool try_submit_batch(
        std::vector<std::function<void()>> tasks,
        std::vector<std::function<void(std::exception_ptr)>> on_timeout_handlers);

    /**
     * @brief 获取线程池状态
     * 
     * @return 线程池状态信息
     */
    ThreadPoolStatus get_status() const;

    /**
     * @brief 调整工作线程数量。
     *
     * 新数量必须位于初始化配置的 [min_threads, max_threads] 范围内。
     * 扩容会先发布新的本地队列，再启动新 worker；缩容会先迁移待执行任务，
     * 请求被移除的 worker 退出并完成 join，保证返回时状态已稳定。
     *
     * @param new_size 目标工作线程数量
     * @return 成功完成调整返回 true；线程池未运行、超出范围或无调整返回 false
     */
    bool resize(size_t new_size);

    /**
     * @brief 关闭线程池
     *
     * @param wait_for_tasks 是否等待所有任务完成（默认true）
     * @return Completed 表示外部调用者已完成关闭；RequestedFromWorker 表示
     *         当前 worker 仅请求关闭，未等待也未 join。
     */
    ShutdownResult shutdown(bool wait_for_tasks = true);

    /**
     * @brief 当前线程是否是此线程池的 worker。
     */
    bool is_current_worker_thread() const noexcept;

    /**
     * @brief 等待所有任务完成
     * 
     * 最多阻塞 kDefaultWaitForCompletionTimeout。
     */
    void wait_for_completion();

    /**
     * @brief 等待所有任务完成并返回是否完成
     *
     * @param timeout 最长等待时间
     * @return true 表示所有任务在 timeout 内完成；false 表示等待超时
     */
    bool try_wait_for_completion(std::chrono::milliseconds timeout);

    /**
     * @brief 检查线程池是否已停止
     * 
     * @return 如果线程池已停止返回true，否则返回false
     */
    bool is_stopped() const;

    /**
     * @brief 设置任务监控器（可选）
     *
     * 设置后，execute_task 前后将调用 record_task_start / record_task_complete。
     * ThreadPool 不拥有该指针；调用方必须保证 monitor 对象在所有可能
     * 已取得该指针快照的任务完成前保持存活。
     * @param m 监控器指针，可为 nullptr 表示禁用
     */
    void set_task_monitor(monitor::TaskMonitor* m);

    /**
     * @brief 获取超时任务计数
     *
     * 返回因软超时（elapsed >= timeout_ms at execution start）被跳过的任务数。
     * @return 超时任务计数
     */
    size_t get_timeout_count() const {
        return static_cast<size_t>(timeout_count_.load(std::memory_order_relaxed));
    }

    // NN-01: 测试钩子无条件参与类布局（不再受 KAIRO_THREAD_POOL_TEST_HOOKS
    // 控制）。此前宏只加在库构建上、测试 TU 可见性不受保证，宏不一致的 TU 与库
    // 对同一类型布局理解不同，构成 ODR 违例（复现为 ASAN stack-buffer-overflow）。
    // 三个空 std::function 的常驻成本可忽略，换来布局与宏彻底解耦。
    void set_worker_queue_create_hook_for_test(std::function<void(size_t)> hook) {
        worker_queue_create_hook_for_test_ = std::move(hook);
    }

    void set_worker_thread_start_hook_for_test(std::function<void(size_t)> hook) {
        worker_thread_start_hook_for_test_ = std::move(hook);
    }

    void set_worker_entry_hook_for_test(std::function<void(size_t)> hook) {
        worker_entry_hook_for_test_ = std::move(hook);
    }

private:
    /**
     * @brief 重建工作线程本地队列。
     *
     * 仅由 resize() 在 resize_mutex_ 保护下调用，保证队列数与 worker 数
     * 一致。发布前会将旧本地队列的待执行任务移回 scheduler_。
     */
    bool resize_local_queues(size_t new_num_queues);

    /**
     * @brief RAII guard that increments active_threads_ on construction
     *        and decrements on destruction. Guarantees that any exception
     *        escaping the guarded scope (including monitor callbacks inside
     *        execute_task) cannot leave active_threads_ permanently inflated
     *        and hang wait_for_completion().
     *
     * P-001 (2026-06-22): replaces the prior fetch_add/fetch_sub pair around
     * the execute_task call in worker_thread, which leaked the decrement on
     * monitor-callback exception.
     */
    class ActiveCounter {
    public:
        explicit ActiveCounter(ThreadPool& pool) noexcept
            : pool_(pool) {
            pool_.active_threads_.fetch_add(1, std::memory_order_relaxed);
        }
        ~ActiveCounter() {
            pool_.active_threads_.fetch_sub(1, std::memory_order_relaxed);
            pool_.notify_completion_waiters();
        }
        ActiveCounter(const ActiveCounter&) = delete;
        ActiveCounter& operator=(const ActiveCounter&) = delete;
        ActiveCounter(ActiveCounter&&) = delete;
        ActiveCounter& operator=(ActiveCounter&&) = delete;
    private:
        ThreadPool& pool_;
    };

    /**
     * @brief 工作线程函数
     *
     * 循环从本地队列、工作窃取或全局调度器获取任务并执行。
     *
     * @param worker_id 工作线程ID
     */
    void worker_thread(size_t worker_id);

    /**
     * @brief 执行任务
     * 
     * 执行任务并处理异常，更新统计信息。
     * 
     * @param task 任务对象
     */
    void execute_task(const Task& task);

    /**
     * @brief 更新统计信息
     * 
     * @param execution_time_ns 任务执行时间（纳秒）
     * @param success 是否成功
     * @param timed_out 是否因软超时而被跳过（P024: 超时不算 failed, 但算 completed）
     */
    void update_statistics(int64_t execution_time_ns, bool success, bool timed_out = false);

    /**
     * @brief 检查 wait_for_completion 的完整完成条件
     */
    bool is_completion_ready() const;

    /**
     * @brief 唤醒等待任务完成的调用方
     */
    void notify_completion_waiters();

    /**
     * @brief Wake workers after tasks have moved to an executable queue.
     *
     * PA-1/PA-2: 递增驻停代次计数并唤醒。wake_all 为 true 时唤醒全部
     * （批次/停止/缩容），否则只唤醒一个（单任务提交）。atomic wait
     * 的原子 check-and-block 保证 notify 早于 wait 到达也不丢失。
     */
    void signal_work_added(bool wake_all);

    /**
     * @brief Dispatch pending scheduler tasks if the dispatcher is still alive.
     */
    size_t dispatch_pending_tasks(size_t max_tasks);

    /**
     * @brief 尝试工作窃取
     *
     * 当本地队列为空时，从其他线程的本地队列窃取任务。
     *
     * @param worker_id 当前工作线程ID
     * @param task 用于接收窃取的任务
     * @return 成功窃取返回 true
     */
    bool try_steal_task(size_t worker_id, Task& task);

    /**
     * @brief try_steal_task 的内部实现
     *
     * P-260617-002: 假设调用方已持 shared_lock(local_queues_mutex_)。
     * worker_thread 的 condition_.wait 谓词需要"已持 shared_lock 时
     * 也能调窃取逻辑"，因此拆出本函数避免 std::shared_mutex 重入 UB。
     */
    bool try_steal_task_impl(size_t worker_id, Task& task);

    /**
     * @brief 检查线程是否需要退出（用于缩容）
     * 
     * @param worker_id 工作线程ID
     * @return 需要退出返回 true
     */
    bool should_exit(size_t worker_id) const;

    /**
     * @brief 监控线程函数（用于动态扩缩容）
     */
    void resize_monitor_thread();

    /**
     * @brief Roll back a failed initialize() attempt.
     */
    void rollback_initialization_failure();

    /**
     * @brief 创建新的工作线程
     * 
     * @param worker_id 工作线程ID
     */
    void create_worker_thread(size_t worker_id);

    // 配置信息
    ThreadPoolConfig config_;

    // 优先级调度器
    PriorityScheduler scheduler_;

    // 负载均衡器
    std::unique_ptr<LoadBalancer> load_balancer_;

    // 工作线程本地队列（PA-9: 由 local_queues_mutex_ 全权保护——reader 持
    // shared_lock、writer 持 unique_lock 的配对已覆盖读写两侧，不再需要
    // shared_ptr + std::atomic_load 的间接层。libstdc++ 的自由函数
    // atomic_load(shared_ptr*) 走全局自旋锁表，所有 shared_ptr 共享一池，
    // 热路径上每次 worker 迭代/派发都要串行化经过它。）
    std::unique_ptr<std::vector<WorkerQueueImpl>> local_queues_;
    // 260610P012: 专门保护 local_queues_ 的 shared_mutex
    // - steal / worker 持 shared_lock(并发读)
    // - resize / shutdown 持 unique_lock(排他写)
    // 这样既消除 UAF,又允许多个 steal 线程并发。
    mutable std::shared_mutex local_queues_mutex_;

    // 任务分发器（模板化以匹配 WorkerQueueImpl）
    std::unique_ptr<TaskDispatcher<WorkerQueueImpl>> dispatcher_;
    mutable std::mutex dispatcher_mutex_;

    // 动态扩缩容控制器
    std::unique_ptr<ThreadPoolResizer> resizer_;

    // 工作线程
    std::vector<std::thread> workers_;

    // 工作线程ID映射（用于跟踪线程）
    std::vector<size_t> worker_ids_;

    // 停止标志
    std::atomic<bool> stop_{false};

    // PA-2: 工作线程驻停代次计数。任何让任务变得可执行的路径
    // （调度器入队 / 本地队列搬运 / resize 迁移）以及 stop_/缩容退出
    // 集变化，都必须 fetch_add(release) 后 notify_one/notify_all。
    // worker 侧在完整空扫描【之前】acquire 采样 seen，然后
    // wake_seq_.wait(seen)：若代次在采样后变化（扫描期间或驻停前有
    // 新任务落位），wait 在阻塞前的原子复核立即返回；若在阻塞后变化，
    // notify 唤醒。两个方向的丢失唤醒窗口都不存在——这是 C++20
    // atomic wait 相比 condition_variable+mutex 的根本优势
    // （check-and-block 相对原子值原子）。
    // 独占缓存行：所有 worker 驻停/唤醒都 RMW 这个字（PA-12 关联）。
    alignas(64) std::atomic<uint32_t> wake_seq_{0};

    // 统计信息
    mutable std::mutex stats_mutex_;
    std::atomic<size_t> total_tasks_{0};
    std::atomic<size_t> completed_tasks_{0};
    std::atomic<size_t> failed_tasks_{0};
    std::atomic<int64_t> total_execution_time_ns_{0};
    std::atomic<size_t> active_threads_{0};
    std::atomic<int64_t> timeout_count_{0};  // P024: 软超时跳过的任务计数

    // 条件变量：用于工作线程等待任务
    std::condition_variable condition_;

    // 条件变量：用于 wait_for_completion 等待所有任务完成
    std::condition_variable completion_cv_;
    mutable std::mutex completion_mutex_;
    // CR-108: 完成等待者计数——任务终态路径仅在确有等待者时才 notify_all。
    // 此前每个任务终态无条件 2 次 notify_all（worker_thread + execute_task
    // 各一），有等待者时实测吞吐 -76.8%（惊群 × 每任务双发）；无等待者时
    // 也是纯粹的调度噪声。计数由 try_wait_for_completion 的 RAII 守卫维护。
    std::atomic<size_t> completion_waiters_{0};

    // 互斥锁：保护共享状态
    mutable std::mutex mutex_;

    // 串行化扩缩容，并与 shutdown 中的 worker join 配对，避免同一 std::thread
    // 被两个控制路径同时 join / 移除。
    std::mutex resize_mutex_;

    // shutdown() 的两阶段状态：worker 只能请求关闭，外部调用者负责最终 wait/join。
    std::mutex shutdown_mutex_;
    std::condition_variable shutdown_cv_;
    bool shutdown_started_{false};
    bool shutdown_finalizer_started_{false};
    bool shutdown_complete_{false};

    static thread_local ThreadPool* current_worker_pool_;

    // 异常处理器
    util::ExceptionHandler exception_handler_;

    // 可选任务监控器（不拥有）。允许运行中 set/unset；execute_task 每个任务
    // acquire-load 一次快照，set_task_monitor release-store 发布新指针。
    std::atomic<monitor::TaskMonitor*> monitor_{nullptr};

    // 初始化标志
    std::atomic<bool> initialized_{false};

    // 待退出的线程ID集合（用于缩容）
    mutable std::mutex exit_threads_mutex_;
    std::vector<size_t> exit_threads_;
    // PA-17: exit_threads_ 的元素计数。稳态（空集）下 should_exit 走
    // 原子 load 快速路径，不获取 exit_threads_mutex_——此前 worker 每次
    // 主循环迭代 4 个调用点全部锁 mutex + 线性扫描。计数在锁内与
    // vector 同步维护：写路径先改 vector 再 store 计数。
    mutable std::atomic<size_t> exit_threads_count_{0};

    // 监控线程（用于动态扩缩容）
    std::thread resize_monitor_thread_;
    std::atomic<bool> resize_monitor_stop_{false};
    std::condition_variable resize_monitor_cv_;
    mutable std::mutex resize_monitor_mutex_;

    // NN-01: 测试钩子无条件参与类布局（见类上方 setter 处说明），宏不再影响布局
    std::function<void(size_t)> worker_queue_create_hook_for_test_;
    std::function<void(size_t)> worker_thread_start_hook_for_test_;
    std::function<void(size_t)> worker_entry_hook_for_test_;
};

// 模板方法实现
template<typename F, typename... Args>
auto ThreadPool::submit(F&& f, Args&&... args)
    -> std::future<typename std::invoke_result<F, Args...>::type> {
    
    using return_type = typename std::invoke_result<F, Args...>::type;

    // CR-110: promise 与就绪标志合并为单次堆分配（旧版 make_shared×2，
    // 任务 lambda 捕获两个 shared_ptr 也带来双份控制块引用计数流量）。
    struct CompletionState {
        std::promise<return_type> promise;
        std::atomic_bool ready{false};
    };
    auto state = std::make_shared<CompletionState>();
    auto& promise = state->promise;
    auto& promise_ready = state->ready;
    std::future<return_type> result = promise.get_future();

    if constexpr (std::is_same_v<
                      std::remove_cv_t<std::remove_reference_t<F>>,
                      std::function<return_type(Args...)>>) {
        if (!f) {
            promise_ready.store(true, std::memory_order_release);
            promise.set_exception(
                std::make_exception_ptr(std::invalid_argument("empty task")));
            return result;
        }
    }

    auto bound_task = std::make_shared<decltype(std::bind(std::forward<F>(f), std::forward<Args>(args)...))>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...)
    );

    auto task = [state, bound_task]() mutable {
        auto& promise = state->promise;
        auto& promise_ready = state->ready;
        try {
            if constexpr (std::is_void_v<return_type>) {
                std::invoke(*bound_task);
                promise.set_value();
            } else {
                promise.set_value(std::invoke(*bound_task));
            }
            promise_ready.store(true, std::memory_order_release);
        } catch (...) {
            auto exception = std::current_exception();
            bool expected = false;
            if (promise_ready.compare_exchange_strong(expected, true)) {
                promise.set_exception(exception);
            }
            throw;
        }
    };

    auto on_timeout = [state](std::exception_ptr exception) {
        bool expected = false;
        if (state->ready.compare_exchange_strong(expected, true)) {
            state->promise.set_exception(exception);
        }
    };
    
    if (!try_submit(std::move(task), std::move(on_timeout))) {
        auto exception = std::make_exception_ptr(
            std::runtime_error("ThreadPool is stopped"));
        bool expected = false;
        if (state->ready.compare_exchange_strong(expected, true)) {
            state->promise.set_exception(exception);
        }
    }
    
    return result;
}

template<typename F, typename... Args>
auto ThreadPool::submit_priority(int priority, F&& f, Args&&... args)
    -> std::future<typename std::invoke_result<F, Args...>::type> {
    
    using return_type = typename std::invoke_result<F, Args...>::type;

    // CR-110: promise 与就绪标志合并为单次堆分配（旧版 make_shared×2，
    // 任务 lambda 捕获两个 shared_ptr 也带来双份控制块引用计数流量）。
    struct CompletionState {
        std::promise<return_type> promise;
        std::atomic_bool ready{false};
    };
    auto state = std::make_shared<CompletionState>();
    auto& promise = state->promise;
    auto& promise_ready = state->ready;
    std::future<return_type> result = promise.get_future();

    if constexpr (std::is_same_v<
                      std::remove_cv_t<std::remove_reference_t<F>>,
                      std::function<return_type(Args...)>>) {
        if (!f) {
            promise_ready.store(true, std::memory_order_release);
            promise.set_exception(
                std::make_exception_ptr(std::invalid_argument("empty task")));
            return result;
        }
    }

    auto bound_task = std::make_shared<decltype(std::bind(std::forward<F>(f), std::forward<Args>(args)...))>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...)
    );

    auto task = [state, bound_task]() mutable {
        auto& promise = state->promise;
        auto& promise_ready = state->ready;
        try {
            if constexpr (std::is_void_v<return_type>) {
                std::invoke(*bound_task);
                promise.set_value();
            } else {
                promise.set_value(std::invoke(*bound_task));
            }
            promise_ready.store(true, std::memory_order_release);
        } catch (...) {
            auto exception = std::current_exception();
            bool expected = false;
            if (promise_ready.compare_exchange_strong(expected, true)) {
                promise.set_exception(exception);
            }
            throw;
        }
    };

    auto on_timeout = [state](std::exception_ptr exception) {
        bool expected = false;
        if (state->ready.compare_exchange_strong(expected, true)) {
            state->promise.set_exception(exception);
        }
    };
    
    if (!try_submit_priority(priority, std::move(task), std::move(on_timeout))) {
        auto exception = std::make_exception_ptr(
            std::runtime_error("ThreadPool is stopped"));
        bool expected = false;
        if (state->ready.compare_exchange_strong(expected, true)) {
            state->promise.set_exception(exception);
        }
    }
    
    return result;
}

} // namespace kairo
