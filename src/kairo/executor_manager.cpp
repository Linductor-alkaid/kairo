#include "kairo/executor_manager.hpp"
#include "thread_pool_executor.hpp"
#include "realtime_thread_executor.hpp"
#include "blocking_io_executor.hpp"
#include "util/thread_utils.hpp"
#include "kairo/monitor/statistics_collector.hpp"
#include "kairo/interfaces.hpp"
#include "kairo/config.hpp"
#include <cstdlib>
#include <mutex>
#include <algorithm>
#include <thread>

#ifdef KAIRO_ENABLE_GPU
#include "kairo/gpu/cuda_executor.hpp"
#ifdef KAIRO_ENABLE_OPENCL
#include "kairo/gpu/opencl_executor.hpp"
#endif
#endif

namespace kairo {

// 静态成员变量定义
ExecutorManager* ExecutorManager::instance_ = nullptr;
std::once_flag ExecutorManager::once_flag_;

// 单例模式：获取单例实例
ExecutorManager& ExecutorManager::instance() {
    std::call_once(once_flag_, []() {
        instance_ = new ExecutorManager();
        std::atexit(&ExecutorManager::atexit_shutdown);
    });
    return *instance_;
}

// 退出时自动关闭：atexit 回调，仅单例创建时注册；不等待未完成任务，不抛异常
void ExecutorManager::atexit_shutdown() {
    try {
        ExecutorManager::instance().shutdown(false);
    } catch (...) {
        // 吞掉异常，避免 atexit 回调抛异常导致 std::terminate
    }
}

// 构造函数（实例化模式）
ExecutorManager::ExecutorManager()
    : default_async_executor_(nullptr)
    , statistics_collector_(std::make_unique<monitor::StatisticsCollector>()) {
    statistics_collector_->set_gpu_status_provider(
        [this]() { return get_all_gpu_executor_statuses(); });
}

// 析构函数（RAII）
ExecutorManager::~ExecutorManager() {
    // CR-014: 析构路径的排空不外泄异常（与下方 retired 终局排空同一约定）：
    // 后端 stop() 抛出时最多放弃完全排空，绝不让异常穿出析构函数触发
    // std::terminate。
    try {
        shutdown(true);  // 等待所有任务完成
    } catch (...) {
        // 析构不外泄异常。
    }
}

// 初始化默认异步执行器（线程池）
bool ExecutorManager::initialize_async_executor(const ExecutorConfig& config) {
    std::lock_guard<std::mutex> lock(default_async_mutex_);
    if (default_async_shutdown_) {
        return false;
    }
    // 检查是否已经初始化
    if (default_async_executor_ != nullptr) {
        return false;  // 已经初始化过
    }

    // 将 ExecutorConfig 转换为 ThreadPoolConfig
    ThreadPoolConfig pool_config;
    pool_config.queue_capacity = config.queue_capacity;
    pool_config.thread_priority = config.thread_priority;
    pool_config.cpu_affinity = config.cpu_affinity;
    pool_config.task_timeout_ms = config.task_timeout_ms;

    // 0 = 自适应 sentinel: 按 hw_concurrency 计算实际值
    if (config.min_threads == 0 || config.max_threads == 0) {
        unsigned hw = std::thread::hardware_concurrency();
#if defined(__ANDROID__)
        // Android App 实际可用的 cpuset 通常小于系统在线核数，默认线程池最多 4 个
        // worker，避免 8 核设备默认拉起 8 条常驻线程造成移动端能耗与调度压力。
        constexpr unsigned kAndroidDefaultMaxThreads = 4;
        const unsigned scheduling_hw =
            (hw == 0) ? 0u : std::min(hw, kAndroidDefaultMaxThreads);
#else
        const unsigned scheduling_hw = hw;
#endif
        if (hw == 0) {
            // 探测失败，退到安全默认
            pool_config.min_threads = (config.min_threads == 0) ? 2 : config.min_threads;
            pool_config.max_threads = (config.max_threads == 0) ? 4 : config.max_threads;
        } else {
            pool_config.min_threads = (config.min_threads == 0) ? std::max(2u, scheduling_hw / 4) : static_cast<unsigned>(config.min_threads);
            pool_config.max_threads = (config.max_threads == 0) ? scheduling_hw : static_cast<unsigned>(config.max_threads);
        }
        // 确保 min <= max
        if (pool_config.min_threads > pool_config.max_threads) {
            pool_config.min_threads = pool_config.max_threads;
        }
    } else {
        pool_config.min_threads = config.min_threads;
        pool_config.max_threads = config.max_threads;
    }

    // max_threads==1 时工作窃取无意义，自动关闭
    pool_config.enable_work_stealing = (pool_config.max_threads == 1) ? false : config.enable_work_stealing;

    // auto-allocate affinity to all detected cores when user didn't specify
    if (pool_config.cpu_affinity.empty() && pool_config.max_threads > 0) {
#if defined(__ANDROID__)
        // 不使用 0..hw-1 伪列表：Android App 只允许绑定到 cgroup cpuset 内的核。
        // 当前线程 affinity 返回允许掩码；为空或调用失败时保持 OS 自由调度。
        pool_config.cpu_affinity = util::get_current_thread_affinity();
#else
        unsigned hw = std::thread::hardware_concurrency();
        if (hw > 0) {
            pool_config.cpu_affinity.resize(hw);
            for (unsigned i = 0; i < hw; ++i) {
                pool_config.cpu_affinity[i] = static_cast<int>(i);
            }
        }
        // hw == 0: probe failed, leave empty → OS free-schedules
#endif
    }

    // 创建 ThreadPoolExecutor
    auto executor = std::make_shared<ThreadPoolExecutor>("default", pool_config);
    executor->set_task_monitor(&statistics_collector_->get_task_monitor());
    statistics_collector_->get_task_monitor().set_enabled(config.enable_monitoring);

    // 启动执行器
    if (!executor->start()) {
        return false;  // 启动失败
    }

    // 保存执行器
    default_async_executor_ = std::move(executor);
    bump_state_epoch();
    return true;
}

bool ExecutorManager::has_default_async_executor() const {
    std::lock_guard<std::mutex> lock(default_async_mutex_);
    return default_async_executor_ != nullptr;
}

bool ExecutorManager::is_default_async_shutdown() const {
    std::lock_guard<std::mutex> lock(default_async_mutex_);
    return default_async_shutdown_;
}

uint64_t ExecutorManager::get_state_epoch() const noexcept {
    return state_epoch_.load(std::memory_order_acquire);
}

void ExecutorManager::bump_state_epoch() noexcept {
    state_epoch_.fetch_add(1, std::memory_order_release);
}

// 获取默认异步执行器（线程池）
// 若尚未初始化，则使用默认配置懒初始化一次（线程安全由 std::call_once 保证）
// shutdown 后不再懒初始化，直接返回 nullptr
IAsyncExecutor* ExecutorManager::get_default_async_executor() {
    return get_default_async_executor_snapshot().get();
}

std::shared_ptr<IAsyncExecutor> ExecutorManager::get_default_async_executor_snapshot() {
    {
        std::lock_guard<std::mutex> lock(default_async_mutex_);
        if (default_async_shutdown_) {
            return nullptr;
        }
        if (default_async_executor_) {
            return default_async_executor_;
        }
    }

    // CR-013: 建池异常在 once 内消化并闩存——此前 initialize_async_executor
    // 抛出（线程创建 std::system_error）时 call_once 未完成，之后每次
    // 查询/提交都会重试整套建池并意外抛异常。
    std::call_once(default_init_once_, [this] {
        try {
            ExecutorConfig default_config{};
            initialize_async_executor(default_config);
        } catch (...) {
            std::lock_guard<std::mutex> lock(default_async_mutex_);
            default_init_failed_ = true;
        }
    });

    std::lock_guard<std::mutex> lock(default_async_mutex_);
    return default_async_executor_;
}

std::shared_ptr<IAsyncExecutor>
ExecutorManager::get_default_async_executor_snapshot_no_create() {
    std::lock_guard<std::mutex> lock(default_async_mutex_);
    if (default_async_shutdown_ || default_init_failed_) {
        return nullptr;
    }
    return default_async_executor_;
}

// 注册实时执行器
bool ExecutorManager::register_realtime_executor(const std::string& name,
                                                 std::unique_ptr<IRealtimeExecutor> executor) {
    if (name.empty() || name == "default" || executor == nullptr) {
        return false;
    }

    std::lock_guard<std::mutex> registration_lock(registration_mutex_);
    if (registries_shutdown_ || is_executor_name_registered_locked(name)) {
        return false;  // 名称已存在
    }
    std::unique_lock<std::shared_mutex> lock(mutex_);
    realtime_executors_[name] = std::shared_ptr<IRealtimeExecutor>(std::move(executor));
    bump_state_epoch();
    return true;
}

// 获取已注册的实时执行器
IRealtimeExecutor* ExecutorManager::get_realtime_executor(const std::string& name) {
    return get_realtime_executor_snapshot(name).get();
}

std::shared_ptr<IRealtimeExecutor> ExecutorManager::get_realtime_executor_snapshot(
    const std::string& name) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto it = realtime_executors_.find(name);
    return it == realtime_executors_.end() ? nullptr : it->second;
}

