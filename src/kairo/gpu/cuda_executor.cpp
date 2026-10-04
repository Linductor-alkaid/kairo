#include "cuda_executor.hpp"
#include "gpu_memory_manager.hpp"
#include <chrono>
#include <cstdio>
#include <exception>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <functional>
#include <memory>

#ifdef KAIRO_ENABLE_CUDA
#include <cuda_runtime.h>
#ifdef _WIN32
#include <windows.h>
#endif
#endif

namespace {

constexpr const char* kInvalidCudaDeviceIdMessage = "CUDA device_id must be >= 0";

std::string invalid_stream_message(int stream_id) {
    return "CudaExecutor: stream_id " + std::to_string(stream_id) + " is invalid or destroyed";
}

struct StreamCallbackContext {
    std::function<void()> callback;
};

void stream_host_callback(void* userData) {
    std::unique_ptr<StreamCallbackContext> ctx(
        static_cast<StreamCallbackContext*>(userData));
    if (!ctx || !ctx->callback) {
        return;
    }

    try {
        ctx->callback();
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "CudaExecutor stream host callback threw: %s\n", exception.what());
    } catch (...) {
        std::fputs("CudaExecutor stream host callback threw a non-standard exception\n", stderr);
    }
}

}  // namespace

namespace kairo {
namespace gpu {

CudaExecutor::CudaExecutor(const std::string& name, const GpuExecutorConfig& config)
    : name_(name)
    , config_(config)
    , device_id_(config.device_id)
    , is_available_(false)
    , loader_(&CudaLoader::instance())
#ifdef KAIRO_ENABLE_CUDA
    , default_stream_(nullptr)
#endif
{
    if (!validate_config()) {
        return;
    }

    try {
        if (!validate_gpu_config(config_)) {
            set_last_error(gpu_config_validation_error(config_));
            is_available_ = false;
            return;
        }

        // 尝试加载CUDA DLL
        if (!loader_->load()) {
            set_last_error("CUDA loader is unavailable");
            is_available_ = false;
            return;
        }

        // 检查 CUDA 是否可用
        if (!check_cuda_available()) {
            set_last_error("CUDA runtime or device is unavailable");
            is_available_ = false;
            return;
        }

        // 初始化设备
        if (!initialize_device()) {
            set_last_error("CUDA device initialization failed");
            is_available_ = false;
            return;
        }

        is_available_ = true;
        clear_last_error();
    } catch (...) {
        // 捕获所有异常，确保构造函数不会抛出
        set_last_error("CUDA initialization threw an exception");
        is_available_ = false;
    }
}

CudaExecutor::~CudaExecutor() {
    stop();

    memory_manager_.reset();

#ifdef KAIRO_ENABLE_CUDA
    if (is_available_ && loader_->is_available()) {
        (void)ensure_device_context();  // 多 GPU 场景下确保操作本设备
        auto funcs = loader_->get_functions();
        
        // 释放所有已分配的内存（仅未使用内存池时 allocated_memory_ 非空）
        {
            std::lock_guard<std::mutex> lock(memory_mutex_);
            for (auto& [ptr, allocation] : allocated_memory_) {
                if (ptr != nullptr && allocation.free_directly && funcs.cudaFree != nullptr) {
                    funcs.cudaFree(ptr);
                }
            }
            allocated_memory_.clear();
        }

        // 销毁所有流（除了默认流）
        std::vector<std::shared_ptr<StreamWrapper>> detached_streams;
        {
            std::lock_guard<std::mutex> lock(streams_mutex_);
            detached_streams.swap(streams_);
        }
        for (auto& stream_wrapper : detached_streams) {
            destroy_stream_wrapper(stream_wrapper);
        }
    }
#endif
}

bool CudaExecutor::check_cuda_available() {
#ifdef KAIRO_ENABLE_CUDA
    if (!loader_->is_available()) {
        set_last_error("CUDA loader is unavailable");
        return false;
    }

    auto funcs = loader_->get_functions();
    if (!funcs.is_complete()) {
        set_last_error("CUDA runtime symbols are incomplete");
        return false;
    }

    try {
        // 尝试初始化 CUDA 运行时（通过调用 cudaFree(0)）
        cudaError_t error = funcs.cudaFree(nullptr);
        if (!check_cuda_error(error, "cudaFree(0)")) {
            return false;
        }

        // 检查设备数量
        int device_count = 0;
        error = funcs.cudaGetDeviceCount(&device_count);
        if (!check_cuda_error(error, "cudaGetDeviceCount")) {
            return false;
        }
        if (device_count == 0) {
            set_last_error("CUDA device enumeration failed: no CUDA devices found");
            return false;
        }

        return true;
    } catch (...) {
        // 捕获所有C++异常
        set_last_error("CUDA availability check threw an exception");
        return false;
    }
#else
    set_last_error("CUDA support is not enabled");
    return false;
#endif
}

bool CudaExecutor::initialize_device() {
#ifdef KAIRO_ENABLE_CUDA
    if (!validate_config()) {
        return false;
    }

    if (!loader_->is_available()) {
        set_last_error("CUDA loader is unavailable");
        return false;
    }

    auto funcs = loader_->get_functions();
    if (!funcs.is_complete()) {
        set_last_error("CUDA runtime symbols are incomplete");
        return false;
    }

    try {
        // 检查设备ID是否有效
        int device_count = 0;
        cudaError_t error = funcs.cudaGetDeviceCount(&device_count);
        if (!check_cuda_error(error, "cudaGetDeviceCount")) {
            return false;
        }

        if (device_id_ >= device_count) {
            std::ostringstream oss;
            oss << "CUDA device_id " << device_id_
                << " is out of range for " << device_count << " device(s)";
            set_last_error(oss.str());
            return false;
        }

        // 设置当前设备
        error = funcs.cudaSetDevice(device_id_);
        if (!check_cuda_error(error, "cudaSetDevice")) {
            return false;
        }

        // 获取设备属性
        error = funcs.cudaGetDeviceProperties(&device_prop_, device_id_);
        if (!check_cuda_error(error, "cudaGetDeviceProperties")) {
            return false;
        }

        // 默认流就是 nullptr（CUDA 默认流）
        default_stream_ = nullptr;

        clear_last_error();
        return true;
    } catch (...) {
        // 捕获所有C++异常
        set_last_error("CUDA device initialization threw an exception");
        return false;
    }
#else
    set_last_error("CUDA support is not enabled");
    return false;
#endif
}

bool CudaExecutor::validate_config() const {
    if (device_id_ < 0) {
        set_last_error(kInvalidCudaDeviceIdMessage);
        return false;
    }
    return true;
}

#ifdef KAIRO_ENABLE_CUDA
bool CudaExecutor::check_cuda_error(cudaError_t error_code, const char* operation) const {
    if (error_code != cudaSuccess) {
        std::string message = std::string("CudaExecutor: ") + operation + " failed";
        if (loader_->is_available()) {
            auto funcs = loader_->get_functions();
            if (funcs.cudaGetErrorString != nullptr) {
                const char* msg = funcs.cudaGetErrorString(error_code);
                if (msg && msg[0] != '\0') {
                    message += ": ";
                    message += msg;
                }
                std::fprintf(stderr, "CudaExecutor: %s failed: %s\n",
                    operation, msg ? msg : "unknown");
            }
        }
        set_last_error(message);
        return false;
    }
    return true;
}

std::exception_ptr CudaExecutor::make_cuda_exception_ptr(cudaError_t error_code, const char* operation) const {
    std::string msg = std::string(operation) + " failed";
    if (loader_->is_available()) {
        auto funcs = loader_->get_functions();
        if (funcs.cudaGetErrorString != nullptr) {
            const char* errStr = funcs.cudaGetErrorString(error_code);
            if (errStr && errStr[0] != '\0') {
                msg += ": ";
                msg += errStr;
            }
        }
    }
    return std::make_exception_ptr(std::runtime_error(msg));
}
#else
bool CudaExecutor::check_cuda_error(int error_code, const char* operation) const {
    (void)error_code;
    set_last_error(std::string("CudaExecutor: ") + operation + " failed: CUDA support is not enabled");
    return false;
}
#endif

bool CudaExecutor::ensure_device_context() const {
#ifdef KAIRO_ENABLE_CUDA
    if (!loader_->is_available()) {
        set_last_error("CUDA loader is unavailable");
        return false;
    }
    auto funcs = loader_->get_functions();
    if (funcs.cudaSetDevice == nullptr) {
        set_last_error("CUDA cudaSetDevice symbol is unavailable");
        return false;
    }
    cudaError_t err = funcs.cudaSetDevice(device_id_);
    return check_cuda_error(err, "cudaSetDevice");
#else
    return false;
#endif
}

bool CudaExecutor::start() {
    if (!validate_gpu_config(config_)) {
        set_last_error(gpu_config_validation_error(config_));
        return false;
    }

    std::lock_guard<std::mutex> stop_lock(stop_mutex_);
    if (!is_available_.load(std::memory_order_acquire)) {
        return false;
    }

    if (!validate_config()) {
        return false;
    }

    bool expected = false;
    if (worker_thread_.joinable() || !worker_joined_ ||
        !is_running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return false;  // 已经在运行
    }

    worker_id_ = std::thread::id{};
    self_stop_requested_.store(false, std::memory_order_release);

    clear_last_error();

#ifdef KAIRO_ENABLE_CUDA
    if (!loader_->is_available()) {
        set_last_error("CUDA loader is unavailable");
        // CR-002: is_running_=true 窗口内的提交必须落终态，否则队列无消费者、
        // stop()/wait_for_completion() 永久挂死（下同）。
        fail_all_queued_tasks("CudaExecutor: start failed (CUDA loader is unavailable)");
        is_running_.store(false);
        return false;
    }

    auto funcs = loader_->get_functions();
    if (funcs.cudaStreamCreate == nullptr || funcs.cudaStreamDestroy == nullptr) {
        set_last_error("CUDA stream create/destroy symbols are unavailable");
        fail_all_queued_tasks(
            "CudaExecutor: start failed (stream symbols unavailable)");
        is_running_.store(false);
        return false;
    }

    bool needs_default_streams = false;
    {
        std::lock_guard<std::mutex> lock(streams_mutex_);
        needs_default_streams = streams_.empty() && config_.default_stream_count > 0;
    }

    if (needs_default_streams) {
        std::vector<std::shared_ptr<StreamWrapper>> created_streams;
        for (int i = 0; i < config_.default_stream_count; ++i) {
            cudaStream_t s = create_one_stream();
            if (s == nullptr) {
                for (auto& stream_wrapper : created_streams) {
                    destroy_stream_wrapper(stream_wrapper);
                }
                if (get_last_error().empty()) {
                    set_last_error("CUDA default stream creation failed");
                }
                fail_all_queued_tasks(
                    "CudaExecutor: start failed (default stream creation failed)");
                is_running_.store(false);
                return false;
            }
            auto stream_wrapper = std::make_shared<StreamWrapper>();
            stream_wrapper->stream = s;
            created_streams.push_back(std::move(stream_wrapper));
        }

        std::lock_guard<std::mutex> lock(streams_mutex_);
        if (streams_.empty()) {
            streams_ = std::move(created_streams);
        } else {
            for (auto& stream_wrapper : created_streams) {
                destroy_stream_wrapper(stream_wrapper);
            }
        }
    }

    if (config_.memory_pool_size > 0) {
        memory_manager_ = std::make_unique<GpuMemoryManager>(
            [this](size_t sz) { return raw_allocate_device_memory(sz); },
            [this](void* p) { raw_free_device_memory(p); },
            config_.memory_pool_size);
    }

    try {
        worker_thread_ = std::thread(&CudaExecutor::worker_thread_func, this);
        worker_joined_ = false;
        worker_started_.store(true, std::memory_order_release);
    } catch (...) {
        fail_all_queued_tasks(
            "CudaExecutor: start failed (worker thread creation failed)");
        is_running_.store(false, std::memory_order_release);
        set_last_error("CUDA worker thread creation failed");
        worker_joined_ = true;
        return false;
    }
#else
    set_last_error("CUDA support is not enabled");
    fail_all_queued_tasks("CudaExecutor: start failed (CUDA not enabled)");
    is_running_.store(false);
    return false;
#endif

    clear_last_error();
    // Reopen dependency-waiter admission only after startup succeeds.
    start_waiter_generation();
    return true;
}

void CudaExecutor::fail_all_queued_tasks(const std::string& message) {
    // CR-002: 供 start() 半途失败路径调用。此刻 worker 未启动，队列中的
    // 任务永远无人消费——逐个 set_exception 落终态并唤醒排空等待者，
    // 否则 stop() → wait_for_completion() 在 queue_drained_cv_ 上永久挂死。
    std::exception_ptr eptr = std::make_exception_ptr(std::runtime_error(message));
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        while (!task_queue_.empty()) {
            auto& top = const_cast<GpuQueuedTask&>(task_queue_.top());
            GpuQueuedTask task = std::move(top);
            task_queue_.pop();
            if (task.promise) {
                task.promise->set_exception(eptr);
            }
        }
    }
    queue_drained_cv_.notify_all();
}

void CudaExecutor::stop() {
    (void)stop_and_join();
}

bool CudaExecutor::stop_and_join() {
    std::thread joiner;
    {
        std::lock_guard<std::mutex> stop_lock(stop_mutex_);
        if (std::this_thread::get_id() == worker_id_) {
            self_stop_requested_.store(true, std::memory_order_release);
            is_running_.store(false, std::memory_order_release);
            close_waiter_admission();
            queue_not_empty_cv_.notify_all();
            queue_not_full_cv_.notify_all();
            return false;
        }

        if (!is_available_.load(std::memory_order_acquire)) {
            return true;
        }

        is_running_.store(false, std::memory_order_release);
        if (worker_thread_.joinable()) {
            joiner = std::move(worker_thread_);
        }
    }

    // P-002 fix: signal + join all submit_kernel_after waiter threads before
    // tearing down internal state, preventing UAF when a waiter accesses
    // submit_kernel_impl() on a partially-destroyed object.
    join_pending_waiters();

    is_running_.store(false);
    // P-260618-005: also notify the queue_not_full_cv_ waiters so submitters
    // blocked on a full queue wake up, observe !is_running_, and return
    // a failed future instead of deadlocking.
    queue_not_empty_cv_.notify_all();
    queue_not_full_cv_.notify_all();

    wait_for_completion();

    queue_not_empty_cv_.notify_all();
    if (joiner.joinable()) {
        joiner.join();
        std::lock_guard<std::mutex> stop_lock(stop_mutex_);
        worker_joined_ = true;
        worker_id_ = std::thread::id{};
        worker_started_.store(false, std::memory_order_release);
    }
    return true;
}

void CudaExecutor::set_exception_callback(
    std::function<void(const std::string&, std::exception_ptr)> callback) {
    exception_handler_.set_exception_callback(std::move(callback));
}

void CudaExecutor::clear_last_error() const {
    std::lock_guard<std::mutex> lock(error_mutex_);
    last_error_message_.clear();
}

void CudaExecutor::set_last_error(const std::string& message) const {
    std::lock_guard<std::mutex> lock(error_mutex_);
    last_error_message_ = message;
}

std::string CudaExecutor::get_last_error() const {
    std::lock_guard<std::mutex> lock(error_mutex_);
    return last_error_message_;
}

bool CudaExecutor::register_external_memory(void* ptr, size_t size) {
    if (ptr == nullptr || size == 0) {
        set_last_error("CUDA external memory registration requires a non-null pointer and non-zero size");
        return false;
    }
    std::lock_guard<std::mutex> lock(memory_mutex_);
    if (allocated_memory_.find(ptr) != allocated_memory_.end()) {
        set_last_error("CUDA external memory registration conflicts with an existing allocation");
        return false;
    }
    allocated_memory_.emplace(ptr, AllocationRecord{size, AllocationKind::ExternalOptIn, false});
    return true;
}

void CudaExecutor::unregister_external_memory(void* ptr) {
    std::lock_guard<std::mutex> lock(memory_mutex_);
    auto it = allocated_memory_.find(ptr);
    if (it != allocated_memory_.end() && it->second.kind == AllocationKind::ExternalOptIn) {
        allocated_memory_.erase(it);
    }
}

bool CudaExecutor::validate_memory_range(const void* ptr, size_t size, const char* argument) const {
    std::lock_guard<std::mutex> lock(memory_mutex_);
    const auto it = allocated_memory_.find(const_cast<void*>(ptr));
    if (it == allocated_memory_.end()) {
        set_last_error(std::string("CUDA ") + argument + " memory was not allocated by this executor");
        return false;
    }
    if (size > it->second.size) {
        std::ostringstream oss;
        oss << "CUDA " << argument << " memory range overflows its allocation (requested "
            << size << " bytes, allocated " << it->second.size << " bytes)";
        set_last_error(oss.str());
        return false;
    }
    return true;
}

void CudaExecutor::wait_for_completion() {
    if (!is_available_) {
        return;
    }

#ifdef KAIRO_ENABLE_CUDA
    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        // CR-002: start() 半途失败时 worker 从未启动，队列中的任务无人排空，
        // 谓词若只看 empty/active 会永久等待。worker_started_ 为 false（从未
        // 启动，或上一轮已完整 join）时队列不可能再有消费者，直接退出；
        // 正常 stop 路径 is_running_=false 但 worker 仍在排空，此时
        // worker_started_ 仍为 true，不会提前返回。
        queue_drained_cv_.wait(lock, [this] {
            return (task_queue_.empty() && active_kernels_.load() == 0) ||
                   !worker_started_.load(std::memory_order_acquire);
        });
        if (!worker_started_.load(std::memory_order_acquire)) {
            return;  // 无 worker 可同步，设备侧无在飞工作
        }
    }
    synchronize();
#endif
}

