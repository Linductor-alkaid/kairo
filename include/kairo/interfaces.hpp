#pragma once

#include "types.hpp"
#include <string>
#include <future>
#include <functional>
#include <type_traits>
#include <vector>
#include <deque>
#include <thread>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <utility>
#include <iterator>

namespace kairo {

namespace detail {

template<typename T>
struct is_std_function : std::false_type {};

template<typename Signature>
struct is_std_function<std::function<Signature>> : std::true_type {};

template<typename T>
inline constexpr bool is_std_function_v =
    is_std_function<std::remove_cv_t<std::remove_reference_t<T>>>::value;

template<typename F>
bool is_empty_std_function(const F& f) {
    if constexpr (is_std_function_v<F>) {
        return !f;
    } else {
        return false;
    }
}

} // namespace detail

/**
 * @brief 异步执行器接口（用于线程池）
 * 
 * 提供任务提交、状态查询、生命周期管理等功能
 */
class IAsyncExecutor {
public:
    virtual ~IAsyncExecutor() = default;

    /**
     * @brief 提交任务（返回Future）
     * 
     * @tparam F 可调用对象类型
     * @tparam Args 参数类型
     * @param f 可调用对象
     * @param args 参数
     * @return std::future 任务执行结果的future
     */
    template<typename F, typename... Args>
    auto submit(F&& f, Args&&... args)
        -> std::future<typename std::invoke_result<F, Args...>::type> {
        using return_type = typename std::invoke_result<F, Args...>::type;

        auto promise = std::make_shared<std::promise<return_type>>();
        auto promise_ready = std::make_shared<std::atomic_bool>(false);
        std::future<return_type> result = promise->get_future();

        if constexpr (sizeof...(Args) == 0) {
            if (detail::is_empty_std_function(f)) {
                promise_ready->store(true, std::memory_order_release);
                promise->set_exception(std::make_exception_ptr(
                    std::invalid_argument("empty task")));
                return result;
            }
        }

        auto bound_task = std::make_shared<decltype(std::bind(std::forward<F>(f), std::forward<Args>(args)...))>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );

        auto task = [promise, promise_ready, bound_task]() mutable {
            try {
                if constexpr (std::is_void_v<return_type>) {
                    std::invoke(*bound_task);
                    promise->set_value();
                } else {
                    promise->set_value(std::invoke(*bound_task));
                }
                promise_ready->store(true, std::memory_order_release);
            } catch (...) {
                promise->set_exception(std::current_exception());
                promise_ready->store(true, std::memory_order_release);
                throw;
            }
        };

        auto on_timeout = [promise, promise_ready](std::exception_ptr exception) {
            bool expected = false;
            if (promise_ready->compare_exchange_strong(expected, true)) {
                promise->set_exception(exception);
            }
        };

        if (!try_submit_with_timeout_impl(std::move(task), std::move(on_timeout))) {
            bool expected = false;
            if (promise_ready->compare_exchange_strong(expected, true)) {
                promise->set_exception(std::make_exception_ptr(
                    std::runtime_error("Executor is stopped")
                ));
            }
        }

        return result;
    }

    /**
     * @brief 获取执行器名称
     * @return 执行器名称
     */
    virtual std::string get_name() const = 0;

    /**
     * @brief 获取执行器状态
     * @return 异步执行器状态
     */
    virtual AsyncExecutorStatus get_status() const = 0;

    /**
     * @brief 启动执行器
     * @return 是否启动成功
     */
    virtual bool start() = 0;

    /**
     * @brief 停止执行器
     */
    virtual void stop() = 0;

    /**
     * @brief 停止执行器
     * @param wait_for_tasks 是否等待已提交任务完成
     */
    virtual void stop(bool wait_for_tasks) {
        (void)wait_for_tasks;
        stop();
    }

    /**
     * @brief Whether the caller is a worker owned by this executor.
     */
    virtual bool is_current_worker_thread() const noexcept {
        return false;
    }

    /**
     * @brief 等待所有任务完成
     */
    virtual void wait_for_completion() = 0;

