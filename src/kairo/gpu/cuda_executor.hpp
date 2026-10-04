#pragma once

#include <kairo/interfaces.hpp>
#include <kairo/config.hpp>
#include <kairo/types.hpp>
#include "cuda_loader.hpp"
#include "gpu_memory_manager.hpp"
#include "../util/exception_handler.hpp"
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <future>
#include <functional>
#include <queue>
#include <condition_variable>
#include <chrono>
#include <thread>

#ifdef KAIRO_ENABLE_CUDA
#include <cuda_runtime.h>
#endif

namespace kairo {
namespace gpu {

class InvalidStreamException : public std::runtime_error {
public:
    explicit InvalidStreamException(const std::string& message)
        : std::runtime_error(message) {}
};

/**
 * @brief CUDA 执行器实现
 * 
 * 实现 IGpuExecutor 接口，提供 CUDA GPU 任务执行功能。
 * 支持构建时和运行时的 CUDA 可用性检测。
 */
class CudaExecutor : public IGpuExecutor {
public:
    /**
     * @brief 构造函数
     * 
     * @param name 执行器名称
     * @param config GPU 执行器配置
     */
    CudaExecutor(const std::string& name, const GpuExecutorConfig& config);

    /**
     * @brief 析构函数
     */
    ~CudaExecutor();

    // 禁止拷贝和移动
    CudaExecutor(const CudaExecutor&) = delete;
    CudaExecutor& operator=(const CudaExecutor&) = delete;
    CudaExecutor(CudaExecutor&&) = delete;
    CudaExecutor& operator=(CudaExecutor&&) = delete;

    // 实现 IGpuExecutor 接口
    void* allocate_device_memory(size_t size) override;
    void free_device_memory(void* ptr) override;
    void* allocate_unified_memory(size_t size) override;
    void free_unified_memory(void* ptr) override;
    bool prefetch_memory(const void* ptr, size_t size, int device_id, int stream_id = 0) override;
    bool copy_to_device(void* dst, const void* src, size_t size, bool async = false, int stream_id = 0) override;
    bool copy_to_host(void* dst, const void* src, size_t size, bool async = false, int stream_id = 0) override;
    bool copy_device_to_device(void* dst, const void* src, size_t size, bool async = false, int stream_id = 0) override;
    bool copy_from_peer(IGpuExecutor* src_executor, const void* src_ptr, void* dst_ptr,
                       size_t size, bool async = false, int stream_id = 0) override;

    /**
     * @brief 显式允许本执行器的复制操作使用外部分配的 CUDA 内存。
     *
     * 外部内存不会由执行器释放；调用方必须在内存失效前注销它。
     */
    bool register_external_memory(void* ptr, size_t size);
    void unregister_external_memory(void* ptr);
    bool add_stream_callback(int stream_id, std::function<void()> callback) override;
    bool supports_stream_callback() const noexcept override;
    void synchronize() override;
    void synchronize_stream(int stream_id) override;
    int create_stream() override;
    void destroy_stream(int stream_id) override;
    std::string get_name() const override;
    GpuDeviceInfo get_device_info() const override;
    GpuExecutorStatus get_status() const override;
    bool start() override;
    void stop() override;

    /**
     * @brief 请求停止并在外部线程中等待 CUDA worker 结束
     * @return 外部调用返回 true；worker 线程内调用返回 false
     */
    bool stop_and_join();
    void wait_for_completion() override;

    /**
     * @brief 设置任务异常回调（与 CPU 执行器一致，用于监控/日志）
     * @param callback 回调函数，参数为执行器名称和异常指针
     */
    void set_exception_callback(
        std::function<void(const std::string&, std::exception_ptr)> callback);