void* CudaExecutor::raw_allocate_device_memory(size_t size) {
#ifdef KAIRO_ENABLE_CUDA
    if (!is_available_ || !is_running_.load() || !loader_->is_available()) {
        return nullptr;
    }
    if (!ensure_device_context()) {
        return nullptr;
    }
    auto funcs = loader_->get_functions();
    if (funcs.cudaMalloc == nullptr) {
        set_last_error("CUDA cudaMalloc symbol is unavailable");
        return nullptr;
    }
    void* ptr = nullptr;
    cudaError_t error = funcs.cudaMalloc(&ptr, size);
    if (!check_cuda_error(error, "cudaMalloc") || ptr == nullptr) {
        return nullptr;
    }
    return ptr;
#else
    (void)size;
    return nullptr;
#endif
}

void CudaExecutor::raw_free_device_memory(void* ptr) {
#ifdef KAIRO_ENABLE_CUDA
    if (!is_available_ || ptr == nullptr || !loader_->is_available()) {
        return;
    }
    if (!ensure_device_context()) {
        return;
    }
    auto funcs = loader_->get_functions();
    if (funcs.cudaFree != nullptr) {
        cudaError_t error = funcs.cudaFree(ptr);
        check_cuda_error(error, "cudaFree");
    } else {
        set_last_error("CUDA cudaFree symbol is unavailable");
    }
#else
    (void)ptr;
#endif
}