// 创建实时执行器（便捷方法）
std::unique_ptr<IRealtimeExecutor> ExecutorManager::create_realtime_executor(
    const std::string& name,
    const RealtimeThreadConfig& config) {
    if (name.empty()) {
        return nullptr;
    }

    try {
        // Facade users should get queue-full diagnostics without knowing the
        // lower-level constructor knobs.
        auto executor = std::make_unique<RealtimeThreadExecutor>(
            name, config, /*enable_stats=*/true);
        return executor;
    } catch (const std::exception&) {
        // 创建失败（如配置无效），返回 nullptr
        return nullptr;
    }
}

// 获取所有实时执行器名称
std::vector<std::string> ExecutorManager::get_realtime_executor_names() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    std::vector<std::string> names;
    names.reserve(realtime_executors_.size());
    
    for (const auto& pair : realtime_executors_) {
        names.push_back(pair.first);
    }
    
    return names;
}

std::map<std::string, RealtimeExecutorStatus>
ExecutorManager::get_all_realtime_executor_statuses() const {
    std::vector<std::pair<std::string, std::shared_ptr<IRealtimeExecutor>>> snapshots;
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        snapshots.reserve(realtime_executors_.size());
        for (const auto& [name, executor] : realtime_executors_) {
            snapshots.emplace_back(name, executor);
        }
    }

    std::map<std::string, RealtimeExecutorStatus> result;
    for (const auto& [name, executor] : snapshots) {
        if (executor) {
            result[name] = executor->get_status();
        }
    }
    return result;
}

