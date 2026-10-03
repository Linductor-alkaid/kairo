#pragma once

#include <kairo/interfaces.hpp>
#include <kairo/config.hpp>
#include <kairo/types.hpp>
#include "opencl_loader.hpp"
#include <memory>
#include <string>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>
#include <future>
#include <functional>
#include <queue>
#include <condition_variable>
#include <thread>

namespace kairo {
namespace gpu {

class OpenCLExecutor : public IGpuExecutor {
public:
    OpenCLExecutor(const std::string& name, const GpuExecutorConfig& config);
    ~OpenCLExecutor();

    OpenCLExecutor(const OpenCLExecutor&) = delete;
    OpenCLExecutor& operator=(const OpenCLExecutor&) = delete;

    // IGpuExecutor 接口实现
    void* allocate_device_memory(size_t size) override;
    void free_device_memory(void* ptr) override;
    bool copy_to_device(void* dst, const void* src, size_t size, bool async = false, int stream_id = 0) override;
    bool copy_to_host(void* dst, const void* src, size_t size, bool async = false, int stream_id = 0) override;
    bool copy_device_to_device(void* dst, const void* src, size_t size, bool async = false, int stream_id = 0) override;
    bool copy_from_peer(IGpuExecutor* src_executor, const void* src_ptr, void* dst_ptr,
                       size_t size, bool async = false, int stream_id = 0) override;
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
    void wait_for_completion() override;

protected:
    std::future<void> submit_kernel_impl(
        std::function<void(void*)> kernel_func,
        const GpuTaskConfig& config) override;

private:
    struct CommandQueueWrapper {
        cl_command_queue queue = nullptr;
        std::mutex mutex;
    };

    struct MemoryAllocation {
        cl_mem buffer = nullptr;
        size_t size = 0;
    };

    struct QueuedTask {
        std::function<void()> run;
        std::shared_ptr<std::promise<void>> promise;
    };

    bool initialize_opencl();
    void cleanup();
    void cleanup_locked();
    bool check_opencl_error(cl_int error, const char* operation);
    bool validate_memory_range(const void* device_ptr, std::size_t size, const char* api_name);
    std::shared_ptr<CommandQueueWrapper> get_queue(int stream_id);
    void worker_thread();
    bool validate_config();
    void clear_last_error();
    void set_last_error(const std::string& message);
    std::string get_last_error() const;
    void rollback_start_failure(const std::string& message);

    std::string name_;
    GpuExecutorConfig config_;
    std::atomic<bool> is_available_{false};
    OpenCLLoader* loader_;

    cl_platform_id platform_;
    cl_device_id device_;
    cl_context context_;
    mutable std::shared_mutex lifecycle_mutex_;
    std::vector<std::shared_ptr<CommandQueueWrapper>> queues_;
    mutable std::mutex queues_mutex_;

    std::unordered_map<void*, MemoryAllocation> memory_map_;
    mutable std::mutex memory_mutex_;

    std::atomic<size_t> active_kernels_{0};
    std::atomic<size_t> completed_kernels_{0};
    std::atomic<size_t> failed_kernels_{0};
    std::atomic<int64_t> total_kernel_time_ns_{0};

    std::atomic<bool> running_{false};
    bool stopping_ = false;
    mutable std::mutex error_mutex_;
    std::string last_error_message_;
    std::thread worker_;
    // worker 线程入口注册、stop() 读取、join 完成后重置；写入均持有
    // lifecycle_mutex_，用于识别"从 kernel 内部调用 stop()"的自停场景。
    std::thread::id worker_id_;
    std::atomic<bool> self_stop_requested_{false};
    // 串行化对 worker_ 的 join/finalize，避免并发 stop() 双重 join。
    std::mutex worker_join_mutex_;
    std::function<std::thread(OpenCLExecutor*)> worker_thread_factory_for_test_;
    std::queue<QueuedTask> task_queue_;
    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::condition_variable queue_not_full_cv_;
    std::condition_variable queue_drained_cv_;
};

} // namespace gpu
} // namespace kairo