void* CudaExecutor::allocate_device_memory(size_t size) {
    if (!is_available_ || !is_running_.load() || !loader_->is_available()) {
        return nullptr;
    }

    if (memory_manager_) {
        void* ptr = memory_manager_->allocate(size);
        if (ptr != nullptr) {
            std::lock_guard<std::mutex> lock(memory_mutex_);
            allocated_memory_[ptr] = AllocationRecord{size, AllocationKind::Owned, false};
        }
        return ptr;
    }

#ifdef KAIRO_ENABLE_CUDA
    if (!ensure_device_context()) {
        return nullptr;
    }
    auto funcs = loader_->get_functions();
    if (funcs.cudaMalloc == nullptr) {
        set_last_error("CUDA cudaMalloc symbol is unavailable");
        return nullptr;
    }

    void* ptr = nullptr;
    cudaError_t error = funcs.cudaMalloc(&ptr, size);
    if (!check_cuda_error(error, "cudaMalloc") || ptr == nullptr) {
        return nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(memory_mutex_);
        allocated_memory_[ptr] = AllocationRecord{size, AllocationKind::Owned, true};
    }

    return ptr;
#else
    (void)size;
    return nullptr;
#endif
}

void CudaExecutor::free_device_memory(void* ptr) {
    if (!is_available_ || ptr == nullptr || !loader_->is_available()) {
        return;
    }

    if (memory_manager_) {
        {
            std::lock_guard<std::mutex> lock(memory_mutex_);
            auto it = allocated_memory_.find(ptr);
            if (it == allocated_memory_.end() || it->second.kind != AllocationKind::Owned) {
                return;
            }
            allocated_memory_.erase(it);
        }
        memory_manager_->free(ptr);
        return;
    }

#ifdef KAIRO_ENABLE_CUDA
    if (!ensure_device_context()) {
        return;
    }
    auto funcs = loader_->get_functions();
    if (funcs.cudaFree == nullptr) {
        set_last_error("CUDA cudaFree symbol is unavailable");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(memory_mutex_);
        auto it = allocated_memory_.find(ptr);
        if (it == allocated_memory_.end() || it->second.kind != AllocationKind::Owned ||
            !it->second.free_directly) {
            return;
        }
        allocated_memory_.erase(it);
    }

    cudaError_t error = funcs.cudaFree(ptr);
    check_cuda_error(error, "cudaFree");
#else
    (void)ptr;
#endif
}

