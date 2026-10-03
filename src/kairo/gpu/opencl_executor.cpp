#include "opencl_executor.hpp"
#include <chrono>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace kairo {
namespace gpu {

namespace {

constexpr const char* kInvalidOpenCLDeviceIdMessage = "OpenCL device_id must be >= 0";

std::string opencl_config_validation_error(const GpuExecutorConfig& config) {
    auto message = gpu_config_validation_error(config);
    if (message == "GPU executor device_id must be >= 0") {
        return kInvalidOpenCLDeviceIdMessage;
    }
    return message;
}

const char* opencl_error_name(cl_int error) {
    switch (error) {
        case CL_SUCCESS:
            return "CL_SUCCESS";
        case -1:
            return "CL_DEVICE_NOT_FOUND";
        case -2:
            return "CL_DEVICE_NOT_AVAILABLE";
        case -3:
            return "CL_COMPILER_NOT_AVAILABLE";
        case -4:
            return "CL_MEM_OBJECT_ALLOCATION_FAILURE";
        case -5:
            return "CL_OUT_OF_RESOURCES";
        case -6:
            return "CL_OUT_OF_HOST_MEMORY";
        case -30:
            return "CL_INVALID_VALUE";
        case -36:
            return "CL_INVALID_COMMAND_QUEUE";
        case -38:
            return "CL_INVALID_MEM_OBJECT";
        case -48:
            return "CL_INVALID_KERNEL";
        case -52:
            return "CL_INVALID_KERNEL_ARGS";
        case -54:
            return "CL_INVALID_WORK_GROUP_SIZE";
        case -57:
            return "CL_INVALID_EVENT_WAIT_LIST";
        case -61:
            return "CL_INVALID_BUFFER_SIZE";
        default:
            return "CL_UNKNOWN_ERROR";
    }
}

}  // namespace

OpenCLExecutor::OpenCLExecutor(const std::string& name, const GpuExecutorConfig& config)
    : name_(name)
    , config_(config)
    , is_available_(false)
    , loader_(&OpenCLLoader::instance())
    , platform_(nullptr)
    , device_(nullptr)
    , context_(nullptr) {
    validate_config();
}

OpenCLExecutor::~OpenCLExecutor() {
    stop();
    cleanup();
}

bool OpenCLExecutor::start() {
    std::unique_lock<std::shared_mutex> lifecycle_lock(lifecycle_mutex_);

    if (stopping_) {
        set_last_error("OpenCLExecutor is stopping");
        return false;
    }

    if (!validate_config()) {
        return false;
    }

    if (running_.load(std::memory_order_acquire)) {
        return true;
    }
    running_.store(true, std::memory_order_release);

    clear_last_error();

    if (!loader_->load()) {
        rollback_start_failure("OpenCL loader is unavailable");
        return false;
    }

    try {
        if (!initialize_opencl()) {
            std::string error = get_last_error();
            if (error.empty()) {
                error = "OpenCL initialization failed";
            }
            cleanup_locked();
            rollback_start_failure(error);
            return false;
        }
    } catch (const std::exception& ex) {
        cleanup_locked();
        rollback_start_failure(std::string("OpenCL initialization failed: ") + ex.what());
        return false;
    } catch (...) {
        cleanup_locked();
        rollback_start_failure("OpenCL initialization failed: unknown exception");
        return false;
    }

    try {
        // 收编可能残留的前一代 worker（例如此前只有 worker 自停、无人 join）。
        // 对 joinable 的 std::thread 赋值会触发 std::terminate，必须先 join。
        {
            std::lock_guard<std::mutex> join_lock(worker_join_mutex_);
            if (worker_.joinable()) {
                worker_.join();
            }
        }
        worker_id_ = std::thread::id{};
        self_stop_requested_.store(false, std::memory_order_release);
        if (worker_thread_factory_for_test_) {
            worker_ = worker_thread_factory_for_test_(this);
        } else {
            worker_ = std::thread(&OpenCLExecutor::worker_thread, this);
        }
    } catch (const std::exception& ex) {
        cleanup_locked();
        rollback_start_failure(std::string("OpenCL worker thread creation failed: ") + ex.what());
        return false;
    } catch (...) {
        cleanup_locked();
        rollback_start_failure("OpenCL worker thread creation failed: unknown exception");
        return false;
    }

    // Reopen dependency-waiter admission only after startup succeeds.
    start_waiter_generation();
    return true;
}

void OpenCLExecutor::stop() {
    bool self_stop = false;
    {
        std::unique_lock<std::shared_mutex> lifecycle_lock(lifecycle_mutex_);

        self_stop = std::this_thread::get_id() == worker_id_;
        if (stopping_ && !self_stop) {
            // 另一个 stop() 正在收尾（可能是不会 join 的自停路径）。本调用
            // 不再重复取消/通知，但必须兜底 finalize：join 掉可能残留的
            // 已退出未 join 的 worker，否则析构路径会析构 joinable 线程。
            lifecycle_lock.unlock();
            join_pending_waiters();
            {
                std::lock_guard<std::mutex> join_lock(worker_join_mutex_);
                if (worker_.joinable()) {
                    worker_.join();
                }
            }
            return;  // stopping_/worker_id_ 由进行中的那个 stop() 复位
        }

        running_.store(false, std::memory_order_release);
        stopping_ = true;
        // 自停（kernel 内调用 stop()）不能 join 自己：join 会抛 EDEADLK，
        // 异常逃逸后 stopping_ 永久为 true，此后 start() 全部失败，且
        // ~OpenCLExecutor 会析构一个 joinable 的 std::thread 触发 terminate。
        // 这里只请求停止并清理队列，join 留给后续外部 stop()/析构完成。
    }

    queue_cv_.notify_all();
    queue_not_full_cv_.notify_all();

    size_t cancelled_tasks = 0;

    // Cancel queued tasks so the worker cannot block trying to enter the
    // lifecycle shared gate while stop() is waiting to join it.
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        while (!task_queue_.empty()) {
            auto task = std::move(task_queue_.front());
            task_queue_.pop();
            task.promise->set_exception(std::make_exception_ptr(
                ExecutorStopping("OpenCLExecutor stopped before queued kernel execution")));
            ++cancelled_tasks;
        }
        if (active_kernels_.load() == 0) {
            queue_drained_cv_.notify_all();
        }
    }

