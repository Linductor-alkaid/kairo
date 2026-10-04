#include "thread_pool_executor.hpp"
#include <stdexcept>
#include <thread>

namespace kairo {

ThreadPoolExecutor::ThreadPoolExecutor(const std::string& name, const ThreadPoolConfig& config)
    : name_(name), config_(config), thread_pool_(std::make_shared<ThreadPool>()) {
}

ThreadPoolExecutor::~ThreadPoolExecutor() {
    stop();
}

std::string ThreadPoolExecutor::get_name() const {
    return name_;
}

AsyncExecutorStatus ThreadPoolExecutor::get_status() const {
    AsyncExecutorStatus status;
    status.name = name_;

    std::shared_ptr<ThreadPool> thread_pool;
    {
        std::lock_guard<std::mutex> lock(thread_pool_mutex_);
        thread_pool = thread_pool_;
    }
    if (!thread_pool) {
        status.is_running = false;
        return status;
    }
    
    // 获取线程池状态
    ThreadPoolStatus pool_status = thread_pool->get_status();
    
    // 映射线程池状态到异步执行器状态
    // 如果total_threads == 0,说明ThreadPool还没有初始化,不应该被认为是运行中
    status.is_running = (pool_status.total_threads > 0) && !thread_pool->is_stopped();
    status.active_tasks = pool_status.active_threads;  // 活跃线程数作为活跃任务数
    status.completed_tasks = pool_status.completed_tasks;
    status.failed_tasks = pool_status.failed_tasks;
    status.queue_size = pool_status.queue_size;
    status.avg_task_time_ms = pool_status.avg_task_time_ms;
    
    return status;
}

bool ThreadPoolExecutor::start() {
    std::shared_ptr<ThreadPool> thread_pool;
    {
        std::lock_guard<std::mutex> lock(thread_pool_mutex_);
        if (!thread_pool_) {
            thread_pool_ = std::make_shared<ThreadPool>();
        }
        thread_pool = thread_pool_;
    }

    // 初始化线程池
    return thread_pool->initialize(config_);
}

void ThreadPoolExecutor::stop() {
    stop(true);
}

void ThreadPoolExecutor::stop(bool wait_for_tasks) {
    std::shared_ptr<ThreadPool> thread_pool;
    bool caller_is_worker = false;
    bool have_pool = false;
    {
        std::lock_guard<std::mutex> lock(thread_pool_mutex_);
        if (thread_pool_) {
            thread_pool = thread_pool_;
            have_pool = true;
            caller_is_worker = thread_pool->is_current_worker_thread();
            // A worker-origin request must retain ownership until an external
            // caller finalizes and joins the pool.
            if (!caller_is_worker) {
                thread_pool_.reset();
                // 退休池保活：stop(false) 的 detached 终结线程持有自己的
                // shared_ptr，这里再留一份可达引用，供后续 stop(true)（含
                // ~Executor → ~ExecutorManager 路径）等待 worker 全部 join。
                retired_pools_.push_back(thread_pool);
            }
        }
    }

    if (have_pool && caller_is_worker) {
        thread_pool->shutdown(true);
    } else if (have_pool && wait_for_tasks) {
        thread_pool->shutdown(true);
        drain_retired_pools();
    } else if (have_pool) {
        std::thread([thread_pool = std::move(thread_pool)]() {
            thread_pool->shutdown(false);
        }).detach();
        // stop(false) 契约是立即返回：退休池留给后续 stop(true)/终局排空。
    } else if (wait_for_tasks) {
        // 此前已 stop(false) 过的执行器再次 stop(true)：池本体已退休，
        // 这里只负责等到退休池的 worker 全部 join。
        drain_retired_pools();
    }
}

void ThreadPoolExecutor::drain_retired_pools() {
    std::vector<std::shared_ptr<ThreadPool>> retired;
    {
        std::lock_guard<std::mutex> lock(thread_pool_mutex_);
        retired.swap(retired_pools_);
    }
    for (const auto& retired_pool : retired) {
        if (!retired_pool) {
            continue;
        }
        // 第二调用方经 shutdown_cv_ 等待 detached 终结线程完成 join；若
        // detached 线程尚未启动，本调用成为 finalizer 亲自 join。两个顺序
        // 都恰好有一个 joiner，不会重复或遗漏。
        retired_pool->shutdown(true);
    }
}

bool ThreadPoolExecutor::is_current_worker_thread() const noexcept {
    std::lock_guard<std::mutex> lock(thread_pool_mutex_);
    return thread_pool_ && thread_pool_->is_current_worker_thread();
}

void ThreadPoolExecutor::wait_for_completion() {
    (void)try_wait_for_completion(kDefaultWaitForCompletionTimeout);
}

bool ThreadPoolExecutor::try_wait_for_completion(std::chrono::milliseconds timeout) {
    std::shared_ptr<ThreadPool> thread_pool;
    {
        std::lock_guard<std::mutex> lock(thread_pool_mutex_);
        thread_pool = thread_pool_;
    }
    if (!thread_pool) {
        return true;
    }

    return thread_pool->try_wait_for_completion(timeout);
}

void ThreadPoolExecutor::set_task_monitor(monitor::TaskMonitor* m) {
    std::shared_ptr<ThreadPool> thread_pool;
    {
        std::lock_guard<std::mutex> lock(thread_pool_mutex_);
        thread_pool = thread_pool_;
    }
    if (thread_pool) {
        thread_pool->set_task_monitor(m);
    }
}

void ThreadPoolExecutor::submit_impl(std::function<void()> task) {
    std::shared_ptr<ThreadPool> thread_pool;
    {
        std::lock_guard<std::mutex> lock(thread_pool_mutex_);
        thread_pool = thread_pool_;
    }
    if (!thread_pool) {
        throw std::runtime_error("ThreadPool is stopped");
    }

    // 将任务提交到线程池（使用NORMAL优先级）
    thread_pool->submit(std::move(task));
}

bool ThreadPoolExecutor::try_submit_impl(std::function<void()> task) {
    std::shared_ptr<ThreadPool> thread_pool;
    {
        std::lock_guard<std::mutex> lock(thread_pool_mutex_);
        thread_pool = thread_pool_;
    }
    if (!thread_pool) {
        return false;
    }

    // 将任务提交到线程池（使用NORMAL优先级），并报告停止后的拒绝
    return thread_pool->try_submit(std::move(task));
}

bool ThreadPoolExecutor::try_submit_with_timeout_impl(
    std::function<void()> task,
    std::function<void(std::exception_ptr)> on_timeout) {
    std::shared_ptr<ThreadPool> thread_pool;
    {
        std::lock_guard<std::mutex> lock(thread_pool_mutex_);
        thread_pool = thread_pool_;
    }
    if (!thread_pool) {
        return false;
    }

    return thread_pool->try_submit(std::move(task), std::move(on_timeout));
}

void ThreadPoolExecutor::submit_priority_impl(int priority, std::function<void()> task) {
    std::shared_ptr<ThreadPool> thread_pool;
    {
        std::lock_guard<std::mutex> lock(thread_pool_mutex_);
        thread_pool = thread_pool_;
    }
    if (!thread_pool) {
        throw std::runtime_error("ThreadPool is stopped");
    }

    // 将任务提交到线程池（使用指定优先级）
    thread_pool->submit_priority(priority, std::move(task));
}

bool ThreadPoolExecutor::try_submit_priority_impl(int priority, std::function<void()> task) {
    std::shared_ptr<ThreadPool> thread_pool;
    {
        std::lock_guard<std::mutex> lock(thread_pool_mutex_);
        thread_pool = thread_pool_;
    }
    if (!thread_pool) {
        return false;
    }

    return thread_pool->try_submit_priority(priority, std::move(task));
}

bool ThreadPoolExecutor::try_submit_priority_with_timeout_meta_impl(
    int priority,
    std::function<void()> task,
    std::function<void(std::exception_ptr)> on_timeout,
    const TaskSchedulingMeta& meta) {
    std::shared_ptr<ThreadPool> thread_pool;
    {
        std::lock_guard<std::mutex> lock(thread_pool_mutex_);
        thread_pool = thread_pool_;
    }
    if (!thread_pool) {
        return false;
    }

    return thread_pool->try_submit_priority(priority, std::move(task),
                                            std::move(on_timeout), meta);
}

bool ThreadPoolExecutor::try_submit_priority_with_timeout_impl(
    int priority,
    std::function<void()> task,
    std::function<void(std::exception_ptr)> on_timeout) {
    std::shared_ptr<ThreadPool> thread_pool;
    {
        std::lock_guard<std::mutex> lock(thread_pool_mutex_);
        thread_pool = thread_pool_;
    }
    if (!thread_pool) {
        return false;
    }

    return thread_pool->try_submit_priority(
        priority, std::move(task), std::move(on_timeout));
}

void ThreadPoolExecutor::submit_batch_impl(std::vector<std::function<void()>> tasks) {
    std::shared_ptr<ThreadPool> thread_pool;
    {
        std::lock_guard<std::mutex> lock(thread_pool_mutex_);
        thread_pool = thread_pool_;
    }
    if (!thread_pool) {
        throw std::runtime_error("ThreadPool is stopped");
    }

    // 调用 ThreadPool 的批量提交方法（一次获取锁）
    thread_pool->submit_batch(std::move(tasks));
}

bool ThreadPoolExecutor::try_submit_batch_impl(std::vector<std::function<void()>> tasks) {
    std::shared_ptr<ThreadPool> thread_pool;
    {
        std::lock_guard<std::mutex> lock(thread_pool_mutex_);
        thread_pool = thread_pool_;
    }
    if (!thread_pool) {
        return false;
    }

    // 调用 ThreadPool 的可报告批量提交方法（一次获取锁）
    return thread_pool->try_submit_batch(std::move(tasks));
}

bool ThreadPoolExecutor::try_submit_batch_with_timeout_impl(
    std::vector<std::function<void()>> tasks,
    std::vector<std::function<void(std::exception_ptr)>> on_timeout_handlers) {
    std::shared_ptr<ThreadPool> thread_pool;
    {
        std::lock_guard<std::mutex> lock(thread_pool_mutex_);
        thread_pool = thread_pool_;
    }
    if (!thread_pool) {
        return false;
    }

    return thread_pool->try_submit_batch(
        std::move(tasks), std::move(on_timeout_handlers));
}

} // namespace kairo