    /**
     * @brief 等待所有任务完成并返回是否完成
     *
     * @param timeout 最长等待时间
     * @return true 表示所有任务在 timeout 内完成；false 表示等待超时
     */
    virtual bool try_wait_for_completion(std::chrono::milliseconds timeout) {
        (void)timeout;
        wait_for_completion();
        return true;
    }

    /**
     * @brief 提交已包装任务，并报告是否被执行器接受
     */
    bool try_submit_task(std::function<void()> task) {
        try {
            return try_submit_impl(std::move(task));
        } catch (...) {
            return false;
        }
    }

    bool try_submit_task(std::function<void()> task,
                         std::function<void(std::exception_ptr)> on_timeout) {
        try {
            return try_submit_with_timeout_impl(std::move(task), std::move(on_timeout));
        } catch (...) {
            return false;
        }
    }

    /**
     * @brief 提交优先级任务（返回Future）
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
        -> std::future<typename std::invoke_result<F, Args...>::type> {
        using return_type = typename std::invoke_result<F, Args...>::type;

        auto promise = std::make_shared<std::promise<return_type>>();
        auto promise_ready = std::make_shared<std::atomic_bool>(false);
        std::future<return_type> result = promise->get_future();

        if constexpr (sizeof...(Args) == 0) {
            if (detail::is_empty_std_function(f)) {
                promise_ready->store(true, std::memory_order_release);
                promise->set_exception(std::make_exception_ptr(
                    std::invalid_argument("empty task")));
                return result;
            }
        }

        auto bound_task = std::make_shared<decltype(std::bind(std::forward<F>(f), std::forward<Args>(args)...))>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );

        auto task = [promise, promise_ready, bound_task]() mutable {
            try {
                if constexpr (std::is_void_v<return_type>) {
                    std::invoke(*bound_task);
                    promise->set_value();
                } else {
                    promise->set_value(std::invoke(*bound_task));
                }
                promise_ready->store(true, std::memory_order_release);
            } catch (...) {
                promise->set_exception(std::current_exception());
                promise_ready->store(true, std::memory_order_release);
                throw;
            }
        };

        auto on_timeout = [promise, promise_ready](std::exception_ptr exception) {
            bool expected = false;
            if (promise_ready->compare_exchange_strong(expected, true)) {
                promise->set_exception(exception);
            }
        };

        if (!try_submit_priority_with_timeout_impl(
                priority, std::move(task), std::move(on_timeout))) {
            bool expected = false;
            if (promise_ready->compare_exchange_strong(expected, true)) {
                promise->set_exception(std::make_exception_ptr(
                    std::runtime_error("Executor is stopped")
                ));
            }
        }
        return result;
    }

    /**
     * @brief 提交已包装优先级任务，并报告是否被执行器接受
     */
    bool try_submit_priority_task(int priority, std::function<void()> task) {
        try {
            return try_submit_priority_impl(priority, std::move(task));
        } catch (...) {
            return false;
        }
    }

    bool try_submit_priority_task(
        int priority,
        std::function<void()> task,
        std::function<void(std::exception_ptr)> on_timeout) {
        try {
            return try_submit_priority_with_timeout_impl(
                priority, std::move(task), std::move(on_timeout));
        } catch (...) {
            return false;
        }
    }