void* CudaExecutor::allocate_unified_memory(size_t size) {
    if (!is_available_ || !is_running_.load() || !loader_->is_available()) {
        return nullptr;
    }

    if (!config_.enable_unified_memory) {
        set_last_error("CUDA unified memory is not enabled");
        return nullptr;  // 未启用统一内存
    }

#ifdef KAIRO_ENABLE_CUDA
    if (!ensure_device_context()) {
        return nullptr;
    }
    auto funcs = loader_->get_functions();
    if (!funcs.is_unified_memory_available() || funcs.cudaMallocManaged == nullptr) {
        set_last_error("CUDA unified memory symbols are unavailable");
        return nullptr;  // 不支持统一内存
    }

    void* ptr = nullptr;
    cudaError_t error = funcs.cudaMallocManaged(&ptr, size, cudaMemAttachGlobal);
    if (!check_cuda_error(error, "cudaMallocManaged") || ptr == nullptr) {
        return nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(memory_mutex_);
        allocated_memory_[ptr] = AllocationRecord{size, AllocationKind::Owned, true};
    }

    return ptr;
#else
    (void)size;
    return nullptr;
#endif
}

void CudaExecutor::free_unified_memory(void* ptr) {
    if (!is_available_ || ptr == nullptr || !loader_->is_available()) {
        return;
    }

#ifdef KAIRO_ENABLE_CUDA
    if (!ensure_device_context()) {
        return;
    }
    auto funcs = loader_->get_functions();
    if (funcs.cudaFree == nullptr) {
        set_last_error("CUDA cudaFree symbol is unavailable");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(memory_mutex_);
        auto it = allocated_memory_.find(ptr);
        if (it == allocated_memory_.end() || it->second.kind != AllocationKind::Owned) {
            return;
        }
        allocated_memory_.erase(it);
    }

    cudaError_t error = funcs.cudaFree(ptr);
    check_cuda_error(error, "cudaFree");
#else
    (void)ptr;
#endif
}

bool CudaExecutor::prefetch_memory(const void* ptr, size_t size, int device_id, int stream_id) {
    if (!is_available_ || !is_running_.load() || ptr == nullptr) {
        return false;
    }

    if (!config_.enable_unified_memory) {
        set_last_error("CUDA unified memory is not enabled");
        return false;  // 未启用统一内存
    }

    if (!validate_memory_range(ptr, size, "prefetch")) {
        return false;
    }

#ifdef KAIRO_ENABLE_CUDA
    if (!loader_->is_available()) {
        set_last_error("CUDA loader is unavailable");
        return false;
    }
    if (!ensure_device_context()) {
        return false;
    }
    auto funcs = loader_->get_functions();
    if (!funcs.is_unified_memory_available() || funcs.cudaMemPrefetchAsync == nullptr) {
        set_last_error("CUDA memory prefetch symbols are unavailable");
        return false;  // 不支持内存预取
    }

    if (stream_id == 0) {
        cudaError_t error = funcs.cudaMemPrefetchAsync(ptr, size, device_id, get_default_stream());
        return check_cuda_error(error, "cudaMemPrefetchAsync");
    }

    auto stream_wrapper = get_stream(stream_id);
    if (!stream_wrapper) {
        set_last_error(invalid_stream_message(stream_id));
        return false;  // 无效 stream_id
    }
    return call_stream(stream_wrapper, "cudaMemPrefetchAsync",
        [&](cudaStream_t stream) {
            return funcs.cudaMemPrefetchAsync(ptr, size, device_id, stream);
        });
#else
    (void)ptr;
    (void)size;
    (void)device_id;
    (void)stream_id;
    return false;
#endif
}

bool CudaExecutor::copy_to_device(void* dst, const void* src, size_t size, bool async, int stream_id) {
    if (!is_available_ || !is_running_.load() || dst == nullptr || src == nullptr) {
        return false;
    }

    if (!validate_memory_range(dst, size, "destination")) {
        return false;
    }

#ifdef KAIRO_ENABLE_CUDA
    if (!loader_->is_available()) {
        set_last_error("CUDA loader is unavailable");
        return false;
    }
    if (!ensure_device_context()) {
        return false;
    }
    auto funcs = loader_->get_functions();
    cudaError_t error;
    std::shared_ptr<StreamWrapper> stream_wrapper;
    if (stream_id != 0) {
        stream_wrapper = get_stream(stream_id);
        if (!stream_wrapper || !validate_stream(stream_wrapper)) {
            set_last_error(invalid_stream_message(stream_id));
            return false;  // 无效或已销毁 stream_id
        }
    }
    if (async) {
        if (funcs.cudaMemcpyAsync == nullptr) {
            set_last_error("CUDA cudaMemcpyAsync symbol is unavailable");
            return false;
        }
        if (stream_id == 0) {
            error = funcs.cudaMemcpyAsync(dst, src, size, cudaMemcpyHostToDevice, get_default_stream());
            return check_cuda_error(error, "cudaMemcpyAsync (H2D)");
        }
        return call_stream(stream_wrapper, "cudaMemcpyAsync (H2D)",
            [&](cudaStream_t stream) {
                return funcs.cudaMemcpyAsync(dst, src, size, cudaMemcpyHostToDevice, stream);
            });
    } else {
        if (funcs.cudaMemcpy == nullptr) {
            set_last_error("CUDA cudaMemcpy symbol is unavailable");
            return false;
        }
        error = funcs.cudaMemcpy(dst, src, size, cudaMemcpyHostToDevice);
    }
    return check_cuda_error(error, "cudaMemcpy (H2D)");
#else
    (void)dst;
    (void)src;
    (void)size;
    (void)async;
    (void)stream_id;
    return false;
#endif
}

bool CudaExecutor::copy_to_host(void* dst, const void* src, size_t size, bool async, int stream_id) {
    if (!is_available_ || !is_running_.load() || dst == nullptr || src == nullptr) {
        return false;
    }

    if (!validate_memory_range(src, size, "source")) {
        return false;
    }

#ifdef KAIRO_ENABLE_CUDA
    if (!loader_->is_available()) {
        set_last_error("CUDA loader is unavailable");
        return false;
    }
    if (!ensure_device_context()) {
        return false;
    }
    auto funcs = loader_->get_functions();
    cudaError_t error;
    std::shared_ptr<StreamWrapper> stream_wrapper;
    if (stream_id != 0) {
        stream_wrapper = get_stream(stream_id);
        if (!stream_wrapper || !validate_stream(stream_wrapper)) {
            set_last_error(invalid_stream_message(stream_id));
            return false;  // 无效或已销毁 stream_id
        }
    }
    if (async) {
        if (funcs.cudaMemcpyAsync == nullptr) {
            set_last_error("CUDA cudaMemcpyAsync symbol is unavailable");
            return false;
        }
        if (stream_id == 0) {
            error = funcs.cudaMemcpyAsync(dst, src, size, cudaMemcpyDeviceToHost, get_default_stream());
            return check_cuda_error(error, "cudaMemcpyAsync (D2H)");
        }
        return call_stream(stream_wrapper, "cudaMemcpyAsync (D2H)",
            [&](cudaStream_t stream) {
                return funcs.cudaMemcpyAsync(dst, src, size, cudaMemcpyDeviceToHost, stream);
            });
    } else {
        if (funcs.cudaMemcpy == nullptr) {
            set_last_error("CUDA cudaMemcpy symbol is unavailable");
            return false;
        }
        error = funcs.cudaMemcpy(dst, src, size, cudaMemcpyDeviceToHost);
    }
    return check_cuda_error(error, "cudaMemcpy (D2H)");
#else
    (void)dst;
    (void)src;
    (void)size;
    (void)async;
    (void)stream_id;
    return false;
#endif
}