    if (cancelled_tasks > 0) {
        failed_kernels_.fetch_add(cancelled_tasks, std::memory_order_relaxed);
        set_last_error("OpenCLExecutor stopped: cancelled " +
                       std::to_string(cancelled_tasks) + " queued kernel task(s)");
    }

    // P-260625-002 fix: signal + join all submit_kernel_after waiter threads
    // before tearing down internal state. Mirrors the same P-002 fix on
    // CudaExecutor (commit 159ab55) to eliminate the UAF where a detached
    // waiter accesses this->submit_kernel_impl() after ~OpenCLExecutor()
    // releases context_/queues_/streams_ via cleanup().
    join_pending_waiters();

    if (!self_stop) {
        // finalize：外部视角收尾 worker。worker_join_mutex_ 串行化并发
        // stop() 的 join（其中一个真正 join，其余看到 not-joinable 跳过）。
        // 若此前只有 worker 自停（线程已退出但无人 join），这里补上 join，
        // 保证 ~OpenCLExecutor 不会析构 joinable 线程。
        {
            std::lock_guard<std::mutex> join_lock(worker_join_mutex_);
            if (worker_.joinable()) {
                worker_.join();
            }
        }

        std::unique_lock<std::shared_mutex> lifecycle_lock(lifecycle_mutex_);
        worker_id_ = std::thread::id{};
        self_stop_requested_.store(false, std::memory_order_release);
        stopping_ = false;
    } else {
        // 自停：worker 线程在当前 kernel 返回后会看到 running_==false 并
        // 退出。stopping_ 立即复位，避免阻塞后续外部 stop()（含析构）的
        // finalize；join 由那条路径完成。
        std::unique_lock<std::shared_mutex> lifecycle_lock(lifecycle_mutex_);
        self_stop_requested_.store(true, std::memory_order_release);
        stopping_ = false;
    }
}