    /**
     * @brief 批量提交任务
     *
     * 批量提交多个任务，内部优化减少锁竞争。
     *
     * @tparam F 可调用对象类型
     * @param tasks 任务列表
     * @return std::vector<std::future<void>> 任务执行结果的 future 列表
     */
    template<typename F>
    std::vector<std::future<void>> submit_batch(const std::vector<F>& tasks) {
        std::vector<std::function<void()>> task_wrappers;
        std::vector<std::function<void(std::exception_ptr)>> timeout_handlers;
        std::vector<std::future<void>> futures;
        std::vector<std::shared_ptr<std::promise<void>>> promises;
        std::vector<std::shared_ptr<std::atomic_bool>> promise_ready_flags;

        task_wrappers.reserve(tasks.size());
        timeout_handlers.reserve(tasks.size());
        futures.reserve(tasks.size());
        promises.reserve(tasks.size());
        promise_ready_flags.reserve(tasks.size());

        // 准备所有任务和 future
        bool has_empty_task = false;
        for (const auto& task : tasks) {
            auto promise = std::make_shared<std::promise<void>>();
            auto promise_ready = std::make_shared<std::atomic_bool>(false);
            futures.push_back(promise->get_future());
            promises.push_back(promise);
            promise_ready_flags.push_back(promise_ready);
            if (detail::is_empty_std_function(task)) {
                has_empty_task = true;
            }
            task_wrappers.push_back([promise, promise_ready, task]() {
                try {
                    task();
                    promise->set_value();
                    promise_ready->store(true, std::memory_order_release);
                } catch (...) {
                    promise->set_exception(std::current_exception());
                    promise_ready->store(true, std::memory_order_release);
                    throw;
                }
            });
            timeout_handlers.push_back([promise, promise_ready](std::exception_ptr exception) {
                bool expected = false;
                if (promise_ready->compare_exchange_strong(expected, true)) {
                    promise->set_exception(exception);
                }
            });
        }

        if (has_empty_task) {
            auto exception = std::make_exception_ptr(std::invalid_argument("empty task"));
            for (size_t i = 0; i < promises.size(); ++i) {
                promise_ready_flags[i]->store(true, std::memory_order_release);
                promises[i]->set_exception(exception);
            }
            return futures;
        }

        // 批量提交（减少锁竞争）
        if (!try_submit_batch_with_timeout_impl(
                std::move(task_wrappers), std::move(timeout_handlers))) {
            for (size_t i = 0; i < promises.size(); ++i) {
                bool expected = false;
                if (promise_ready_flags[i]->compare_exchange_strong(expected, true)) {
                    promises[i]->set_exception(std::make_exception_ptr(
                        std::runtime_error("ThreadPool is stopped")
                    ));
                }
            }
        }

        return futures;
    }

    /**
     * @brief 批量提交已包装任务，并报告是否被执行器接受
     */
    bool try_submit_batch_tasks(std::vector<std::function<void()>> tasks) {
        try {
            return try_submit_batch_impl(std::move(tasks));
        } catch (...) {
            return false;
        }
    }

    bool try_submit_batch_tasks(
        std::vector<std::function<void()>> tasks,
        std::vector<std::function<void(std::exception_ptr)>> on_timeout_handlers) {
        try {
            return try_submit_batch_with_timeout_impl(
                std::move(tasks), std::move(on_timeout_handlers));
        } catch (...) {
            return false;
        }
    }

    /**
     * @brief 批量提交任务（无 future 版本）
     *
     * 批量提交多个任务，不返回 future，性能更高。
     *
     * @tparam F 可调用对象类型
     * @param tasks 任务列表
     */
    template<typename F>
    void submit_batch_no_future(const std::vector<F>& tasks) {
        std::vector<std::function<void()>> task_wrappers;
        task_wrappers.reserve(tasks.size());

        for (const auto& task : tasks) {
            task_wrappers.push_back(task);
        }

        submit_batch_impl(std::move(task_wrappers));
    }

protected:
    /**
     * @brief 提交任务实现（内部方法）
     * @param task 任务函数
     */
    virtual void submit_impl(std::function<void()> task) = 0;

    /**
     * @brief 提交任务实现（可报告拒绝）
     * @param task 任务函数
     * @return true 表示任务已被接受；false 表示执行器已拒绝任务
     */
    virtual bool try_submit_impl(std::function<void()> task) {
        try {
            submit_impl(std::move(task));
            return true;
        } catch (...) {
            return false;
        }
    }

    virtual bool try_submit_with_timeout_impl(
        std::function<void()> task,
        std::function<void(std::exception_ptr)> on_timeout) {
        (void)on_timeout;
        return try_submit_impl(std::move(task));
    }

    /**
     * @brief 提交优先级任务实现（内部方法）
     * @param priority 优先级（0=LOW, 1=NORMAL, 2=HIGH, 3=CRITICAL）
     * @param task 任务函数
     */
    virtual void submit_priority_impl(int priority, std::function<void()> task) {
        // 默认实现：忽略优先级，使用普通提交
        (void)priority;
        submit_impl(std::move(task));
    }

