#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>

#include <kairo/executor.hpp>

namespace {

class MockBlockingWorker final : public kairo::IBlockingIoWorker {
public:
    void run(kairo::StopToken stop_token) override {
        std::unique_lock<std::mutex> lock(mutex_);
        started_.store(true, std::memory_order_release);
        condition_.notify_all();
        condition_.wait(lock, [this, stop_token] {
            return woken_ || stop_token.stop_requested();
        });
        stopped_.store(true, std::memory_order_release);
    }

    void wakeup() noexcept override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            woken_ = true;
        }
        condition_.notify_all();
    }

    bool wait_until_started() {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(1), [this] {
            return started_.load(std::memory_order_acquire);
        });
    }

    bool stopped() const {
        return stopped_.load(std::memory_order_acquire);
    }

private:
    std::atomic<bool> started_{false};
    std::atomic<bool> stopped_{false};
    std::mutex mutex_;
    std::condition_variable condition_;
    bool woken_ = false;
};

} // namespace

int main() {
    kairo::Executor executor;
    kairo::BlockingIoConfig config;
    config.thread_name = "tutorial_io";

    auto worker = std::make_unique<MockBlockingWorker>();
    MockBlockingWorker* worker_view = worker.get();
    kairo::BlockingWorkerSpec spec{
        "tutorial_io", config, std::move(worker)};
    auto handle = executor.start_worker(std::move(spec));
    if (!handle.started() || !worker_view->wait_until_started()) {
        std::cerr << "blocking I/O worker start failed\n";
        executor.shutdown();
        return 1;
    }

    const auto running = handle.status();
    handle.stop();
    const auto stopped = handle.status();
    const bool worker_stopped = worker_view->stopped();

    const bool passed = running.is_running && worker_stopped &&
                        !stopped.is_running &&
                        stopped.stop_reason == kairo::BlockingIoStopReason::Requested &&
                        stopped.wakeup_count == 1;
    std::cout << "blocking worker started=" << (running.is_running ? "yes" : "no")
              << ", stopped=" << (worker_stopped ? "yes" : "no")
              << ", wakeups=" << stopped.wakeup_count << '\n';
    executor.shutdown();
    return passed ? 0 : 1;
}
