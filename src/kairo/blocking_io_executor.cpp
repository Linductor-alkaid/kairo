#include "blocking_io_executor.hpp"

#include "util/thread_utils.hpp"

#include <exception>
#include <utility>

namespace kairo {

BlockingIoExecutor::BlockingIoExecutor(
    std::string name,
    BlockingIoConfig config,
    std::unique_ptr<IBlockingIoWorker> worker)
    : name_(std::move(name))
    , config_(std::move(config))
    , worker_(std::move(worker)) {}

BlockingIoExecutor::~BlockingIoExecutor() {
    stop();
}

bool BlockingIoExecutor::start() {
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (running_.load(std::memory_order_acquire) || thread_.joinable() || !worker_) {
            return false;
        }

        stop_requested_.store(false, std::memory_order_release);
        ready_.store(false, std::memory_order_release);
        cpu_affinity_applied_.store(false, std::memory_order_release);
        memory_locked_.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> startup_lock(startup_mutex_);
            startup_reported_ = false;
        }
        {
            std::lock_guard<std::mutex> status_lock(status_mutex_);
            stop_reason_ = BlockingIoStopReason::None;
            last_error_message_.clear();
        }

        running_.store(true, std::memory_order_release);
        try {
            thread_ = thread_factory_([this](StopToken stop_token) noexcept {
                run(stop_token);
            });
        } catch (...) {
            running_.store(false, std::memory_order_release);
            ready_.store(false, std::memory_order_release);
            set_error("Blocking I/O worker thread creation failed");
            std::lock_guard<std::mutex> status_lock(status_mutex_);
            stop_reason_ = BlockingIoStopReason::StartFailed;
            return false;
        }
    }

    if (config_.startup_timeout.count() == 0) {
        return true;
    }
    std::unique_lock<std::mutex> startup_lock(startup_mutex_);
    const bool ready = startup_cv_.wait_for(
        startup_lock, config_.startup_timeout, [this] { return startup_reported_; });
    startup_lock.unlock();
    if (ready) {
        return true;
    }

    stop();
    set_error("Blocking I/O worker startup timed out before ready");
    std::lock_guard<std::mutex> status_lock(status_mutex_);
    stop_reason_ = BlockingIoStopReason::StartFailed;
    return false;
}

void BlockingIoExecutor::request_stop() noexcept {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    request_stop_locked();
}

void BlockingIoExecutor::request_stop_locked() noexcept {
    if (!thread_.joinable()) {
        return;
    }
    if (stop_requested_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    thread_.request_stop();
    if (worker_) {
        try {
            worker_->wakeup();
            wakeup_count_.fetch_add(1, std::memory_order_relaxed);
        } catch (...) {
            set_error("Blocking I/O worker wakeup threw an exception");
        }
    }
}

void BlockingIoExecutor::stop() {
    detail::JThread joiner;
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        request_stop_locked();
        if (!thread_.joinable()) {
            running_.store(false, std::memory_order_release);
            return;
        }
        if (std::this_thread::get_id() == worker_id_) {
            return;
        }
        joiner = std::move(thread_);
    }

    joiner.join();
    running_.store(false, std::memory_order_release);
    ready_.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> status_lock(status_mutex_);
    if (stop_reason_ == BlockingIoStopReason::None) {
        stop_reason_ = BlockingIoStopReason::Requested;
    }
}

std::string BlockingIoExecutor::get_name() const {
    return name_;
}

BlockingIoExecutorStatus BlockingIoExecutor::get_status() const {
    BlockingIoExecutorStatus status;
    status.name = name_;
    status.is_running = running_.load(std::memory_order_acquire);
    status.stop_requested = stop_requested_.load(std::memory_order_acquire);
    status.ready = ready_.load(std::memory_order_acquire);
    status.cpu_affinity_applied = cpu_affinity_applied_.load(std::memory_order_acquire);
    status.memory_locked = memory_locked_.load(std::memory_order_acquire);
    status.wakeup_count = wakeup_count_.load(std::memory_order_acquire);
    std::lock_guard<std::mutex> lock(status_mutex_);
    status.stop_reason = stop_reason_;
    status.last_error_message = last_error_message_;
    return status;
}

void BlockingIoExecutor::run(StopToken stop_token) noexcept {
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        worker_id_ = std::this_thread::get_id();
    }
    util::set_current_thread_name(config_.thread_name);

    if (!config_.cpu_affinity.empty()) {
        cpu_affinity_applied_.store(
            util::set_current_thread_affinity(config_.cpu_affinity),
            std::memory_order_release);
    }
    // 引用计数租约：run() 返回（worker 退出、执行器停止）时释放，多个
    // 执行器并发启停时只有最后一个停止的才 munlockall（见 ProcessMemoryLockLease）。
    util::ProcessMemoryLockLease memory_lock_lease;
    if (config_.enable_memory_lock) {
        memory_lock_lease = util::ProcessMemoryLockLease::try_acquire();
        memory_locked_.store(
            memory_lock_lease.holds_lock(), std::memory_order_release);
    }

    ready_.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(startup_mutex_);
        startup_reported_ = true;
    }
    startup_cv_.notify_all();
    try {
        worker_->run(stop_token);
        std::lock_guard<std::mutex> lock(status_mutex_);
        stop_reason_ = stop_token.stop_requested() ||
                               stop_requested_.load(std::memory_order_acquire)
                           ? BlockingIoStopReason::Requested
                           : BlockingIoStopReason::WorkerReturned;
    } catch (const std::exception& error) {
        set_error(error.what());
        std::lock_guard<std::mutex> lock(status_mutex_);
        stop_reason_ = BlockingIoStopReason::WorkerException;
    } catch (...) {
        set_error("Blocking I/O worker threw a non-standard exception");
        std::lock_guard<std::mutex> lock(status_mutex_);
        stop_reason_ = BlockingIoStopReason::WorkerException;
    }
    ready_.store(false, std::memory_order_release);
    running_.store(false, std::memory_order_release);
}

void BlockingIoExecutor::set_error(std::string message) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    last_error_message_ = std::move(message);
}

} // namespace kairo