bool ExecutorManager::register_lockfree_executor(
    const std::string& name,
    std::unique_ptr<LockFreeTaskExecutor> executor) {
    if (name.empty() || name == "default" || !executor) {
        return false;
    }
    std::lock_guard<std::mutex> registration_lock(registration_mutex_);
    if (registries_shutdown_ || is_executor_name_registered_locked(name)) {
        return false;
    }
    std::unique_lock<std::shared_mutex> lock(lockfree_mutex_);
    lockfree_executors_[name] = std::shared_ptr<LockFreeTaskExecutor>(std::move(executor));
    bump_state_epoch();
    return true;
}

LockFreeTaskExecutor* ExecutorManager::get_lockfree_executor(const std::string& name) {
    return get_lockfree_executor_snapshot(name).get();
}

std::shared_ptr<LockFreeTaskExecutor> ExecutorManager::get_lockfree_executor_snapshot(
    const std::string& name) const {
    std::shared_lock<std::shared_mutex> lock(lockfree_mutex_);
    const auto iterator = lockfree_executors_.find(name);
    return iterator == lockfree_executors_.end() ? nullptr : iterator->second;
}

std::vector<std::string> ExecutorManager::get_lockfree_executor_names() const {
    std::shared_lock<std::shared_mutex> lock(lockfree_mutex_);
    std::vector<std::string> names;
    names.reserve(lockfree_executors_.size());
    for (const auto& [name, executor] : lockfree_executors_) {
        (void)executor;
        names.push_back(name);
    }
    return names;
}