    /**
     * @brief 提交优先级任务实现（可报告拒绝）
     * @param priority 优先级（0=LOW, 1=NORMAL, 2=HIGH, 3=CRITICAL）
     * @param task 任务函数
     * @return true 表示任务已被接受；false 表示执行器已拒绝任务
     */
    virtual bool try_submit_priority_impl(int priority, std::function<void()> task) {
        try {
            submit_priority_impl(priority, std::move(task));
            return true;
        } catch (...) {
            return false;
        }
    }

    virtual bool try_submit_priority_with_timeout_impl(
        int priority,
        std::function<void()> task,
        std::function<void(std::exception_ptr)> on_timeout) {
        (void)on_timeout;
        return try_submit_priority_impl(priority, std::move(task));
    }

    /**
     * @brief 批量提交任务实现（内部方法）
     * @param tasks 任务列表
     */
    virtual void submit_batch_impl(std::vector<std::function<void()>> tasks) {
        // 默认实现：逐个提交
        for (auto& task : tasks) {
            submit_impl(std::move(task));
        }
    }

    /**
     * @brief 批量提交任务实现（可报告拒绝）
     * @param tasks 任务列表
     * @return true 表示任务已被接受；false 表示执行器已拒绝整批任务
     */
    virtual bool try_submit_batch_impl(std::vector<std::function<void()>> tasks) {
        submit_batch_impl(std::move(tasks));
        return true;
    }

    virtual bool try_submit_batch_with_timeout_impl(
        std::vector<std::function<void()>> tasks,
        std::vector<std::function<void(std::exception_ptr)>> on_timeout_handlers) {
        (void)on_timeout_handlers;
        return try_submit_batch_impl(std::move(tasks));
    }
};

/**
 * @brief 实时执行器接口（用于专用实时线程）
 * 
 * 提供实时线程的启动、停止、任务推送等功能
 * 注意：实时执行器不提供 submit() 接口，因为实时线程是周期执行的
 */
class IRealtimeExecutor {
public:
    virtual ~IRealtimeExecutor() = default;

    /**
     * @brief 启动实时线程
     * @return 是否启动成功
     */
    virtual bool start() = 0;

    /**
     * @brief 停止实时线程
     */
    virtual void stop() = 0;

    /**
     * @brief 推送任务到无锁队列（在周期回调中处理）
     *
     * 任务通过无锁队列传递，在实时线程的下一个周期回调中执行。
     *
     * P-001 (260615) 曾为 ABI 兼容保持 void 签名、以 push_task_ex 扩展返回值；
     * 0.6.0 起接口直接返回 ExecutorResult，背压失败同时通过返回值与
     * RealtimeExecutorStatus::dropped_task_count / failed_pushes 可见。
     *
     * @param task 任务函数
     * @return ExecutorResult 任务被接受时 ok；队列满/未运行/空任务时携带失败类别
     */
    virtual ExecutorResult push_task(std::function<void()> task) = 0;

    /**
     * @brief 获取执行器名称
     * @return 执行器名称
     */
    virtual std::string get_name() const = 0;

    /**
     * @brief 获取执行器状态
     * @return 实时执行器状态
     */
    virtual RealtimeExecutorStatus get_status() const = 0;
};

/**
 * @brief 周期管理器接口（可选，用于更精确的周期控制和监控）
 * 
 * 如果不提供周期管理器，executor使用内置的简单周期实现（sleep_until）
 * 对于需要精确周期控制的场景，可以实现此接口并注入到RealtimeThreadConfig中
 */
class ICycleManager {
public:
    virtual ~ICycleManager() = default;

    /**
     * @brief 注册周期任务
     * 
     * @param name 周期任务名称
     * @param period_ns 周期（纳秒）
     * @param callback 周期回调函数
     * @return 是否注册成功
     */
    // Implementations may throw; RealtimeThreadExecutor catches and reports
    // such failures before falling back to its built-in cycle loop.
    virtual bool register_cycle(const std::string& name,
                                int64_t period_ns,
                                std::function<void()> callback) noexcept(false) = 0;

    /**
     * @brief 启动周期任务
     * 
     * @param name 周期任务名称
     * @return 是否启动成功
     */
    virtual bool start_cycle(const std::string& name) noexcept(false) = 0;