    /**
     * @brief 批量提交（覆盖以在同一把锁内入队，对外可调用）
     */
    std::vector<std::future<void>> submit_kernels_batch(
        const std::vector<std::pair<std::function<void(void*)>, GpuTaskConfig>>& tasks) override;

protected:
    /**
     * @brief 提交 GPU kernel 实现（内部方法）
     */
    std::future<void> submit_kernel_impl(
        std::function<void(void*)> kernel_func,
        const GpuTaskConfig& config) override;

private:
#ifdef KAIRO_ENABLE_CUDA
    struct StreamWrapper {
        cudaStream_t stream = nullptr;
        std::mutex mutex;
        std::atomic<bool> destroyed{false};
    };
#endif

    /** 队列项：优先级高的先执行，同优先级按 submit_time_ns FIFO */
    struct GpuQueuedTask {
        std::function<void(void*)> kernel_func;
        GpuTaskConfig config;
        std::shared_ptr<std::promise<void>> promise;
        int64_t submit_time_ns = 0;
#ifdef KAIRO_ENABLE_CUDA
        std::shared_ptr<StreamWrapper> stream;
#endif
    };
    struct GpuQueuedTaskCompare {
        bool operator()(const GpuQueuedTask& a, const GpuQueuedTask& b) const {
            if (a.config.priority != b.config.priority)
                return a.config.priority < b.config.priority;
            return a.submit_time_ns > b.submit_time_ns;
        }
    };

    /** worker 线程主循环 */
    void worker_thread_func();

    /** CR-002: start() 半途失败时排空队列——对每个在途 promise set_exception，
     *  使 stop()/wait_for_completion() 不因"无 worker 排空"而永久挂死。 */
    void fail_all_queued_tasks(const std::string& message);

    /** 执行单任务（设备上下文、kernel、错误检查、promise、统计） */
    void run_one_task(GpuQueuedTask& task);
    void clear_last_error() const;
    void set_last_error(const std::string& message) const;
    std::string get_last_error() const;

private:
    enum class AllocationKind {
        Owned,
        ExternalOptIn,
    };

    struct AllocationRecord {
        size_t size = 0;
        AllocationKind kind = AllocationKind::Owned;
        bool free_directly = true;
    };

    /**
     * CUDA runtime APIs cannot establish which executor owns a raw pointer.  Keep
     * every user-visible allocation here and validate an exact allocation base
     * plus its requested range before reaching the driver.  External CUDA memory
     * is deliberately accepted only after register_external_memory(); it is
     * tracked for range checks but never released by this executor.
     */
    bool validate_memory_range(const void* ptr, size_t size, const char* argument) const;

    /**
     * @brief 检查 CUDA 运行时是否可用
     * @return 是否可用
     */
    bool check_cuda_available();

    /**
     * @brief 校验 CUDA 配置并记录错误状态
     * @return 配置是否有效
     */
    bool validate_config() const;

    /**
     * @brief 初始化 CUDA 设备
     * @return 是否初始化成功
     */
    bool initialize_device();

    /**
     * @brief 检查 CUDA 错误并记录
     * @param error_code CUDA 错误码
     * @param operation 操作名称
     * @return 是否成功
     */
#ifdef KAIRO_ENABLE_CUDA
    bool check_cuda_error(cudaError_t error_code, const char* operation) const;
#else
    bool check_cuda_error(int error_code, const char* operation) const;
#endif

    /**
     * @brief 获取默认流
     * @return 默认流句柄
     */
#ifdef KAIRO_ENABLE_CUDA
    cudaStream_t get_default_stream() const;
#endif

    /**
     * @brief 根据 stream_id 解析流 wrapper（1..N=streams_[id-1]）
     * @param stream_id 流ID
     * @return 流 wrapper，无效或已销毁时返回 nullptr
     */
#ifdef KAIRO_ENABLE_CUDA
    std::shared_ptr<StreamWrapper> get_stream(int stream_id) const;
#endif