bool ExecutorManager::start_lockfree_executor(const std::string& name) {
    auto executor = get_lockfree_executor_snapshot(name);
    const bool started = executor && executor->start();
    if (started) bump_state_epoch();
    return started;
}

void ExecutorManager::stop_lockfree_executor(const std::string& name) {
    if (auto executor = get_lockfree_executor_snapshot(name)) {
        executor->stop_and_join();
        bump_state_epoch();
    }
}

bool ExecutorManager::try_push_lockfree_task(const std::string& name,
                                              std::function<void()> task) {
    auto executor = get_lockfree_executor_snapshot(name);
    return executor && executor->push_task(std::move(task));
}

bool ExecutorManager::register_blocking_io_executor(
    const std::string& name,
    std::unique_ptr<IBlockingIoExecutor> executor) {
    if (name.empty() || name == "default" || !executor) {
        return false;
    }

    std::lock_guard<std::mutex> registration_lock(registration_mutex_);
    if (registries_shutdown_ || is_executor_name_registered_locked(name)) {
        return false;
    }
    std::unique_lock<std::shared_mutex> lock(blocking_io_mutex_);
    blocking_io_executors_[name] = std::shared_ptr<IBlockingIoExecutor>(std::move(executor));
    bump_state_epoch();
    return true;
}

IBlockingIoExecutor* ExecutorManager::get_blocking_io_executor(const std::string& name) {
    return get_blocking_io_executor_snapshot(name).get();
}

std::shared_ptr<IBlockingIoExecutor> ExecutorManager::get_blocking_io_executor_snapshot(
    const std::string& name) const {
    std::shared_lock<std::shared_mutex> lock(blocking_io_mutex_);
    const auto it = blocking_io_executors_.find(name);
    return it == blocking_io_executors_.end() ? nullptr : it->second;
}

void ExecutorManager::request_stop_blocking_io_executor(const std::string& name) noexcept {
    try {
        if (auto executor = get_blocking_io_executor_snapshot(name)) {
            executor->request_stop();
            bump_state_epoch();
        }
    } catch (...) {
    }
}

void ExecutorManager::stop_blocking_io_executor(const std::string& name) {
    if (auto executor = get_blocking_io_executor_snapshot(name)) {
        executor->stop();
        bump_state_epoch();
    }
}

BlockingIoExecutorStatus ExecutorManager::get_blocking_io_executor_status(
    const std::string& name) const {
    if (auto executor = get_blocking_io_executor_snapshot(name)) return executor->get_status();
    BlockingIoExecutorStatus status;
    status.name = name;
    return status;
}

std::unique_ptr<IBlockingIoExecutor> ExecutorManager::create_blocking_io_executor(
    const std::string& name,
    const BlockingIoConfig& config,
    std::unique_ptr<IBlockingIoWorker> worker) {
    if (name.empty() || config.thread_name.empty() || !worker) {
        return nullptr;
    }
    try {
        return std::make_unique<BlockingIoExecutor>(name, config, std::move(worker));
    } catch (...) {
        return nullptr;
    }
}

std::vector<std::string> ExecutorManager::get_blocking_io_executor_names() const {
    std::shared_lock<std::shared_mutex> lock(blocking_io_mutex_);
    std::vector<std::string> names;
    names.reserve(blocking_io_executors_.size());
    for (const auto& pair : blocking_io_executors_) {
        names.push_back(pair.first);
    }
    return names;
}

std::map<std::string, BlockingIoExecutorStatus>
ExecutorManager::get_all_blocking_io_executor_statuses() const {
    std::vector<std::pair<std::string, std::shared_ptr<IBlockingIoExecutor>>> snapshots;
    {
        std::shared_lock<std::shared_mutex> lock(blocking_io_mutex_);
        snapshots.reserve(blocking_io_executors_.size());
        for (const auto& [name, executor] : blocking_io_executors_) {
            snapshots.emplace_back(name, executor);
        }
    }

    std::map<std::string, BlockingIoExecutorStatus> result;
    for (const auto& [name, executor] : snapshots) {
        if (executor) {
            result[name] = executor->get_status();
        }
    }
    return result;
}