    /**
     * @brief 停止周期任务
     * 
     * @param name 周期任务名称
     *
     * The manager may invoke the registered callback synchronously from this
     * call. That callback may safely call RealtimeThreadExecutor::stop() or
     * stop_and_join(); the executor does not hold its lifecycle mutex while
     * invoking stop_cycle().
     */
    virtual void stop_cycle(const std::string& name) noexcept(false) = 0;

    /**
     * @brief 获取周期统计信息（可选）
     * 
     * @param name 周期任务名称
     * @return 周期统计信息
     */
    virtual CycleStatistics get_statistics(const std::string& name) const = 0;
};

/**
 * @brief GPU 执行器接口
 * 
 * 提供 GPU kernel 任务提交、内存管理、流管理等功能
 */
class IGpuExecutor {
public:
    virtual ~IGpuExecutor() = default;

    /**
     * @brief 提交 GPU kernel 任务
     * 
     * @tparam KernelFunc GPU kernel 函数类型
     * @param kernel GPU kernel 函数（类型擦除，通过回调执行）
     * @param config GPU 任务配置
     * @return std::future<void> 任务执行结果的 future
     */
    template<typename KernelFunc>
    auto submit_kernel(KernelFunc&& kernel, const gpu::GpuTaskConfig& config)
        -> std::future<void>;

    /**
     * @brief 批量提交 GPU kernel 任务
     *
     * 在同一把锁内连续入队，减少多次 submit 时的锁竞争。
     *
     * @param tasks 任务列表，每项为 (kernel 函数, 配置)
     * @return 与 tasks 一一对应的 future 列表
     */
    virtual std::vector<std::future<void>> submit_kernels_batch(
        const std::vector<std::pair<std::function<void(void*)>, gpu::GpuTaskConfig>>& tasks);

    /**
     * @brief 在依赖完成后提交 GPU kernel 任务
     *
     * dependency 完成后才执行 kernel，priority 仍来自 config。
     *
     * @tparam KernelFunc GPU kernel 函数类型
     * @param dependency 依赖的 future，需先完成
     * @param kernel GPU kernel 函数
     * @param config GPU 任务配置
     * @return std::future<void> 任务执行结果的 future
     */
    template<typename KernelFunc>
    auto submit_kernel_after(std::shared_future<void> dependency, KernelFunc&& kernel,
                             const gpu::GpuTaskConfig& config) -> std::future<void>;

    /**
     * @brief 分配设备内存
     * 
     * @param size 内存大小（字节）
     * @return 设备内存指针，失败返回 nullptr
     */
    virtual void* allocate_device_memory(size_t size) = 0;

    /**
     * @brief 释放设备内存
     * 
     * @param ptr 设备内存指针
     */
    virtual void free_device_memory(void* ptr) = 0;

    /**
     * @brief 从主机内存复制到设备内存
     * 
     * @param dst 目标设备内存指针
     * @param src 源主机内存指针
     * @param size 复制大小（字节）
     * @param async 是否异步复制
     * @param stream_id 流ID（async 时使用，0=默认流）
     * @return 是否成功
     */
    virtual bool copy_to_device(void* dst, const void* src, size_t size, bool async = false, int stream_id = 0) = 0;

    /**
     * @brief 从设备内存复制到主机内存
     * 
     * @param dst 目标主机内存指针
     * @param src 源设备内存指针
     * @param size 复制大小（字节）
     * @param async 是否异步复制
     * @param stream_id 流ID（async 时使用，0=默认流）
     * @return 是否成功
     */
    virtual bool copy_to_host(void* dst, const void* src, size_t size, bool async = false, int stream_id = 0) = 0;

    /**
     * @brief 在设备内存之间复制
     * 
     * @param dst 目标设备内存指针
     * @param src 源设备内存指针
     * @param size 复制大小（字节）
     * @param async 是否异步复制
     * @param stream_id 流ID（async 时使用，0=默认流）
     * @return 是否成功
     */
    virtual bool copy_device_to_device(void* dst, const void* src, size_t size, bool async = false, int stream_id = 0) = 0;