    /**
     * @brief 创建单个流（不加锁，由调用方持有 streams_mutex_ 时使用）
     * @return 新流句柄，失败返回 nullptr
     */
#ifdef KAIRO_ENABLE_CUDA
    cudaStream_t create_one_stream();
    void destroy_stream_wrapper(const std::shared_ptr<StreamWrapper>& stream_wrapper);
    bool call_stream(const std::shared_ptr<StreamWrapper>& stream_wrapper,
                     const char* operation,
                     const std::function<cudaError_t(cudaStream_t)>& call) const;
    bool validate_stream(const std::shared_ptr<StreamWrapper>& stream_wrapper) const;
    std::exception_ptr make_invalid_stream_exception_ptr(int stream_id) const;
#endif

    /**
     * @brief 确保当前 CUDA 上下文为本执行器对应设备（多 GPU 安全）
     * @return 是否成功
     */
    bool ensure_device_context() const;

    /**
     * @brief 底层设备内存分配（不经过内存池，不写入 allocated_memory_）
     * @param size 字节数
     * @return 设备指针，失败返回 nullptr
     */
    void* raw_allocate_device_memory(size_t size);

    /**
     * @brief 底层设备内存释放（不操作 allocated_memory_）
     * @param ptr 设备指针
     */
    void raw_free_device_memory(void* ptr);

    /**
     * @brief 将 CUDA 错误码转换为标准异常（用于 ExceptionHandler / future）
     * @param error_code CUDA 错误码
     * @param operation 操作名称
     * @return 异常指针，可传递给 handle_task_exception 或 promise::set_exception
     */
#ifdef KAIRO_ENABLE_CUDA
    std::exception_ptr make_cuda_exception_ptr(cudaError_t error_code, const char* operation) const;
#endif

private:
    std::string name_;                          // 执行器名称
    GpuExecutorConfig config_;                  // 配置
    int device_id_;                            // 设备ID
    std::atomic<bool> is_available_{false};    // CUDA是否可用
    std::atomic<bool> is_running_{false};      // 是否运行中
    CudaLoader* loader_;                       // CUDA加载器（单例引用）
    mutable std::mutex error_mutex_;
    mutable std::string last_error_message_;

#ifdef KAIRO_ENABLE_CUDA
    cudaDeviceProp device_prop_;               // 设备属性
    cudaStream_t default_stream_;              // 默认流
    std::vector<std::shared_ptr<StreamWrapper>> streams_;  // 流列表
    mutable std::mutex streams_mutex_;          // 流列表互斥锁（mutable用于const方法）
#endif

    // 内存管理
    std::unique_ptr<GpuMemoryManager> memory_manager_;    // 内存池（memory_pool_size > 0 时使用）
    std::unordered_map<void*, AllocationRecord> allocated_memory_;  // 已登记内存范围
    mutable std::mutex memory_mutex_;                     // 内存映射互斥锁（mutable用于const方法）

    // 统计信息
    std::atomic<size_t> active_kernels_{0};     // 活跃kernel数
    std::atomic<size_t> completed_kernels_{0}; // 已完成kernel数
    std::atomic<size_t> failed_kernels_{0};    // 失败kernel数
    std::atomic<int64_t> total_kernel_time_ns_{0}; // 总kernel执行时间（纳秒）
    // 任务队列与 worker
    std::priority_queue<GpuQueuedTask, std::vector<GpuQueuedTask>, GpuQueuedTaskCompare> task_queue_;
    mutable std::mutex queue_mutex_;
    std::condition_variable queue_not_empty_cv_;
    std::condition_variable queue_not_full_cv_;
    std::condition_variable queue_drained_cv_;
    std::thread worker_thread_;
    std::thread::id worker_id_;
    std::mutex stop_mutex_;                    // 串行化 start()/stop() 和 worker 句柄移交
    std::atomic<bool> self_stop_requested_{false};
    bool worker_joined_ = true;
    // CR-002: worker 是否曾成功启动。start() 在建流/建线程之前就置
    // is_running_=true，半途失败时队列中的任务无人消费——wait_for_completion
    // 的排空谓词据此退出等待，而不是永久挂死。
    std::atomic<bool> worker_started_{false};

    util::ExceptionHandler exception_handler_;  // 任务异常处理，与 CPU 执行器一致
};

} // namespace gpu
} // namespace kairo