// 注册 GPU 执行器
bool ExecutorManager::register_gpu_executor(const std::string& name,
                                             std::unique_ptr<IGpuExecutor> executor) {
    if (name.empty() || name == "default" || executor == nullptr) {
        return false;
    }

    std::lock_guard<std::mutex> registration_lock(registration_mutex_);
    if (registries_shutdown_ || is_executor_name_registered_locked(name)) {
        return false;  // 名称已存在
    }
    std::unique_lock<std::shared_mutex> lock(gpu_mutex_);
    gpu_executors_[name] = std::shared_ptr<IGpuExecutor>(std::move(executor));
    bump_state_epoch();
    return true;
}

// 获取已注册的 GPU 执行器
IGpuExecutor* ExecutorManager::get_gpu_executor(const std::string& name) {
    return get_gpu_executor_snapshot(name).get();
}

std::shared_ptr<IGpuExecutor> ExecutorManager::get_gpu_executor_snapshot(
    const std::string& name) const {
    std::shared_lock<std::shared_mutex> lock(gpu_mutex_);
    const auto it = gpu_executors_.find(name);
    return it == gpu_executors_.end() ? nullptr : it->second;
}

// 创建 GPU 执行器（便捷方法）
std::unique_ptr<IGpuExecutor> ExecutorManager::create_gpu_executor(
    const gpu::GpuExecutorConfig& config) {
#ifdef KAIRO_ENABLE_GPU
    // 验证配置
    if (!gpu::validate_gpu_config(config)) {
        return nullptr;
    }

    try {
        // 根据后端类型创建对应的执行器
        if (config.backend == gpu::GpuBackend::CUDA) {
#ifdef KAIRO_ENABLE_CUDA
            return std::make_unique<gpu::CudaExecutor>(config.name, config);
#else
            return nullptr;  // CUDA 支持未启用
#endif
        } else if (config.backend == gpu::GpuBackend::OPENCL) {
#ifdef KAIRO_ENABLE_OPENCL
            return std::make_unique<gpu::OpenCLExecutor>(config.name, config);
#else
            return nullptr;  // OpenCL 支持未启用
#endif
        }
        // 其他后端（SYCL）待后续实现
        return nullptr;
    } catch (const std::exception&) {
        // 创建失败（如配置无效），返回 nullptr
        return nullptr;
    }
#else
    // GPU 支持未启用
    return nullptr;
#endif
}

// 获取所有 GPU 执行器名称
std::vector<std::string> ExecutorManager::get_gpu_executor_names() const {
    std::shared_lock<std::shared_mutex> lock(gpu_mutex_);
    
    std::vector<std::string> names;
    names.reserve(gpu_executors_.size());
    
    for (const auto& pair : gpu_executors_) {
        names.push_back(pair.first);
    }
    
    return names;
}

// 获取所有 GPU 执行器状态
std::map<std::string, gpu::GpuExecutorStatus>
ExecutorManager::get_all_gpu_executor_statuses() const {
    std::vector<std::pair<std::string, std::shared_ptr<IGpuExecutor>>> snapshots;
    {
        std::shared_lock<std::shared_mutex> lock(gpu_mutex_);
        snapshots.reserve(gpu_executors_.size());
        for (const auto& [name, executor] : gpu_executors_) {
            snapshots.emplace_back(name, executor);
        }
    }

    std::map<std::string, gpu::GpuExecutorStatus> result;
    for (const auto& [name, executor] : snapshots) {
        if (executor) {
            result[name] = executor->get_status();
        }
    }
    return result;
}

bool ExecutorManager::is_executor_name_registered(const std::string& name) const {
    std::lock_guard<std::mutex> registration_lock(registration_mutex_);
    return is_executor_name_registered_locked(name);
}

bool ExecutorManager::is_executor_name_registered_locked(const std::string& name) const {
    std::shared_lock<std::shared_mutex> realtime_lock(mutex_);
    std::shared_lock<std::shared_mutex> lockfree_lock(lockfree_mutex_);
    std::shared_lock<std::shared_mutex> blocking_lock(blocking_io_mutex_);
    std::shared_lock<std::shared_mutex> gpu_lock(gpu_mutex_);
    return realtime_executors_.contains(name) || lockfree_executors_.contains(name) ||
           blocking_io_executors_.contains(name) || gpu_executors_.contains(name);
}

