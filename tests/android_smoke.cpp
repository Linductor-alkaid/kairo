#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <thread>

#include <kairo/comm.hpp>
#include <kairo/executor.hpp>

#define SMOKE_CHECK(condition, message)                                      \
    do {                                                                     \
        if (!(condition)) {                                                  \
            std::cerr << "FAILED: " << message << " at " << __FILE__      \
                      << ':' << __LINE__ << '\n';                          \
            return false;                                                    \
        }                                                                    \
    } while (0)

namespace {

class BlockingSmokeWorker final : public kairo::IBlockingIoWorker {
public:
    void run(kairo::StopToken stop_token) override {
        started_.store(true, std::memory_order_release);
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this, stop_token] {
            return woken_ || stop_token.stop_requested();
        });
        stopped_.store(true, std::memory_order_release);
    }

    void wakeup() noexcept override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            woken_ = true;
        }
        cv_.notify_all();
    }

    bool wait_started() {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, std::chrono::seconds(5), [this] {
            return started_.load(std::memory_order_acquire);
        });
    }

    bool stopped() const { return stopped_.load(std::memory_order_acquire); }

private:
    std::atomic<bool> started_{false};
    std::atomic<bool> stopped_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
    bool woken_ = false;
};

bool smoke_async_submit() {
    kairo::Executor executor;
    auto future = executor.submit_auto([] { return 42; });
    SMOKE_CHECK(future.get() == 42, "async task result mismatch");
    executor.shutdown();
    return true;
}

bool smoke_blocking_io_lifecycle() {
    kairo::Executor executor;
    kairo::BlockingIoConfig config;
    config.thread_name = "android_smoke_io";
    config.startup_timeout = std::chrono::seconds(5);

    auto worker = std::make_unique<BlockingSmokeWorker>();
    auto* worker_view = worker.get();
    kairo::BlockingWorkerSpec spec{
        "android_smoke_io", config, std::move(worker)};
    auto handle = executor.start_worker(std::move(spec));

    SMOKE_CHECK(handle.started(), "blocking I/O worker should start");
    SMOKE_CHECK(worker_view->wait_started(), "blocking I/O worker should enter run");
    SMOKE_CHECK(handle.status().is_running, "blocking I/O worker should report running");

    handle.stop();
    SMOKE_CHECK(worker_view->stopped(), "blocking I/O worker should stop after wakeup");
    SMOKE_CHECK(!handle.status().is_running, "blocking I/O worker should report stopped");
    executor.shutdown();
    return true;
}

bool smoke_comm_channel() {
    kairo::comm::MpscChannel<int> channel;
    SMOKE_CHECK(channel.try_send(7), "channel send should succeed");
    int value = 0;
    SMOKE_CHECK(channel.try_receive(value), "channel receive should succeed");
    SMOKE_CHECK(value == 7, "channel value mismatch");
    channel.close();
    SMOKE_CHECK(channel.is_closed(), "channel should be closed");
    return true;
}

bool smoke_mpsc_burst() {
    constexpr int kTotal = 20'000;
    kairo::comm::MpscChannel<int> channel;
    std::atomic<int> received{0};
    std::atomic<bool> producer_done{false};

    std::thread producer([&] {
        for (int index = 0; index < kTotal; ++index) {
            while (!channel.try_send(index)) {
                std::this_thread::yield();
            }
        }
        producer_done.store(true, std::memory_order_release);
    });

    int value = 0;
    while (received.load(std::memory_order_acquire) < kTotal) {
        if (channel.try_receive(value)) {
            received.fetch_add(1, std::memory_order_relaxed);
        } else if (producer_done.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }

    producer.join();
    channel.close();
    SMOKE_CHECK(received.load(std::memory_order_acquire) == kTotal,
                "MPSC burst lost messages");
    return true;
}

int soak_seconds() {
    const char* value = std::getenv("KAIRO_ANDROID_SOAK_SECONDS");
    if (value == nullptr || *value == '\0') {
        return 0;
    }
    try {
        const int seconds = std::stoi(value);
        return seconds > 0 ? seconds : 0;
    } catch (...) {
        return 0;
    }
}

bool smoke_mpsc_soak() {
    const int seconds = soak_seconds();
    if (seconds == 0) {
        return true;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    kairo::comm::MpscChannel<int> channel;
    std::atomic<uint64_t> sent{0};
    std::atomic<uint64_t> received{0};
    std::atomic<bool> stop{false};

    std::thread producer([&] {
        while (!stop.load(std::memory_order_acquire)) {
            if (channel.try_send(1)) {
                sent.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    int value = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        while (channel.try_receive(value)) {
            received.fetch_add(1, std::memory_order_relaxed);
        }
        std::this_thread::yield();
    }
    while (channel.try_receive(value)) {
        received.fetch_add(1, std::memory_order_relaxed);
    }
    stop.store(true, std::memory_order_release);
    producer.join();
    while (channel.try_receive(value)) {
        received.fetch_add(1, std::memory_order_relaxed);
    }
    channel.close();

    std::cout << "soak: sent=" << sent.load() << " received="
              << received.load() << '\n';
    SMOKE_CHECK(sent.load(std::memory_order_relaxed) ==
                    received.load(std::memory_order_relaxed),
                "MPSC soak sent/received mismatch");
    return true;
}

} // namespace

int main() {
    bool ok = true;
    ok &= smoke_async_submit();
    ok &= smoke_blocking_io_lifecycle();
    ok &= smoke_comm_channel();
    ok &= smoke_mpsc_burst();
    ok &= smoke_mpsc_soak();
    std::cout << (ok ? "All Android smoke tests PASSED\n"
                     : "Android smoke tests FAILED\n");
    return ok ? 0 : 1;
}
