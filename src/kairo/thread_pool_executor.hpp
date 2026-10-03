#pragma once

#include "kairo/interfaces.hpp"
#include "kairo/config.hpp"
#include "kairo/types.hpp"
#include "thread_pool/thread_pool.hpp"
#include "kairo/monitor/task_monitor.hpp"
#include <string>
#include <memory>
#include <mutex>

namespace kairo {

/**
 * @brief 线程池执行器
 * 
 * 实现 IAsyncExecutor 接口，封装 ThreadPool，提供异步任务执行功能。
 * 用于处理普通并发任务，支持任务提交、优先级调度、状态监控等。
 */
class ThreadPoolExecutor : public IAsyncExecutor {
public:
    /**
     * @brief 构造函数
     * 
     * @param name 执行器名称
     * @param config 线程池配置
     */
    ThreadPoolExecutor(const std::string& name, const ThreadPoolConfig& config);

    /**
     * @brief 析构函数
     * 
     * 自动停止执行器并等待所有任务完成
     */
    ~ThreadPoolExecutor();

    // 禁止拷贝和移动
    ThreadPoolExecutor(const ThreadPoolExecutor&) = delete;
    ThreadPoolExecutor& operator=(const ThreadPoolExecutor&) = delete;
    ThreadPoolExecutor(ThreadPoolExecutor&&) = delete;
    ThreadPoolExecutor& operator=(ThreadPoolExecutor&&) = delete;

    /**
     * @brief 获取执行器名称
     * @return 执行器名称
     */
    std::string get_name() const override;

    /**
     * @brief 获取执行器状态
     * @return 异步执行器状态
     */
    AsyncExecutorStatus get_status() const override;

    /**
     * @brief 启动执行器
     * 
     * 初始化线程池并启动工作线程。
     * 
     * @return 如果启动成功返回true，否则返回false
     */
    bool start() override;

    /**
     * @brief 停止执行器
     * 
     * 关闭线程池，默认等待所有任务完成。
     */
    void stop() override;

    /**
     * @brief 停止执行器
     *
     * @param wait_for_tasks 是否等待所有任务完成
     */
    void stop(bool wait_for_tasks) override;

    bool is_current_worker_thread() const noexcept override;

    /**
     * @brief 等待所有任务完成
     * 
     * 阻塞直到所有已提交的任务执行完成。
     */
    void wait_for_completion() override;

    /**
     * @brief 等待所有任务完成并返回是否完成
     */
    bool try_wait_for_completion(std::chrono::milliseconds timeout) override;

    /**
     * @brief 设置任务监控器（可选）
     */
    void set_task_monitor(monitor::TaskMonitor* m);

protected:
    /**
     * @brief 提交任务实现（内部方法）
     * 
     * 将任务提交到线程池执行。
     * 
     * @param task 任务函数
     */
    void submit_impl(std::function<void()> task) override;

    /**
     * @brief 提交任务实现（可报告拒绝）
     *
     * @param task 任务函数
     * @return true 表示任务已被线程池接受；false 表示线程池已停止
     */
    bool try_submit_impl(std::function<void()> task) override;

    bool try_submit_with_timeout_impl(
        std::function<void()> task,
        std::function<void(std::exception_ptr)> on_timeout) override;

    /**
     * @brief 提交优先级任务实现（内部方法）
     *
     * 将任务提交到线程池执行，使用指定优先级。
     *
     * @param priority 优先级（0=LOW, 1=NORMAL, 2=HIGH, 3=CRITICAL）
     * @param task 任务函数
     */
    void submit_priority_impl(int priority, std::function<void()> task) override;

    /**
     * @brief 提交优先级任务实现（可报告拒绝）
     *
     * @param priority 优先级（0=LOW, 1=NORMAL, 2=HIGH, 3=CRITICAL）
     * @param task 任务函数
     * @return true 表示任务已被线程池接受；false 表示线程池已停止
     */
    bool try_submit_priority_impl(int priority, std::function<void()> task) override;

    bool try_submit_priority_with_timeout_impl(
        int priority,
        std::function<void()> task,
        std::function<void(std::exception_ptr)> on_timeout) override;

    /**
     * @brief 批量提交任务实现（内部方法）
     *
     * 优化的批量提交，减少锁竞争。
     *
     * @param tasks 任务列表
     */
    void submit_batch_impl(std::vector<std::function<void()>> tasks) override;

    /**
     * @brief 批量提交任务实现（可报告拒绝）
     *
     * @param tasks 任务列表
     * @return true 表示任务已被线程池接受；false 表示线程池已停止
     */
    bool try_submit_batch_impl(std::vector<std::function<void()>> tasks) override;

    bool try_submit_batch_with_timeout_impl(
        std::vector<std::function<void()>> tasks,
        std::vector<std::function<void(std::exception_ptr)>> on_timeout_handlers) override;

private:
    // 对 stop(false) 退休的池做终局排空：逐个 shutdown(true)，经 ThreadPool
    // 的 shutdown_cv_ 等待 detached 终结线程 join 完全部 worker 后返回。
    void drain_retired_pools();

    std::string name_;              // 执行器名称
    ThreadPoolConfig config_;       // 线程池配置
    mutable std::mutex thread_pool_mutex_;
    std::shared_ptr<ThreadPool> thread_pool_;  // 线程池实例
    // stop(false) 退休的池。stop(false) 把池交给 detached 终结线程后立即
    // 返回（契约），但 wrapper 闭包捕获裸 facade 指针且仍可能在 worker 上
    // 运行：后续 stop(true) 与管理器终局必须经 retired_pools_ 等到全部
    // worker join，才能安全析构 facade/manager 状态（UAF 闭环，
    // 见 docs/design/dependency_driven_scheduling.md 生命周期一节）。
    std::vector<std::shared_ptr<ThreadPool>> retired_pools_;
};

} // namespace kairo