std::vector<ExecutorCapability> ExecutorManager::get_executor_capabilities() const {
    std::vector<ExecutorCapability> capabilities;

    {
        std::lock_guard<std::mutex> lock(default_async_mutex_);
        ExecutorCapability capability;
        capability.backend = ExecutionBackend::DefaultAsync;
        capability.name = default_async_executor_ ? default_async_executor_->get_name() : "default";
        capability.registered = default_async_executor_ != nullptr;
        capability.supports_future_submission = true;
        if (default_async_executor_) {
            const auto status = default_async_executor_->get_status();
            capability.running = status.is_running;
            capability.pending_work = status.active_tasks + status.queue_size;
        }
        capabilities.push_back(std::move(capability));
    }

    {
        std::shared_lock<std::shared_mutex> lock(gpu_mutex_);
        capabilities.reserve(capabilities.size() + gpu_executors_.size());
        for (const auto& [name, executor] : gpu_executors_) {
            ExecutorCapability capability;
            capability.backend = ExecutionBackend::Gpu;
            capability.name = name;
            capability.registered = executor != nullptr;
            capability.supports_future_submission = true;
            capability.supports_gpu_kernel = true;
            if (executor) {
                const auto status = executor->get_status();
                capability.running = status.is_running && status.last_error_message.empty();
                capability.pending_work = status.queue_size + status.active_kernels;
                capability.capacity_hint = status.queue_capacity;
            }
            capabilities.push_back(std::move(capability));
        }
    }

    {
        std::shared_lock<std::shared_mutex> lock(lockfree_mutex_);
        capabilities.reserve(capabilities.size() + lockfree_executors_.size());
        for (const auto& [name, executor] : lockfree_executors_) {
            ExecutorCapability capability;
            capability.backend = ExecutionBackend::LockFree;
            capability.name = name;
            capability.registered = executor != nullptr;
            capability.supports_bounded_dispatch = true;
            if (executor) {
                const auto status = executor->get_status_snapshot();
                capability.running = executor->is_running();
                capability.pending_work = status.current_size;
                capability.capacity_hint = status.queue_capacity;
            }
            capabilities.push_back(std::move(capability));
        }
    }

    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        capabilities.reserve(capabilities.size() + realtime_executors_.size());
        for (const auto& [name, executor] : realtime_executors_) {
            ExecutorCapability capability;
            capability.backend = ExecutionBackend::Realtime;
            capability.name = name;
            capability.registered = executor != nullptr;
            capability.supports_bounded_dispatch = true;
            if (executor) {
                const auto status = executor->get_status();
                capability.running = status.is_running;
                // Realtime status deliberately exposes only bounded-capacity
                // diagnostics, not an instantaneous queue-length snapshot.
                capability.pending_work = 0;
                capability.capacity_hint = status.queue_capacity;
            }
            capabilities.push_back(std::move(capability));
        }
    }

    {
        std::shared_lock<std::shared_mutex> lock(blocking_io_mutex_);
        capabilities.reserve(capabilities.size() + blocking_io_executors_.size());
        for (const auto& [name, executor] : blocking_io_executors_) {
            ExecutorCapability capability;
            capability.backend = ExecutionBackend::BlockingIo;
            capability.name = name;
            capability.registered = executor != nullptr;
            if (executor) {
                capability.running = executor->get_status().is_running;
            }
            capabilities.push_back(std::move(capability));
        }
    }
    return capabilities;
}

bool ExecutorManager::try_push_realtime_task(const std::string& name,
                                              std::function<void()> task) {
    auto executor = get_realtime_executor_snapshot(name);
    return executor && executor->push_task(std::move(task)).ok;
}

void ExecutorManager::enable_monitoring(bool enable) {
    if (statistics_collector_) {
        statistics_collector_->get_task_monitor().set_enabled(enable);
    }
}

void ExecutorManager::set_monitoring_sampling_rate(double rate) {
    if (statistics_collector_) {
        statistics_collector_->get_task_monitor().set_sampling_rate(rate);
    }
}