    /**
     * @brief 分配统一内存（Unified Memory）
     *
     * 统一内存可被 CPU 和 GPU 同时访问，无需显式传输。
     * 需要硬件和驱动支持（CUDA 6.0+）。
     *
     * @param size 内存大小（字节）
     * @return 统一内存指针，失败或不支持返回 nullptr
     */
    virtual void* allocate_unified_memory(size_t size) {
        (void)size;
        return nullptr;  // 默认不支持
    }

    /**
     * @brief 释放统一内存
     *
     * @param ptr 统一内存指针
     */
    virtual void free_unified_memory(void* ptr) {
        (void)ptr;
        // 默认不支持，什么也不做
    }

    /**
     * @brief 预取内存到指定设备
     *
     * 将统一内存预取到指定设备，优化访问性能。
     * device_id = cudaCpuDeviceId (-1) 表示预取到主机。
     *
     * @param ptr 统一内存指针
     * @param size 预取大小（字节）
     * @param device_id 目标设备ID（-1 表示主机）
     * @param stream_id 流ID（0=默认流）
     * @return 是否成功
     */
    virtual bool prefetch_memory(const void* ptr, size_t size, int device_id, int stream_id = 0) {
        (void)ptr;
        (void)size;
        (void)device_id;
        (void)stream_id;
        return false;  // 默认不支持
    }

    /**
     * @brief 同步所有操作
     * 
     * 等待所有已提交的 GPU 操作完成
     */
    virtual void synchronize() = 0;

    /**
     * @brief 同步指定流
     * 
     * @param stream_id 流ID
     */
    virtual void synchronize_stream(int stream_id) = 0;

    /**
     * @brief 创建新的流
     * 
     * @return 流ID，失败返回 -1
     */
    virtual int create_stream() = 0;

    /**
     * @brief 销毁流
     * 
     * @param stream_id 流ID
     */
    virtual void destroy_stream(int stream_id) = 0;

    /**
     * @brief 在指定流上注册主机回调
     *
     * 当该流上此前排队的操作（包括异步 copy）都完成后，在适当时机调用 callback。
     * 当前仅 CUDA 后端支持此操作；调用前可用 supports_stream_callback() 查询。
     * 返回 false 时，通过 get_status().last_error_message 获取失败原因。
     *
     * @param stream_id 流ID（0=默认流）
     * @param callback 回调函数
     * @return 成功返回 true，无效 stream_id 或不可用时返回 false
     */
    virtual bool add_stream_callback(int stream_id, std::function<void()> callback) = 0;

    /**
     * @brief 查询后端是否支持流回调
     *
     * 此查询描述后端能力，而非设备或执行器当前是否可用。当前 CUDA 返回 true，
     * OpenCL 返回 false；当操作失败时可通过 get_status().last_error_message 诊断原因。
     *
     * @return 后端支持 add_stream_callback 时返回 true
     */
    virtual bool supports_stream_callback() const noexcept {
        return false;
    }

    /**
     * @brief 从 peer 执行器所在设备拷贝到本执行器设备（P2P 设备间拷贝）
     *
     * dst_ptr 须由本执行器分配，src_ptr 须由 src_executor 分配。
     * 同设备请使用 copy_device_to_device。
     *
     * @note EXPERIMENTAL - Not tested on real hardware.
     *       If you have a multi-GPU setup, please test and report issues.
     *
     * @param src_executor 源设备对应执行器
     * @param src_ptr 源设备内存指针
     * @param dst_ptr 本设备目标内存指针
     * @param size 拷贝字节数
     * @param async 是否异步
     * @param stream_id 流ID（async 时使用，0=默认流）
     * @return 是否成功；默认实现返回 false
     */
    virtual bool copy_from_peer(IGpuExecutor* src_executor, const void* src_ptr, void* dst_ptr,
                               size_t size, bool async = false, int stream_id = 0) {
        (void)src_executor;
        (void)src_ptr;
        (void)dst_ptr;
        (void)size;
        (void)async;
        (void)stream_id;
        return false;
    }

    /**
     * @brief 获取执行器名称
     * @return 执行器名称
     */
    virtual std::string get_name() const = 0;

    /**
     * @brief 获取 GPU 设备信息
     * @return GPU 设备信息
     */
    virtual gpu::GpuDeviceInfo get_device_info() const = 0;

    /**
     * @brief 获取执行器状态
     * @return GPU 执行器状态
     */
    virtual gpu::GpuExecutorStatus get_status() const = 0;