void OpenCLExecutor::wait_for_completion() {
    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        queue_drained_cv_.wait(lock, [this] {
            return task_queue_.empty() && active_kernels_.load() == 0;
        });
    }

    synchronize();
}

bool OpenCLExecutor::initialize_opencl() {
    if (!validate_config()) {
        return false;
    }

    auto funcs = loader_->get_functions();
    cl_int err;

    // CR-003: stop() 不释放 OpenCL 资源（避免释放仍有在飞命令的队列）。
    // 此前重启时 context_ 被直接覆盖（每轮 start/stop 泄漏一个
    // cl_context）、queues_ 逐轮 push_back 无界增长且 stream_id 语义漂移。
    // 重启入口一次性收编上一代资源：先对每个存活队列 clFinish 等待在飞
    // 命令完成（对齐 CUDA stop 的 synchronize 语义），再整体释放。
    {
        bool has_previous_generation = false;
        {
            std::lock_guard<std::mutex> lock(queues_mutex_);
            has_previous_generation = !queues_.empty();
        }
        if (!has_previous_generation && context_ != nullptr) {
            has_previous_generation = true;
        }
        if (has_previous_generation) {
            {
                std::lock_guard<std::mutex> lock(queues_mutex_);
                for (auto& queue_wrapper : queues_) {
                    if (!queue_wrapper || !queue_wrapper->queue) {
                        continue;
                    }
                    std::lock_guard<std::mutex> queue_lock(queue_wrapper->mutex);
                    funcs.clFinish(queue_wrapper->queue);
                }
            }
            cleanup_locked();
        }
    }

    // 获取平台
    cl_uint num_platforms;
    err = funcs.clGetPlatformIDs(0, nullptr, &num_platforms);
    if (err != CL_SUCCESS || num_platforms == 0) {
        set_last_error("OpenCL platform enumeration failed");
        return false;
    }

    std::vector<cl_platform_id> platforms(num_platforms);
    err = funcs.clGetPlatformIDs(num_platforms, platforms.data(), nullptr);
    if (err != CL_SUCCESS) {
        set_last_error("OpenCL platform list retrieval failed");
        return false;
    }
    platform_ = platforms[0];

    // 获取GPU设备
    cl_uint num_devices;
    err = funcs.clGetDeviceIDs(platform_, CL_DEVICE_TYPE_GPU, 0, nullptr, &num_devices);
    if (err != CL_SUCCESS || num_devices == 0) {
        set_last_error("OpenCL GPU device enumeration failed");
        return false;
    }

    std::vector<cl_device_id> devices(num_devices);
    err = funcs.clGetDeviceIDs(platform_, CL_DEVICE_TYPE_GPU, num_devices, devices.data(), nullptr);
    if (err != CL_SUCCESS) {
        set_last_error("OpenCL GPU device list retrieval failed");
        return false;
    }

    if (config_.device_id < 0) {
        set_last_error(kInvalidOpenCLDeviceIdMessage);
        return false;
    }

    if (config_.device_id >= static_cast<int>(num_devices)) {
        set_last_error("OpenCL device_id is out of range");
        return false;
    }
    device_ = devices[config_.device_id];

    // 创建上下文
    context_ = funcs.clCreateContext(nullptr, 1, &device_, nullptr, nullptr, &err);
    if (err != CL_SUCCESS) {
        set_last_error("OpenCL context creation failed");
        return false;
    }

    // 创建默认命令队列
    for (int i = 0; i < config_.default_stream_count; ++i) {
        auto queue_wrapper = std::make_shared<CommandQueueWrapper>();
        queue_wrapper->queue = funcs.clCreateCommandQueue(context_, device_, 0, &err);
        if (err != CL_SUCCESS) {
            cleanup_locked();
            set_last_error("OpenCL command queue creation failed");
            return false;
        }
        queues_.push_back(std::move(queue_wrapper));
    }

    is_available_.store(true, std::memory_order_release);
    clear_last_error();
    return true;
}

bool OpenCLExecutor::validate_config() {
    auto message = opencl_config_validation_error(config_);
    if (!message.empty()) {
        set_last_error(message);
        return false;
    }
    return true;
}