void ExecutorManager::set_in_flight_task_capacity(size_t capacity) {
    if (statistics_collector_) {
        statistics_collector_->get_task_monitor().set_in_flight_capacity(capacity);
    }
}

void ExecutorManager::set_in_flight_task_sampling_rate(double rate) {
    if (statistics_collector_) {
        statistics_collector_->get_task_monitor().set_in_flight_sampling_rate(rate);
    }
}

TaskStatistics ExecutorManager::get_task_statistics(
    const std::string& task_type) const {
    return statistics_collector_
           ? statistics_collector_->get_task_statistics(task_type)
           : TaskStatistics{};
}

std::map<std::string, TaskStatistics>
ExecutorManager::get_all_task_statistics() const {
    return statistics_collector_
           ? statistics_collector_->get_all_task_statistics()
           : std::map<std::string, TaskStatistics>{};
}

InFlightTaskDiagnostics ExecutorManager::get_in_flight_task_diagnostics() const {
    return statistics_collector_
           ? statistics_collector_->get_in_flight_task_diagnostics()
           : InFlightTaskDiagnostics{};
}

void ExecutorManager::record_in_flight_task_pending(const std::string& task_id,
                                                     const std::string& task_type,
                                                     const std::string& executor_name) {
    if (statistics_collector_) {
        try {
            statistics_collector_->get_task_monitor().record_task_pending(
                task_id, task_type, executor_name);
        } catch (...) {
        }
    }
}

void ExecutorManager::record_in_flight_task_state(const std::string& task_id,
                                                   TaskLifecycleState state) {
    if (statistics_collector_) {
        try {
            statistics_collector_->get_task_monitor().record_task_state(task_id, state);
        } catch (...) {
        }
    }
}

void ExecutorManager::record_in_flight_task_terminal(const std::string& task_id) {
    if (statistics_collector_) {
        try {
            statistics_collector_->get_task_monitor().record_task_terminal(task_id);
        } catch (...) {
        }
    }
}