bool CudaExecutor::copy_device_to_device(void* dst, const void* src, size_t size, bool async, int stream_id) {
    if (!is_available_ || !is_running_.load() || dst == nullptr || src == nullptr) {
        return false;
    }

    if (!validate_memory_range(dst, size, "destination") ||
        !validate_memory_range(src, size, "source")) {
        return false;
    }

#ifdef KAIRO_ENABLE_CUDA
    if (!loader_->is_available()) {
        set_last_error("CUDA loader is unavailable");
        return false;
    }
    if (!ensure_device_context()) {
        return false;
    }
    auto funcs = loader_->get_functions();
    cudaError_t error;
    std::shared_ptr<StreamWrapper> stream_wrapper;
    if (stream_id != 0) {
        stream_wrapper = get_stream(stream_id);
        if (!stream_wrapper || !validate_stream(stream_wrapper)) {
            set_last_error(invalid_stream_message(stream_id));
            return false;  // 无效或已销毁 stream_id
        }
    }
    if (async) {
        if (funcs.cudaMemcpyAsync == nullptr) {
            set_last_error("CUDA cudaMemcpyAsync symbol is unavailable");
            return false;
        }
        if (stream_id == 0) {
            error = funcs.cudaMemcpyAsync(dst, src, size, cudaMemcpyDeviceToDevice, get_default_stream());
            return check_cuda_error(error, "cudaMemcpyAsync (D2D)");
        }
        return call_stream(stream_wrapper, "cudaMemcpyAsync (D2D)",
            [&](cudaStream_t stream) {
                return funcs.cudaMemcpyAsync(dst, src, size, cudaMemcpyDeviceToDevice, stream);
            });
    } else {
        if (funcs.cudaMemcpy == nullptr) {
            set_last_error("CUDA cudaMemcpy symbol is unavailable");
            return false;
        }
        error = funcs.cudaMemcpy(dst, src, size, cudaMemcpyDeviceToDevice);
    }
    return check_cuda_error(error, "cudaMemcpy (D2D)");
#else
    (void)dst;
    (void)src;
    (void)size;
    (void)async;
    (void)stream_id;
    return false;
#endif
}

bool CudaExecutor::copy_from_peer(IGpuExecutor* src_executor, const void* src_ptr, void* dst_ptr,
                                  size_t size, bool async, int stream_id) {
    if (!is_available_ || !is_running_.load() ||
        src_executor == nullptr || src_executor == this || src_ptr == nullptr || dst_ptr == nullptr) {
        return false;
    }

    if (!validate_memory_range(dst_ptr, size, "P2P destination")) {
        return false;
    }
    auto* cuda_source = dynamic_cast<CudaExecutor*>(src_executor);
    if (cuda_source == nullptr || !cuda_source->validate_memory_range(src_ptr, size, "P2P source")) {
        if (cuda_source == nullptr) {
            set_last_error("CUDA P2P source memory was not allocated by a CUDA executor");
        }
        return false;
    }

    const int dst_device = device_id_;
    const int src_device = src_executor->get_device_info().device_id;
    if (src_device == dst_device) {
        set_last_error("P2P copy_from_peer failed: source and destination devices are the same");
        return false;  /* 同设备请使用 copy_device_to_device */
    }

#ifdef KAIRO_ENABLE_CUDA
    if (!loader_->is_available()) {
        set_last_error("CUDA loader is unavailable");
        return false;
    }
    auto funcs = loader_->get_functions();
    auto p2p_log = [this, &funcs](const char* step, bool use_cuda_err) {
        if (funcs.cudaGetLastError && funcs.cudaGetErrorString) {
            cudaError_t e = funcs.cudaGetLastError();
            std::string message = std::string("P2P copy_from_peer failed at ") + step + ": ";
            message += use_cuda_err ? funcs.cudaGetErrorString(e) : "(see above)";
            if (get_last_error().empty()) {
                set_last_error(message);
            }
            std::fprintf(stderr, "P2P copy_from_peer failed at %s: %s\n",
                step, use_cuda_err ? funcs.cudaGetErrorString(e) : "(see above)");
        } else {
            if (get_last_error().empty()) {
                set_last_error(std::string("P2P copy_from_peer failed at ") + step);
            }
        }
    };

    if (!funcs.is_p2p_available()) {
        const std::string message =
            "P2P copy_from_peer failed: P2P symbols not loaded (cudaMemcpyPeer etc.)";
        set_last_error(message);
        std::fprintf(stderr, "%s\n", message.c_str());
        return false;
    }

    int can = 0;
    cudaError_t err = funcs.cudaDeviceCanAccessPeer(&can, dst_device, src_device);
    if (!check_cuda_error(err, "cudaDeviceCanAccessPeer")) {
        p2p_log("cudaDeviceCanAccessPeer", true);
        return false;
    }
    if (can != 1) {
        std::ostringstream oss;
        oss << "P2P copy_from_peer failed: device " << dst_device
            << " cannot access peer " << src_device << " (CanAccessPeer=0)";
        set_last_error(oss.str());
        std::fprintf(stderr, "P2P copy_from_peer failed: device %d cannot access peer %d (CanAccessPeer=0)\n",
            dst_device, src_device);
        return false;
    }

    if (!ensure_device_context()) {
        p2p_log("ensure_device_context", true);
        return false;
    }

    err = funcs.cudaDeviceEnablePeerAccess(src_device, 0);
    if (err != cudaSuccess && err != cudaErrorPeerAccessAlreadyEnabled) {
        check_cuda_error(err, "cudaDeviceEnablePeerAccess");
        p2p_log("cudaDeviceEnablePeerAccess", true);
        return false;
    }

    if (async) {
        if (funcs.cudaMemcpyPeerAsync == nullptr) {
            const std::string message =
                "P2P copy_from_peer failed: cudaMemcpyPeerAsync not available";
            set_last_error(message);
            std::fprintf(stderr, "%s\n", message.c_str());
            return false;
        }
        if (stream_id == 0) {
            err = funcs.cudaMemcpyPeerAsync(dst_ptr, dst_device, src_ptr, src_device, size, get_default_stream());
        } else {
            auto stream_wrapper = get_stream(stream_id);
            if (!stream_wrapper) {
                set_last_error(invalid_stream_message(stream_id));
                std::fprintf(stderr, "P2P copy_from_peer failed: invalid stream_id %d\n", stream_id);
                return false;
            }
            bool submitted = call_stream(stream_wrapper, "cudaMemcpyPeerAsync",
                [&](cudaStream_t stream) {
                    return funcs.cudaMemcpyPeerAsync(dst_ptr, dst_device, src_ptr, src_device, size, stream);
                });
            if (!submitted) {
                set_last_error(invalid_stream_message(stream_id));
                std::fprintf(stderr, "P2P copy_from_peer failed: stream_id %d is invalid or destroyed\n", stream_id);
                return false;
            }
            return true;
        }
    } else {
        if (stream_id != 0) {
            auto stream_wrapper = get_stream(stream_id);
            if (!stream_wrapper || !validate_stream(stream_wrapper)) {
                set_last_error(invalid_stream_message(stream_id));
                std::fprintf(stderr, "P2P copy_from_peer failed: invalid stream_id %d\n", stream_id);
                return false;
            }
        }
        if (funcs.cudaMemcpyPeer == nullptr) {
            const std::string message =
                "P2P copy_from_peer failed: cudaMemcpyPeer not available";
            set_last_error(message);
            std::fprintf(stderr, "%s\n", message.c_str());
            return false;
        }
        err = funcs.cudaMemcpyPeer(dst_ptr, dst_device, src_ptr, src_device, size);
    }
    if (!check_cuda_error(err, "cudaMemcpyPeer")) {
        p2p_log("cudaMemcpyPeer", true);
        return false;
    }
    return true;
#else
    (void)size;
    (void)async;
    (void)stream_id;
    return false;
#endif
}