void OpenCLExecutor::clear_last_error() {
    std::lock_guard<std::mutex> lock(error_mutex_);
    last_error_message_.clear();
}

void OpenCLExecutor::set_last_error(const std::string& message) {
    std::lock_guard<std::mutex> lock(error_mutex_);
    last_error_message_ = message;
}

std::string OpenCLExecutor::get_last_error() const {
    std::lock_guard<std::mutex> lock(error_mutex_);
    return last_error_message_;
}

void OpenCLExecutor::rollback_start_failure(const std::string& message) {
    running_.store(false, std::memory_order_release);
    set_last_error(message);
    queue_cv_.notify_all();
    queue_not_full_cv_.notify_all();
    queue_drained_cv_.notify_all();
}

void OpenCLExecutor::cleanup() {
    std::unique_lock<std::shared_mutex> lifecycle_lock(lifecycle_mutex_);
    cleanup_locked();
}

void OpenCLExecutor::cleanup_locked() {
    auto funcs = loader_->get_functions();
    std::vector<std::shared_ptr<CommandQueueWrapper>> detached_queues;

    // 释放内存
    {
        std::lock_guard<std::mutex> lock(memory_mutex_);
        for (auto& [ptr, allocation] : memory_map_) {
            if (allocation.buffer) {
                funcs.clReleaseMemObject(allocation.buffer);
            }
        }
        memory_map_.clear();
    }

    // 释放命令队列
    {
        std::lock_guard<std::mutex> lock(queues_mutex_);
        detached_queues.swap(queues_);
    }

    for (auto& queue_wrapper : detached_queues) {
        if (!queue_wrapper) {
            continue;
        }
        std::lock_guard<std::mutex> queue_lock(queue_wrapper->mutex);
        if (queue_wrapper->queue) {
            funcs.clReleaseCommandQueue(queue_wrapper->queue);
            queue_wrapper->queue = nullptr;
        }
    }

    // 释放上下文
    if (context_) {
        funcs.clReleaseContext(context_);
        context_ = nullptr;
    }

    device_ = nullptr;
    platform_ = nullptr;
    is_available_.store(false, std::memory_order_release);
}

void* OpenCLExecutor::allocate_device_memory(size_t size) {
    if (!is_available_.load(std::memory_order_acquire)) {
        return nullptr;
    }
    std::shared_lock<std::shared_mutex> lifecycle_lock(lifecycle_mutex_);
    if (!is_available_.load(std::memory_order_acquire)) {
        return nullptr;
    }

    auto funcs = loader_->get_functions();
    cl_int err;
    cl_mem buffer = funcs.clCreateBuffer(context_, CL_MEM_READ_WRITE, size, nullptr, &err);

    if (err != CL_SUCCESS || !buffer) {
        return nullptr;
    }

    void* ptr = reinterpret_cast<void*>(buffer);

    std::lock_guard<std::mutex> lock(memory_mutex_);
    memory_map_[ptr] = MemoryAllocation{buffer, size};

    return ptr;
}

void OpenCLExecutor::free_device_memory(void* ptr) {
    if (!ptr || !is_available_.load(std::memory_order_acquire)) {
        return;
    }
    std::shared_lock<std::shared_mutex> lifecycle_lock(lifecycle_mutex_);
    if (!is_available_.load(std::memory_order_acquire)) {
        return;
    }

    std::lock_guard<std::mutex> lock(memory_mutex_);
    auto it = memory_map_.find(ptr);
    if (it != memory_map_.end()) {
        auto funcs = loader_->get_functions();
        funcs.clReleaseMemObject(it->second.buffer);
        memory_map_.erase(it);
    }
}

bool OpenCLExecutor::validate_memory_range(const void* device_ptr, std::size_t size,
                                           const char* api_name) {
    const char* api = (api_name && api_name[0] != '\0') ? api_name : "OpenCL copy";
    const auto it = memory_map_.find(const_cast<void*>(device_ptr));
    if (it == memory_map_.end()) {
        set_last_error(std::string(api) + ": device pointer was not allocated by this executor");
        return false;
    }
    if (size == 0) {
        set_last_error(std::string(api) + ": requested copy size must be greater than zero bytes");
        return false;
    }
    if (size > it->second.size) {
        std::ostringstream oss;
        oss << api << ": requested " << size << " bytes exceeds allocation size "
            << it->second.size << " bytes";
        set_last_error(oss.str());
        return false;
    }
    return true;
}