// 关闭所有执行器
ShutdownResult ExecutorManager::shutdown(bool wait_for_tasks) {
    bool shutdown_requested_from_worker = false;
    std::vector<std::shared_ptr<LockFreeTaskExecutor>> lockfree_executors;
    std::vector<std::shared_ptr<IBlockingIoExecutor>> blocking_io_executors;
    std::vector<std::shared_ptr<IRealtimeExecutor>> realtime_executors;
    std::vector<std::shared_ptr<IGpuExecutor>> gpu_executors;

    // Seal every named registry before removing entries so a concurrent
    // registration cannot escape this shutdown pass.
    {
        std::lock_guard<std::mutex> registration_lock(registration_mutex_);
        registries_shutdown_ = true;

        {
            std::unique_lock<std::shared_mutex> lock(mutex_);
            realtime_executors.reserve(realtime_executors_.size());
            for (const auto& pair : realtime_executors_) {
                realtime_executors.push_back(pair.second);
            }
            realtime_executors_.clear();
        }
        {
            std::unique_lock<std::shared_mutex> lock(lockfree_mutex_);
            lockfree_executors.reserve(lockfree_executors_.size());
            for (const auto& [name, executor] : lockfree_executors_) {
                (void)name;
                lockfree_executors.push_back(executor);
            }
            lockfree_executors_.clear();
        }
        {
            std::unique_lock<std::shared_mutex> lock(blocking_io_mutex_);
            blocking_io_executors.reserve(blocking_io_executors_.size());
            for (const auto& pair : blocking_io_executors_) {
                blocking_io_executors.push_back(pair.second);
            }
            blocking_io_executors_.clear();
        }
        {
            std::unique_lock<std::shared_mutex> lock(gpu_mutex_);
            gpu_executors.reserve(gpu_executors_.size());
            for (const auto& pair : gpu_executors_) {
                gpu_executors.push_back(pair.second);
            }
            gpu_executors_.clear();
        }
        bump_state_epoch();
    }

    for (auto& executor : lockfree_executors) {
        if (executor) executor->stop_and_join();
    }
    for (auto& executor : blocking_io_executors) {
        if (executor) executor->request_stop();
    }
    for (auto& executor : blocking_io_executors) {
        if (executor) executor->stop();
    }
    for (const auto& executor : realtime_executors) {
        if (executor) executor->stop();
    }
    for (const auto& executor : gpu_executors) {
        if (executor) {
            executor->stop();
            if (wait_for_tasks) executor->wait_for_completion();
        }
    }
    
    // 停止异步执行器。
    //
    // P-260816-001: 排空（stop/wait_for_completion 内含 worker join，可阻塞数秒）
    // 绝不能在持有 default_async_mutex_ 时进行 —— submit()/snapshot/状态查询等
    // 读路径都要拿这把锁，池内任务在排空期间再入 facade 会与 shutdown 互相
    // 等待形成自死锁。这里只在锁内置 shutdown 闩并快照执行器，随后在锁外
    // 排空；并发 shutdown 调用者通过 default_async_shutdown_cv_ 等待排空完成，
    // 与旧实现（持锁阻塞到排空结束）的对外语义一致，但不再阻塞无关读路径。
    // 置闩后读路径立即走拒绝分支，与 ThreadPool 自身"先停止接收新任务，再
    // 等待已接受任务完成"的关停顺序对齐。
    std::shared_ptr<IAsyncExecutor> executor_to_stop;
    {
        std::unique_lock<std::mutex> lock(default_async_mutex_);
        default_async_shutdown_cv_.wait(
            lock, [this] { return !default_async_shutdown_in_progress_; });

        default_async_shutdown_ = true;
        executor_to_stop = default_async_executor_;
        if (executor_to_stop) {
            default_async_shutdown_in_progress_ = true;
            shutdown_requested_from_worker =
                executor_to_stop->is_current_worker_thread();
            if (!shutdown_requested_from_worker) {
                // 管理器侧引用撤下但不可直接丢弃：池可能已交由 detached
                // 终结线程排空（stop(false)），worker 闭包捕获裸 facade
                // 指针。退休执行器保存在 retired_async_executors_，由
                // wait_for_tasks 路径与本管理器析构负责等到 worker 全部
                // join 后再释放。
                retired_async_executors_.push_back(executor_to_stop);
                default_async_executor_.reset();
            }
            // worker 发起的 shutdown 保留成员，由外部调用者最终化。
        }
        bump_state_epoch();
    }

    if (executor_to_stop) {
        try {
            executor_to_stop->stop(wait_for_tasks);
            if (wait_for_tasks && !shutdown_requested_from_worker) {
                executor_to_stop->wait_for_completion();
            }
        } catch (...) {
            finish_default_async_shutdown_drain();
            throw;
        }
        finish_default_async_shutdown_drain();
    }

    // 退休执行器终局排空：仅 wait_for_tasks 路径执行（保持 stop(false)
    // 立即返回的契约）。此前的 shutdown(false) 可能把执行器送进
    // retired_async_executors_；本次 shutdown(true)（含 ~Executor 路径）
    // 必须等到这些池的 detached 终结线程 join 完全部 worker，才能允许
    // 调用方继续析构 facade/manager 状态——worker 闭包捕获裸 facade
    // 指针，收尾时还会触达 failure 面板 / monitor / registry。
    if (wait_for_tasks) {
        std::vector<std::shared_ptr<IAsyncExecutor>> retired;
        {
            std::unique_lock<std::mutex> lock(default_async_mutex_);
            retired.swap(retired_async_executors_);
        }
        for (auto& retired_executor : retired) {
            if (!retired_executor) {
                continue;
            }
            try {
                retired_executor->stop(true);
            } catch (...) {
                // 终局排空不外泄异常；池无法完全排空时最多提前退役，
                // 不能阻断 shutdown/析构。
            }
        }
    }
    return shutdown_requested_from_worker
               ? ShutdownResult::RequestedFromWorker
               : ShutdownResult::Completed;
}

void ExecutorManager::finish_default_async_shutdown_drain() {
    {
        std::lock_guard<std::mutex> lock(default_async_mutex_);
        default_async_shutdown_in_progress_ = false;
    }
    default_async_shutdown_cv_.notify_all();
}

} // namespace kairo