bool CudaExecutor::add_stream_callback(int stream_id, std::function<void()> callback) {
    if (!is_available_ || !is_running_.load() || !callback || !loader_->is_available()) {
        return false;
    }

#ifdef KAIRO_ENABLE_CUDA
    if (!ensure_device_context()) {
        return false;
    }
    auto funcs = loader_->get_functions();
    if (funcs.cudaLaunchHostFunc == nullptr) {
        set_last_error("CUDA cudaLaunchHostFunc symbol is unavailable");
        return false;
    }
    std::unique_ptr<StreamCallbackContext> ctx(
        new (std::nothrow) StreamCallbackContext{std::move(callback)});
    if (ctx == nullptr) {
        return false;
    }

    cudaError_t error;
    bool submitted = false;
    if (stream_id == 0) {
        error = funcs.cudaLaunchHostFunc(get_default_stream(), &stream_host_callback, ctx.get());
        submitted = true;
    } else {
        auto stream_wrapper = get_stream(stream_id);
        if (stream_wrapper) {
            submitted = call_stream(stream_wrapper, "cudaLaunchHostFunc",
                [&](cudaStream_t stream) {
                    error = funcs.cudaLaunchHostFunc(stream, &stream_host_callback, ctx.get());
                    return error;
                });
        }
    }
    if (!submitted) {
        set_last_error(invalid_stream_message(stream_id));
        return false;
    }
    if (!check_cuda_error(error, "cudaLaunchHostFunc")) {
        return false;
    }
    ctx.release();
    return true;
#else
    (void)stream_id;
    (void)callback;
    return false;
#endif
}

bool CudaExecutor::supports_stream_callback() const noexcept {
    return true;
}

void CudaExecutor::synchronize() {
    if (!is_available_ || !loader_->is_available()) {
        return;
    }

#ifdef KAIRO_ENABLE_CUDA
    if (!ensure_device_context()) {
        return;
    }
    auto funcs = loader_->get_functions();
    if (funcs.cudaDeviceSynchronize != nullptr) {
        cudaError_t error = funcs.cudaDeviceSynchronize();
        check_cuda_error(error, "cudaDeviceSynchronize");
    }
#endif
}

void CudaExecutor::synchronize_stream(int stream_id) {
    if (!is_available_ || !loader_->is_available()) {
        return;
    }

#ifdef KAIRO_ENABLE_CUDA
    if (!ensure_device_context()) {
        return;
    }
    auto funcs = loader_->get_functions();
    if (funcs.cudaStreamSynchronize == nullptr) {
        set_last_error("CUDA cudaStreamSynchronize symbol is unavailable");
        return;
    }

    if (stream_id == 0) {
        // 默认流
        synchronize();
        return;
    }

    auto stream_wrapper = get_stream(stream_id);
    if (stream_wrapper) {
        (void)call_stream(stream_wrapper, "cudaStreamSynchronize",
            [&](cudaStream_t stream) {
                return funcs.cudaStreamSynchronize(stream);
            });
    } else {
        set_last_error(invalid_stream_message(stream_id));
    }
#else
    (void)stream_id;
#endif
}

int CudaExecutor::create_stream() {
    if (!is_available_ || !loader_->is_available()) {
        return -1;
    }

#ifdef KAIRO_ENABLE_CUDA
    cudaStream_t stream = create_one_stream();
    if (stream == nullptr) {
        if (get_last_error().empty()) {
            set_last_error("CUDA stream creation failed");
        }
        return -1;
    }
    auto stream_wrapper = std::make_shared<StreamWrapper>();
    stream_wrapper->stream = stream;

    std::lock_guard<std::mutex> lock(streams_mutex_);
    for (size_t i = 0; i < streams_.size(); ++i) {
        if (!streams_[i]) {
            streams_[i] = std::move(stream_wrapper);
            return static_cast<int>(i + 1);
        }
    }
    streams_.push_back(std::move(stream_wrapper));
    // 返回流ID（从1开始，0是默认流）
    return static_cast<int>(streams_.size());
#else
    return -1;
#endif
}

void CudaExecutor::destroy_stream(int stream_id) {
    if (!is_available_ || stream_id <= 0 || !loader_->is_available()) {
        return;
    }

#ifdef KAIRO_ENABLE_CUDA
    if (!ensure_device_context()) {
        return;
    }

    std::shared_ptr<StreamWrapper> stream_wrapper;
    {
        std::lock_guard<std::mutex> lock(streams_mutex_);
        if (static_cast<size_t>(stream_id) <= streams_.size()) {
            const auto stream_index = static_cast<size_t>(stream_id - 1);
            stream_wrapper = std::move(streams_[stream_index]);
            streams_[stream_index].reset();
        }
    }

    destroy_stream_wrapper(stream_wrapper);
#else
    (void)stream_id;
#endif
}

std::string CudaExecutor::get_name() const {
    return name_;
}

GpuDeviceInfo CudaExecutor::get_device_info() const {
    GpuDeviceInfo info;
    info.name = name_;
    info.backend = GpuBackend::CUDA;
    info.device_id = device_id_;

    if (!is_available_) {
        return info;
    }

#ifdef KAIRO_ENABLE_CUDA
    info.name = device_prop_.name;
    info.compute_capability_major = device_prop_.major;
    info.compute_capability_minor = device_prop_.minor;
    info.max_threads_per_block = static_cast<size_t>(device_prop_.maxThreadsPerBlock);
    info.max_blocks_per_grid[0] = static_cast<size_t>(device_prop_.maxGridSize[0]);
    info.max_blocks_per_grid[1] = static_cast<size_t>(device_prop_.maxGridSize[1]);
    info.max_blocks_per_grid[2] = static_cast<size_t>(device_prop_.maxGridSize[2]);

    // 获取内存信息
    if (loader_->is_available() && ensure_device_context()) {
        auto funcs = loader_->get_functions();
        if (funcs.cudaMemGetInfo != nullptr) {
            size_t free_mem = 0;
            size_t total_mem = 0;
            cudaError_t error = funcs.cudaMemGetInfo(&free_mem, &total_mem);
            if (error == cudaSuccess) {
                info.free_memory_bytes = free_mem;
                info.total_memory_bytes = total_mem;
            }
        }
    }
#endif

    return info;
}

GpuExecutorStatus CudaExecutor::get_status() const {
    GpuExecutorStatus status;
    status.name = name_;
    status.is_running = is_running_.load();
    status.backend = GpuBackend::CUDA;
    status.device_id = device_id_;
    status.last_error_message = get_last_error();
    status.active_kernels = active_kernels_.load();
    status.completed_kernels = completed_kernels_.load();
    status.failed_kernels = failed_kernels_.load();
    status.queue_capacity = config_.max_queue_size;
    status.last_error_message = get_last_error();
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        status.queue_size = task_queue_.size();
    }

    if (!is_available_) {
        return status;
    }