    /**
     * @brief 启动执行器
     * @return 是否启动成功
     */
    virtual bool start() = 0;

    /**
     * @brief 停止执行器
     */
    virtual void stop() = 0;

    /**
     * @brief 等待所有任务完成
     */
    virtual void wait_for_completion() = 0;

    /**
     * @brief Set the maximum number of unresolved dependency waiters.
     *
     * A submission above this limit completes immediately with an exception.
     * This setting must be changed while the executor is stopped.
     */
    void set_max_pending_waiters(size_t max_pending_waiters) {
        std::lock_guard<std::mutex> lk(pending_waiters_mutex_);
        max_pending_waiters_ = max_pending_waiters;
    }

protected:
    /**
     * @brief 提交 GPU kernel 实现（内部方法）
     *
     * kernel_func 接收流句柄 void*：nullptr 表示默认流，非空表示后端流（如 cudaStream_t）。
     *
     * @param kernel_func GPU kernel 函数（类型擦除，接收 void* 流句柄）
     * @param config GPU 任务配置
     * @return std::future<void> 任务执行结果的 future
     */
    virtual std::future<void> submit_kernel_impl(
        std::function<void(void*)> kernel_func,
        const gpu::GpuTaskConfig& config) = 0;

    /**
     * @brief Start a generation of the bounded dependency waiter scheduler.
     *
     * One scheduler thread polls all unresolved dependencies at a short
     * interval and submits ready kernels outside the scheduler lock.  This
     * bounds both waiting threads (one per executor) and queued waiter state;
     * submissions beyond max_pending_waiters_ fail immediately.  stop()
     * closes admission, wakes this thread, cancels queued waiters, and joins
     * the one scheduler thread before backend state is destroyed.
     */
    void start_waiter_generation() {
        std::lock_guard<std::mutex> lk(pending_waiters_mutex_);
        ++waiter_generation_;
        accepting_waiters_ = true;
        waiter_stop_requested_ = false;
        waiter_thread_ = std::thread([this, generation = waiter_generation_] {
            run_pending_waiters(generation);
        });
    }

    void close_waiter_admission() {
        {
            std::lock_guard<std::mutex> lk(pending_waiters_mutex_);
            accepting_waiters_ = false;
            waiter_stop_requested_ = true;
        }
        pending_waiters_cv_.notify_all();
    }

    void join_pending_waiters() {
        std::thread waiter_thread;
        {
            std::lock_guard<std::mutex> lk(pending_waiters_mutex_);
            accepting_waiters_ = false;
            waiter_stop_requested_ = true;
            waiter_thread.swap(waiter_thread_);
        }
        pending_waiters_cv_.notify_all();
        if (waiter_thread.joinable()) {
            waiter_thread.join();
        }
    }

    struct PendingWaiter {
        std::shared_future<void> dependency;
        std::function<void(void*)> kernel_func;
        gpu::GpuTaskConfig config;
        std::shared_ptr<std::promise<void>> promise;
    };

    void run_pending_waiters(uint64_t generation) {
        for (;;) {
            std::vector<PendingWaiter> ready_waiters;
            std::vector<PendingWaiter> cancelled_waiters;
            bool stopping = false;
            {
                std::unique_lock<std::mutex> lk(pending_waiters_mutex_);
                pending_waiters_cv_.wait_for(lk, std::chrono::milliseconds(10), [this, generation] {
                    return waiter_stop_requested_ || waiter_generation_ != generation;
                });

                if (waiter_stop_requested_ || waiter_generation_ != generation) {
                    stopping = true;
                    cancelled_waiters.assign(
                        std::make_move_iterator(pending_waiters_.begin()),
                        std::make_move_iterator(pending_waiters_.end()));
                    pending_waiters_.clear();
                } else {
                    auto waiter = pending_waiters_.begin();
                    while (waiter != pending_waiters_.end()) {
                        if (waiter->dependency.wait_for(std::chrono::seconds(0)) ==
                            std::future_status::ready) {
                            ready_waiters.push_back(std::move(*waiter));
                            waiter = pending_waiters_.erase(waiter);
                        } else {
                            ++waiter;
                        }
                    }
                }
            }

            for (auto& waiter : cancelled_waiters) {
                waiter.promise->set_exception(std::make_exception_ptr(
                    std::runtime_error("executor stopped before dependency completed")));
            }
            if (stopping) {
                return;
            }

            for (auto& waiter : ready_waiters) {
                try {
                    if (!waiter_generation_is_active(generation)) {
                        throw std::runtime_error("executor stopped before dependency completed");
                    }
                    auto inner_future = submit_kernel_impl(std::move(waiter.kernel_func), waiter.config);
                    inner_future.get();
                    waiter.promise->set_value();
                } catch (...) {
                    waiter.promise->set_exception(std::current_exception());
                }
            }
        }
    }

