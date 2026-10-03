#pragma once

#include "kairo/blocking_io.hpp"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace kairo {

class BlockingIoExecutor final : public IBlockingIoExecutor {
public:
    BlockingIoExecutor(std::string name,
                       BlockingIoConfig config,
                       std::unique_ptr<IBlockingIoWorker> worker);
    ~BlockingIoExecutor() override;

    BlockingIoExecutor(const BlockingIoExecutor&) = delete;
    BlockingIoExecutor& operator=(const BlockingIoExecutor&) = delete;

    bool start() override;
    void request_stop() noexcept override;
    void stop() override;
    std::string get_name() const override;
    BlockingIoExecutorStatus get_status() const override;

private:
    using ThreadFactory = std::function<detail::JThread(
        std::function<void(StopToken)>)>;

    void run(StopToken stop_token) noexcept;
    void request_stop_locked() noexcept;
    void set_error(std::string message);

    const std::string name_;
    const BlockingIoConfig config_;
    std::unique_ptr<IBlockingIoWorker> worker_;
    detail::JThread thread_;
    ThreadFactory thread_factory_{[](std::function<void(StopToken)> entry) {
        return detail::JThread(std::move(entry));
    }};
    std::thread::id worker_id_;

    mutable std::mutex lifecycle_mutex_;
    mutable std::mutex startup_mutex_;
    std::condition_variable startup_cv_;
    bool startup_reported_ = false;
    mutable std::mutex status_mutex_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> ready_{false};
    std::atomic<bool> cpu_affinity_applied_{false};
    std::atomic<bool> memory_locked_{false};
    std::atomic<uint64_t> wakeup_count_{0};
    BlockingIoStopReason stop_reason_{BlockingIoStopReason::None};
    std::string last_error_message_;
};

} // namespace kairo