#ifdef KAIRO_ENABLE_CUDA
    if (loader_->is_available() && ensure_device_context()) {
        auto funcs = loader_->get_functions();
        
        // 获取内存使用情况
        if (funcs.cudaMemGetInfo != nullptr) {
            size_t free_mem = 0;
            size_t total_mem = 0;
            cudaError_t error = funcs.cudaMemGetInfo(&free_mem, &total_mem);
            if (error == cudaSuccess) {
                status.memory_total_bytes = total_mem;
                
                // 计算已使用内存。启用内存池时 allocated_memory_ 不记录池分配。
                if (memory_manager_) {
                    auto memory_stats = memory_manager_->get_stats();
                    status.memory_used_bytes = memory_stats.total_allocated;
                } else {
                    std::lock_guard<std::mutex> lock(memory_mutex_);
                    size_t allocated = 0;
                    for (const auto& [ptr, allocation] : allocated_memory_) {
                        allocated += allocation.size;
                    }
                    status.memory_used_bytes = allocated;
                }

                if (total_mem > 0) {
                    status.memory_usage_percent = 
                        (static_cast<double>(status.memory_used_bytes)
                            / static_cast<double>(total_mem)) * 100.0;
                }
            }
        }

        // 计算平均kernel执行时间
        size_t completed = completed_kernels_.load();
        if (completed > 0) {
            int64_t total_time = total_kernel_time_ns_.load();
            status.avg_kernel_time_ms = (static_cast<double>(total_time)
                / static_cast<double>(completed)) / 1000000.0;
        }
    }
#endif

    return status;
}

void CudaExecutor::worker_thread_func() {
    {
        std::lock_guard<std::mutex> stop_lock(stop_mutex_);
        worker_id_ = std::this_thread::get_id();
    }

#ifdef KAIRO_ENABLE_CUDA
    while (true) {
        GpuQueuedTask task;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            while (task_queue_.empty() && is_running_.load()) {
                queue_not_empty_cv_.wait(lock);
            }
            if (!is_running_.load() && task_queue_.empty()) {
                break;
            }
            if (task_queue_.empty()) {
                continue;
            }
            task = task_queue_.top();
            task_queue_.pop();
            active_kernels_++;
            queue_not_full_cv_.notify_one();
        }
        run_one_task(task);
        active_kernels_--;
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (task_queue_.empty() && active_kernels_.load() == 0) {
                queue_drained_cv_.notify_all();
            }
        }
    }
#else
    (void)0;
#endif
}

void CudaExecutor::run_one_task(GpuQueuedTask& task) {
#ifdef KAIRO_ENABLE_CUDA
    auto start_time = std::chrono::high_resolution_clock::now();
    void* stream_ptr = nullptr;
    try {
        if (!ensure_device_context()) {
            set_last_error("CudaExecutor: ensure_device_context failed");
            auto eptr = std::make_exception_ptr(
                std::runtime_error("CudaExecutor: ensure_device_context failed"));
            exception_handler_.handle_task_exception(name_, eptr);
            if (task.promise) task.promise->set_exception(eptr);
            failed_kernels_++;
            return;
        }

        auto funcs = loader_->get_functions();
        auto execute_kernel = [&](cudaStream_t stream) {
            stream_ptr = static_cast<void*>(stream);
            task.kernel_func(stream_ptr);
            if (funcs.cudaGetLastError != nullptr) {
                cudaError_t err = funcs.cudaGetLastError();
                if (err != cudaSuccess) {
                    check_cuda_error(err, "cudaGetLastError (after kernel)");
                    std::exception_ptr eptr = make_cuda_exception_ptr(err, "cudaGetLastError (after kernel)");
                    exception_handler_.handle_task_exception(name_, eptr);
                    if (task.promise) task.promise->set_exception(eptr);
                    failed_kernels_++;
                    return false;
                }
            }
            if (!task.config.async && funcs.cudaStreamSynchronize != nullptr && stream != nullptr) {
                cudaError_t err = funcs.cudaStreamSynchronize(stream);
                if (!check_cuda_error(err, "cudaStreamSynchronize")) {
                    std::exception_ptr eptr = make_cuda_exception_ptr(err, "cudaStreamSynchronize");
                    exception_handler_.handle_task_exception(name_, eptr);
                    if (task.promise) task.promise->set_exception(eptr);
                    failed_kernels_++;
                    return false;
                }
            } else if (!task.config.async && stream == nullptr) {
                synchronize();
            }
            return true;
        };

        if (task.config.stream_id == 0) {
            if (!execute_kernel(get_default_stream())) {
                return;
            }
        } else {
            if (!task.stream) {
                auto eptr = make_invalid_stream_exception_ptr(task.config.stream_id);
                exception_handler_.handle_task_exception(name_, eptr);
                if (task.promise) task.promise->set_exception(eptr);
                failed_kernels_++;
                return;
            }
            if (task.stream->destroyed.load(std::memory_order_acquire)) {
                auto eptr = make_invalid_stream_exception_ptr(task.config.stream_id);
                exception_handler_.handle_task_exception(name_, eptr);
                if (task.promise) task.promise->set_exception(eptr);
                failed_kernels_++;
                return;
            }
            std::lock_guard<std::mutex> stream_lock(task.stream->mutex);
            if (task.stream->destroyed.load(std::memory_order_acquire) || task.stream->stream == nullptr) {
                auto eptr = make_invalid_stream_exception_ptr(task.config.stream_id);
                exception_handler_.handle_task_exception(name_, eptr);
                if (task.promise) task.promise->set_exception(eptr);
                failed_kernels_++;
                return;
            }
            if (!execute_kernel(task.stream->stream)) {
                return;
            }
        }
        if (task.promise) task.promise->set_value();
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
            end_time - start_time).count();
        total_kernel_time_ns_ += duration;
        completed_kernels_++;
    } catch (...) {
        set_last_error("CudaExecutor: kernel execution threw an exception");
        exception_handler_.handle_task_exception(name_, std::current_exception());
        if (task.promise) task.promise->set_exception(std::current_exception());
        failed_kernels_++;
    }
#else
    (void)task;
#endif
}

std::future<void> CudaExecutor::submit_kernel_impl(
    std::function<void(void*)> kernel_func,
    const GpuTaskConfig& config) {
    
    auto promise = std::make_shared<std::promise<void>>();
    auto future = promise->get_future();

    if (!validate_gpu_config(config_)) {
        const auto message = gpu_config_validation_error(config_);
        set_last_error(message);
        promise->set_exception(std::make_exception_ptr(
            std::runtime_error("CudaExecutor invalid configuration: " + message)));
        return future;
    }

    if (!is_available_ || !is_running_.load()) {
        set_last_error("CudaExecutor is not available or not running");
        promise->set_exception(std::make_exception_ptr(
            std::runtime_error("CudaExecutor is not available or not running")));
        return future;
    }

#ifdef KAIRO_ENABLE_CUDA
    GpuQueuedTask task;
    task.kernel_func = std::move(kernel_func);
    task.config = config;
    if (task.config.priority < 0) task.config.priority = 0;
    if (task.config.priority > 3) task.config.priority = 3;
    task.promise = promise;
    task.submit_time_ns = std::chrono::steady_clock::now().time_since_epoch().count();
    if (task.config.stream_id != 0) {
        task.stream = get_stream(task.config.stream_id);
        if (!task.stream || !validate_stream(task.stream)) {
            promise->set_exception(make_invalid_stream_exception_ptr(task.config.stream_id));
            return future;
        }
    }
    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        // P-260618-005: also exit the wait when the executor is no longer
        // running, otherwise a stop() arriving while submitter is parked
        // here can deadlock (stop() only notifies queue_not_empty_cv_).
        queue_not_full_cv_.wait(lock, [this] {
            return !is_running_.load(std::memory_order_acquire)
                || task_queue_.size() < config_.max_queue_size;
        });
        if (!is_running_.load(std::memory_order_acquire)) {
            // Shutdown happened while we were waiting for room. Return a
            // failed future without enqueuing.
            set_last_error("CudaExecutor: submit aborted because executor is stopping");
            promise->set_exception(std::make_exception_ptr(
                std::runtime_error("CudaExecutor: submit aborted because executor is stopping")));
            return future;
        }
        task_queue_.push(std::move(task));
    }
    queue_not_empty_cv_.notify_one();
    return future;