bool OpenCLExecutor::copy_to_device(void* dst, const void* src, size_t size, bool async, int stream_id) {
    if (!is_available_.load(std::memory_order_acquire)) {
        return false;
    }
    std::shared_lock<std::shared_mutex> lifecycle_lock(lifecycle_mutex_);
    if (!is_available_.load(std::memory_order_acquire)) {
        return false;
    }

    auto queue_wrapper = get_queue(stream_id);
    if (!queue_wrapper) {
        return false;
    }

    std::lock_guard<std::mutex> lock(memory_mutex_);
    if (src == nullptr) {
        set_last_error("copy_to_device: host source pointer must not be null");
        return false;
    }
    if (!validate_memory_range(dst, size, "copy_to_device destination")) {
        return false;
    }
    const auto it = memory_map_.find(dst);

    auto funcs = loader_->get_functions();
    std::lock_guard<std::mutex> queue_lock(queue_wrapper->mutex);
    if (!queue_wrapper->queue) {
        return false;
    }
    cl_int err = funcs.clEnqueueWriteBuffer(
        queue_wrapper->queue, it->second.buffer, async ? CL_FALSE : CL_TRUE,
        0, size, src, 0, nullptr, nullptr);

    return check_opencl_error(err, "clEnqueueWriteBuffer");
}

bool OpenCLExecutor::copy_to_host(void* dst, const void* src, size_t size, bool async, int stream_id) {
    if (!is_available_.load(std::memory_order_acquire)) {
        return false;
    }
    std::shared_lock<std::shared_mutex> lifecycle_lock(lifecycle_mutex_);
    if (!is_available_.load(std::memory_order_acquire)) {
        return false;
    }

    auto queue_wrapper = get_queue(stream_id);
    if (!queue_wrapper) {
        return false;
    }

    std::lock_guard<std::mutex> lock(memory_mutex_);
    if (dst == nullptr) {
        set_last_error("copy_to_host: host destination pointer must not be null");
        return false;
    }
    if (!validate_memory_range(src, size, "copy_to_host source")) {
        return false;
    }
    const auto it = memory_map_.find(const_cast<void*>(src));

    auto funcs = loader_->get_functions();
    std::lock_guard<std::mutex> queue_lock(queue_wrapper->mutex);
    if (!queue_wrapper->queue) {
        return false;
    }
    cl_int err = funcs.clEnqueueReadBuffer(
        queue_wrapper->queue, it->second.buffer, async ? CL_FALSE : CL_TRUE,
        0, size, dst, 0, nullptr, nullptr);

    return check_opencl_error(err, "clEnqueueReadBuffer");
}

bool OpenCLExecutor::copy_device_to_device(void* dst, const void* src, size_t size, bool async, int stream_id) {
    if (!is_available_.load(std::memory_order_acquire)) {
        return false;
    }
    std::shared_lock<std::shared_mutex> lifecycle_lock(lifecycle_mutex_);
    if (!is_available_.load(std::memory_order_acquire)) {
        return false;
    }

    auto queue_wrapper = get_queue(stream_id);
    if (!queue_wrapper) {
        return false;
    }

    std::lock_guard<std::mutex> lock(memory_mutex_);
    if (!validate_memory_range(src, size, "copy_device_to_device source") ||
        !validate_memory_range(dst, size, "copy_device_to_device destination")) {
        return false;
    }
    const auto src_it = memory_map_.find(const_cast<void*>(src));
    const auto dst_it = memory_map_.find(dst);

    auto funcs = loader_->get_functions();
    std::lock_guard<std::mutex> queue_lock(queue_wrapper->mutex);
    if (!queue_wrapper->queue) {
        return false;
    }
    cl_int err = funcs.clEnqueueCopyBuffer(
        queue_wrapper->queue, src_it->second.buffer, dst_it->second.buffer,
        0, 0, size, 0, nullptr, nullptr);

    if (!check_opencl_error(err, "clEnqueueCopyBuffer")) {
        return false;
    }
    if (!async) {
        return check_opencl_error(funcs.clFinish(queue_wrapper->queue), "clFinish");
    }
    return true;
}