    bool waiter_generation_is_active(uint64_t generation) {
        std::lock_guard<std::mutex> lk(pending_waiters_mutex_);
        return accepting_waiters_ && !waiter_stop_requested_ && waiter_generation_ == generation;
    }

    // The lock protects both waiter state and admission, so stop() cannot
    // miss a submission registered concurrently with it.
    std::mutex                    pending_waiters_mutex_;
    std::condition_variable       pending_waiters_cv_;
    std::deque<PendingWaiter>     pending_waiters_;
    std::thread                   waiter_thread_;
    uint64_t                      waiter_generation_{0};
    size_t                        max_pending_waiters_{256};
    bool                          accepting_waiters_{false};
    bool                          waiter_stop_requested_{false};
};

// 模板方法实现
template<typename KernelFunc>
auto IGpuExecutor::submit_kernel(KernelFunc&& kernel, const gpu::GpuTaskConfig& config)
    -> std::future<void> {
    std::function<void(void*)> kernel_func;
    if constexpr (std::is_invocable_v<KernelFunc, void*>) {
        kernel_func = [kernel = std::forward<KernelFunc>(kernel)](void* stream) mutable {
            kernel(stream);
        };
    } else {
        kernel_func = [kernel = std::forward<KernelFunc>(kernel)](void* stream) mutable {
            (void)stream;
            kernel();
        };
    }
    return submit_kernel_impl(std::move(kernel_func), config);
}

inline std::vector<std::future<void>> IGpuExecutor::submit_kernels_batch(
    const std::vector<std::pair<std::function<void(void*)>, gpu::GpuTaskConfig>>& tasks) {
    std::vector<std::future<void>> result;
    result.reserve(tasks.size());
    for (const auto& p : tasks) {
        result.push_back(submit_kernel_impl(p.first, p.second));
    }
    return result;
}

template<typename KernelFunc>
auto IGpuExecutor::submit_kernel_after(std::shared_future<void> dependency, KernelFunc&& kernel,
                                       const gpu::GpuTaskConfig& config) -> std::future<void> {
    std::shared_future<void> dep = std::move(dependency);

    // 构造"去 dep 等待"的 kernel lambda,这样重新入队时 worker 线程
    // 不会再次阻塞。
    std::function<void(void*)> kernel_func;
    if constexpr (std::is_invocable_v<KernelFunc, void*>) {
        kernel_func = [kernel = std::forward<KernelFunc>(kernel)](void* stream) mutable {
            kernel(stream);
        };
    } else {
        kernel_func = [kernel = std::forward<KernelFunc>(kernel)](void* /*stream*/) mutable {
            kernel();
        };
    }

    // 创建外层 promise/future — 调用方拿到的 future
    auto promise = std::make_shared<std::promise<void>>();
    auto result_future = promise->get_future();

    {
        std::lock_guard<std::mutex> lk(pending_waiters_mutex_);
        if (!accepting_waiters_) {
            promise->set_exception(std::make_exception_ptr(
                std::runtime_error("executor is stopped")));
            return result_future;
        }
        if (pending_waiters_.size() >= max_pending_waiters_) {
            promise->set_exception(std::make_exception_ptr(
                std::runtime_error("too many pending gpu dependencies; waiter pool saturated")));
            return result_future;
        }
        pending_waiters_.push_back(
            PendingWaiter{std::move(dep), std::move(kernel_func), config, promise});
    }
    pending_waiters_cv_.notify_one();
    return result_future;
}

} // namespace kairo