#else
    promise->set_exception(std::make_exception_ptr(
        std::runtime_error("CudaExecutor: CUDA not enabled")));
    return future;
#endif
}

std::vector<std::future<void>> CudaExecutor::submit_kernels_batch(
    const std::vector<std::pair<std::function<void(void*)>, GpuTaskConfig>>& tasks) {
    std::vector<std::future<void>> result;
    result.reserve(tasks.size());
    if (!validate_gpu_config(config_)) {
        const auto message = gpu_config_validation_error(config_);
        set_last_error(message);
        auto eptr = std::make_exception_ptr(
            std::runtime_error("CudaExecutor invalid configuration: " + message));
        for (size_t i = 0; i < tasks.size(); ++i) {
            (void)i;
            auto p = std::make_shared<std::promise<void>>();
            p->set_exception(eptr);
            result.push_back(p->get_future());
        }
        return result;
    }
    if (!is_available_ || !is_running_.load()) {
        set_last_error("CudaExecutor is not available or not running");
        auto eptr = std::make_exception_ptr(
            std::runtime_error("CudaExecutor is not available or not running"));
        for (size_t i = 0; i < tasks.size(); ++i) {
            (void)i;
            auto p = std::make_shared<std::promise<void>>();
            p->set_exception(eptr);
            result.push_back(p->get_future());
        }
        return result;
    }
#ifdef KAIRO_ENABLE_CUDA
    auto now_ns = std::chrono::steady_clock::now().time_since_epoch().count();
    const size_t chunk = 64u;
    for (size_t off = 0; off < tasks.size(); off += chunk) {
        size_t end = (std::min)(off + chunk, tasks.size());
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            for (size_t i = off; i < end; ++i) {
                // P-260618-005: see submit_kernel_impl. Break out of the
                // wait early when stop() arrives mid-batch; remaining
                // tasks in the batch become failed futures.
                queue_not_full_cv_.wait(lock, [this] {
                    return !is_running_.load(std::memory_order_acquire)
                        || task_queue_.size() < config_.max_queue_size;
                });
                if (!is_running_.load(std::memory_order_acquire)) {
                    set_last_error("CudaExecutor: submit aborted because executor is stopping");
                    auto eptr = std::make_exception_ptr(
                        std::runtime_error("CudaExecutor: submit aborted because executor is stopping"));
                    for (size_t j = i; j < tasks.size(); ++j) {
                        auto prom = std::make_shared<std::promise<void>>();
                        prom->set_exception(eptr);
                        result.push_back(prom->get_future());
                    }
                    return result;
                }
                const auto& p = tasks[i];
                GpuQueuedTask task;
                task.kernel_func = p.first;
                task.config = p.second;
                if (task.config.priority < 0) task.config.priority = 0;
                if (task.config.priority > 3) task.config.priority = 3;
                task.promise = std::make_shared<std::promise<void>>();
                result.push_back(task.promise->get_future());
                task.submit_time_ns = now_ns + static_cast<int64_t>(i);
                if (task.config.stream_id != 0) {
                    task.stream = get_stream(task.config.stream_id);
                    if (!task.stream || !validate_stream(task.stream)) {
                        task.promise->set_exception(
                            make_invalid_stream_exception_ptr(task.config.stream_id));
                        continue;
                    }
                }
                task_queue_.push(std::move(task));
            }
        }
        queue_not_empty_cv_.notify_one();
    }
    return result;
#else
    auto eptr = std::make_exception_ptr(
        std::runtime_error("CudaExecutor: CUDA not enabled"));
    for (size_t i = 0; i < tasks.size(); ++i) {
        (void)i;
        auto prom = std::make_shared<std::promise<void>>();
        prom->set_exception(eptr);
        result.push_back(prom->get_future());
    }
    return result;
#endif
}

#ifdef KAIRO_ENABLE_CUDA
cudaStream_t CudaExecutor::get_default_stream() const {
    return default_stream_;  // nullptr 表示默认流
}

std::shared_ptr<CudaExecutor::StreamWrapper> CudaExecutor::get_stream(int stream_id) const {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    if (stream_id < 1 || static_cast<size_t>(stream_id) > streams_.size()) {
        return nullptr;
    }
    return streams_[static_cast<size_t>(stream_id) - 1];
}

cudaStream_t CudaExecutor::create_one_stream() {
    if (!loader_->is_available()) {
        return nullptr;
    }
    if (!ensure_device_context()) {
        return nullptr;
    }
    auto funcs = loader_->get_functions();
    if (funcs.cudaStreamCreate == nullptr) {
        return nullptr;
    }
    cudaStream_t stream = nullptr;
    cudaError_t error = funcs.cudaStreamCreate(&stream);
    if (!check_cuda_error(error, "cudaStreamCreate") || stream == nullptr) {
        return nullptr;
    }
    return stream;
}

void CudaExecutor::destroy_stream_wrapper(const std::shared_ptr<StreamWrapper>& stream_wrapper) {
    if (!stream_wrapper || !loader_->is_available()) {
        return;
    }

    stream_wrapper->destroyed.store(true, std::memory_order_release);

    auto funcs = loader_->get_functions();
    std::lock_guard<std::mutex> lock(stream_wrapper->mutex);
    if (stream_wrapper->stream == nullptr) {
        return;
    }

    if (funcs.cudaStreamSynchronize != nullptr) {
        cudaError_t sync_error = funcs.cudaStreamSynchronize(stream_wrapper->stream);
        check_cuda_error(sync_error, "cudaStreamSynchronize");
    }
    if (funcs.cudaStreamDestroy != nullptr) {
        cudaError_t destroy_error = funcs.cudaStreamDestroy(stream_wrapper->stream);
        check_cuda_error(destroy_error, "cudaStreamDestroy");
    }
    stream_wrapper->stream = nullptr;
}

bool CudaExecutor::call_stream(const std::shared_ptr<StreamWrapper>& stream_wrapper,
                               const char* operation,
                               const std::function<cudaError_t(cudaStream_t)>& call) const {
    if (!stream_wrapper || stream_wrapper->destroyed.load(std::memory_order_acquire)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(stream_wrapper->mutex);
    if (stream_wrapper->destroyed.load(std::memory_order_acquire) || stream_wrapper->stream == nullptr) {
        return false;
    }

    cudaError_t error = call(stream_wrapper->stream);
    return check_cuda_error(error, operation);
}

bool CudaExecutor::validate_stream(const std::shared_ptr<StreamWrapper>& stream_wrapper) const {
    if (!stream_wrapper || stream_wrapper->destroyed.load(std::memory_order_acquire)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(stream_wrapper->mutex);
    return !stream_wrapper->destroyed.load(std::memory_order_acquire)
        && stream_wrapper->stream != nullptr;
}

std::exception_ptr CudaExecutor::make_invalid_stream_exception_ptr(int stream_id) const {
    const std::string message = invalid_stream_message(stream_id);
    set_last_error(message);
    return std::make_exception_ptr(InvalidStreamException(message));
}
#endif

} // namespace gpu
} // namespace kairo