bool OpenCLExecutor::copy_from_peer(IGpuExecutor* src_executor, const void* src_ptr,
                                    void* dst_ptr, size_t size, bool async, int stream_id) {
    // OpenCL不直接支持P2P，需要通过主机内存中转
    return false;
}

void OpenCLExecutor::synchronize() {
    if (!is_available_.load(std::memory_order_acquire)) {
        return;
    }
    std::shared_lock<std::shared_mutex> lifecycle_lock(lifecycle_mutex_);
    if (!is_available_.load(std::memory_order_acquire)) {
        return;
    }

    auto funcs = loader_->get_functions();
    std::vector<std::shared_ptr<CommandQueueWrapper>> queue_snapshot;

    {
        std::lock_guard<std::mutex> lock(queues_mutex_);
        queue_snapshot = queues_;
    }

    for (auto& queue_wrapper : queue_snapshot) {
        if (queue_wrapper) {
            std::lock_guard<std::mutex> queue_lock(queue_wrapper->mutex);
            if (!queue_wrapper->queue) {
                continue;
            }
            check_opencl_error(funcs.clFinish(queue_wrapper->queue), "clFinish");
        }
    }
}

void OpenCLExecutor::synchronize_stream(int stream_id) {
    if (!is_available_.load(std::memory_order_acquire)) {
        return;
    }
    std::shared_lock<std::shared_mutex> lifecycle_lock(lifecycle_mutex_);
    if (!is_available_.load(std::memory_order_acquire)) {
        return;
    }
    auto queue_wrapper = get_queue(stream_id);
    if (!queue_wrapper) {
        return;
    }

    auto funcs = loader_->get_functions();
    std::lock_guard<std::mutex> lock(queue_wrapper->mutex);
    if (!queue_wrapper->queue) {
        return;
    }
    check_opencl_error(funcs.clFinish(queue_wrapper->queue), "clFinish");
}

int OpenCLExecutor::create_stream() {
    if (!is_available_.load(std::memory_order_acquire)) {
        return -1;
    }
    std::shared_lock<std::shared_mutex> lifecycle_lock(lifecycle_mutex_);
    if (!is_available_.load(std::memory_order_acquire)) {
        return -1;
    }

    auto funcs = loader_->get_functions();
    cl_int err;

    auto queue_wrapper = std::make_shared<CommandQueueWrapper>();
    queue_wrapper->queue = funcs.clCreateCommandQueue(context_, device_, 0, &err);

    if (err != CL_SUCCESS) {
        return -1;
    }

    std::lock_guard<std::mutex> lock(queues_mutex_);
    int stream_id = static_cast<int>(queues_.size());
    queues_.push_back(std::move(queue_wrapper));

    return stream_id;
}

void OpenCLExecutor::destroy_stream(int stream_id) {
    if (!is_available_.load(std::memory_order_acquire)) {
        return;
    }
    std::shared_lock<std::shared_mutex> lifecycle_lock(lifecycle_mutex_);
    if (!is_available_.load(std::memory_order_acquire)) {
        return;
    }
    if (stream_id < static_cast<int>(config_.default_stream_count)) {
        return;
    }

    std::shared_ptr<CommandQueueWrapper> queue_wrapper;

    {
        std::lock_guard<std::mutex> lock(queues_mutex_);
        if (stream_id >= 0 && stream_id < static_cast<int>(queues_.size())) {
            queue_wrapper = std::move(queues_[stream_id]);
            queues_[stream_id].reset();
        }
    }

    if (queue_wrapper) {
        auto funcs = loader_->get_functions();
        std::lock_guard<std::mutex> queue_lock(queue_wrapper->mutex);
        if (queue_wrapper->queue) {
            funcs.clReleaseCommandQueue(queue_wrapper->queue);
            queue_wrapper->queue = nullptr;
        }
    }
}

