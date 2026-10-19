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
    // 请求停止的持锁部分：置位/请求 stop token，返回是否需要补发
    // wakeup。用户 wakeup() 不在此调用（CR-055：锁外执行，防重入死锁）。
    bool request_stop_locked() noexcept;
    void perform_wakeup() noexcept;
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
    // CR-055（0.7.2 §4.3）：stop 终结栅栏。join 期间 thread_ 已被移走，
    // 并发 stop 否则会见到 moved-from 句柄而提前返回；栅栏使第二个
    // stop 等待终结完成（与 RealtimeThreadExecutor 同款协议）。
    bool stop_finalization_in_progress_ = false;
    // 栅栏持有者线程 id（lifecycle_mutex_ 保护）：重入 stop() 判定用。
    std::thread::id stop_finalizer_id_;
    std::condition_variable stop_completion_cv_;
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