bool OpenCLExecutor::add_stream_callback(int stream_id, std::function<void()> callback) {
    if (!callback) {
        set_last_error("OpenCL stream callback failed: callback is null");
        return false;
    }
    if (!get_queue(stream_id)) {
        set_last_error("OpenCL stream callback failed: invalid stream_id " +
                       std::to_string(stream_id));
        return false;
    }

    set_last_error("OpenCL stream callbacks are not supported; cl_event polling is a follow-up");
    return false;
}

bool OpenCLExecutor::supports_stream_callback() const noexcept {
    return false;
}

std::string OpenCLExecutor::get_name() const {
    return name_;
}

GpuDeviceInfo OpenCLExecutor::get_device_info() const {
    GpuDeviceInfo info;
    info.name = name_;
    info.backend = GpuBackend::OPENCL;
    info.device_id = config_.device_id;

    if (!is_available_.load(std::memory_order_acquire)) {
        return info;
    }
    std::shared_lock<std::shared_mutex> lifecycle_lock(lifecycle_mutex_);
    if (is_available_.load(std::memory_order_acquire) && device_) {
        auto funcs = loader_->get_functions();
        char device_name[256] = {0};
        funcs.clGetDeviceInfo(device_, 0x102B, sizeof(device_name), device_name, nullptr); // CL_DEVICE_NAME
        info.name = device_name;

        cl_ulong mem_size = 0;
        funcs.clGetDeviceInfo(device_, 0x101F, sizeof(mem_size), &mem_size, nullptr); // CL_DEVICE_GLOBAL_MEM_SIZE
        info.total_memory_bytes = static_cast<size_t>(mem_size);
    }

    return info;
}

GpuExecutorStatus OpenCLExecutor::get_status() const {
    GpuExecutorStatus status;
    status.name = name_;
    status.is_running = running_.load(std::memory_order_acquire);
    status.backend = GpuBackend::OPENCL;
    status.device_id = config_.device_id;
    status.last_error_message = get_last_error();
    status.active_kernels = active_kernels_.load();
    status.completed_kernels = completed_kernels_.load();
    status.failed_kernels = failed_kernels_.load();
    status.queue_capacity = config_.max_queue_size;
    std::shared_lock<std::shared_mutex> lifecycle_lock(lifecycle_mutex_);
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        status.queue_size = task_queue_.size();
    }
    {
        std::lock_guard<std::mutex> lock(memory_mutex_);
        for (const auto& [ptr, allocation] : memory_map_) {
            (void)ptr;
            status.memory_used_bytes += allocation.size;
        }
    }

    if (is_available_.load(std::memory_order_acquire) && device_) {
        auto funcs = loader_->get_functions();
        cl_ulong mem_size = 0;
        cl_int err = funcs.clGetDeviceInfo(
            device_, 0x101F, sizeof(mem_size), &mem_size, nullptr); // CL_DEVICE_GLOBAL_MEM_SIZE
        if (err == CL_SUCCESS) {
            status.memory_total_bytes = static_cast<size_t>(mem_size);
            if (status.memory_total_bytes > 0) {
                status.memory_usage_percent =
                    (static_cast<double>(status.memory_used_bytes) /
                     static_cast<double>(status.memory_total_bytes)) * 100.0;
            }
        }
    }

    size_t completed = completed_kernels_.load();
    if (completed > 0) {
        status.avg_kernel_time_ms =
            static_cast<double>(total_kernel_time_ns_.load()) / 1000000.0 /
            static_cast<double>(completed);
    }

    return status;
}

std::future<void> OpenCLExecutor::submit_kernel_impl(
    std::function<void(void*)> kernel_func,
    const GpuTaskConfig& config) {
    if (!validate_config()) {
        std::promise<void> promise;
        promise.set_exception(std::make_exception_ptr(
            std::runtime_error("OpenCLExecutor invalid configuration: " + get_last_error())));
        return promise.get_future();
    }

    auto promise = std::make_shared<std::promise<void>>();
    auto future = promise->get_future();
    QueuedTask task;
    task.promise = promise;
    task.run = [this, kernel_func, config, promise]() {
        auto start = std::chrono::high_resolution_clock::now();
        bool kernel_func_started = false;

        try {
            std::shared_ptr<CommandQueueWrapper> queue_wrapper;
            {
                std::shared_lock<std::shared_mutex> lifecycle_lock(lifecycle_mutex_);
                if (!is_available_.load(std::memory_order_acquire)) {
                    throw std::runtime_error("OpenCLExecutor is unavailable");
                }
                queue_wrapper = get_queue(config.stream_id);
                if (!queue_wrapper) {
                    std::ostringstream oss;
                    oss << "submit_kernel: invalid stream_id " << config.stream_id;
                    throw std::runtime_error(oss.str());
                }
            }

            std::lock_guard<std::mutex> queue_lock(queue_wrapper->mutex);
            if (!queue_wrapper->queue) {
                std::ostringstream oss;
                oss << "submit_kernel: invalid stream_id " << config.stream_id;
                throw std::runtime_error(oss.str());
            }
            kernel_func_started = true;
            kernel_func(queue_wrapper->queue);

            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
            total_kernel_time_ns_ += duration;
            completed_kernels_++;
            promise->set_value();
        } catch (const std::exception& ex) {
            if (kernel_func_started) {
                set_last_error(std::string("submit_kernel: kernel_func exception: ") + ex.what());
            } else {
                set_last_error(ex.what());
            }
            failed_kernels_++;
            promise->set_exception(std::current_exception());
        } catch (...) {
            set_last_error(kernel_func_started
                ? "submit_kernel: kernel_func exception: unknown"
                : "submit_kernel: unknown exception");
            failed_kernels_++;
            promise->set_exception(std::current_exception());
        }
    };

    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        queue_not_full_cv_.wait(lock, [this] {
            return !running_.load(std::memory_order_acquire) ||
                   task_queue_.size() < config_.max_queue_size;
        });
        if (!running_.load(std::memory_order_acquire)) {
            std::promise<void> stopped_promise;
            stopped_promise.set_exception(std::make_exception_ptr(
                std::runtime_error("OpenCLExecutor is not running")));
            return stopped_promise.get_future();
        }

        task_queue_.push(std::move(task));
    }
    queue_cv_.notify_one();

    return future;
}

void OpenCLExecutor::worker_thread() {
    {
        // 注册 worker 线程身份，供 stop() 识别"从 kernel 内部调用 stop()"
        // 的自停场景（对齐 CudaExecutor::worker_thread_func 的做法）。
        std::unique_lock<std::shared_mutex> lifecycle_lock(lifecycle_mutex_);
        worker_id_ = std::this_thread::get_id();
    }

    while (true) {
        QueuedTask task;

        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] {
                return !task_queue_.empty() ||
                       !running_.load(std::memory_order_acquire);
            });

            if (!running_.load(std::memory_order_acquire) && task_queue_.empty()) {
                queue_drained_cv_.notify_all();
                break;
            }

            task = std::move(task_queue_.front());
            task_queue_.pop();
            active_kernels_++;
            queue_not_full_cv_.notify_one();
        }

        if (task.run) {
            task.run();
        }

        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            active_kernels_--;
            if (task_queue_.empty() && active_kernels_.load() == 0) {
                queue_drained_cv_.notify_all();
            }
        }
    }
}

std::shared_ptr<OpenCLExecutor::CommandQueueWrapper> OpenCLExecutor::get_queue(int stream_id) {
    std::lock_guard<std::mutex> lock(queues_mutex_);

    if (stream_id < 0 || stream_id >= static_cast<int>(queues_.size())) {
        return nullptr;
    }

    return queues_[stream_id];
}

bool OpenCLExecutor::check_opencl_error(cl_int error, const char* operation) {
    if (error == CL_SUCCESS) {
        return true;
    }

    std::ostringstream oss;
    oss << (operation && operation[0] != '\0' ? operation : "OpenCL operation")
        << ": " << opencl_error_name(error)
        << " (" << error << ")";
    set_last_error(oss.str());
    return false;
}

} // namespace gpu
} // namespace kairo
