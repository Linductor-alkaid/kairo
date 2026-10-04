#include <kairo/executor.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>

using namespace kairo;

#define TEST_ASSERT(condition, message)                                      \
    do {                                                                     \
        if (!(condition)) {                                                  \
            std::cerr << "FAILED: " << message << " at " << __FILE__       \
                      << ':' << __LINE__ << std::endl;                      \
            return false;                                                    \
        }                                                                    \
    } while (0)

namespace {

class WakeableWorker final : public IBlockingIoWorker {
public:
    void run(StopToken stop_token) override {
        std::unique_lock<std::mutex> lock(mutex_);
        entered_ = true;
        entered_cv_.notify_all();
        wakeup_cv_.wait(lock, [&] { return woken_ || stop_token.stop_requested(); });
    }

    void wakeup() noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        woken_ = true;
        ++wakeup_count_;
        wakeup_cv_.notify_all();
    }

    void wait_until_entered() {
        std::unique_lock<std::mutex> lock(mutex_);
        entered_cv_.wait(lock, [&] { return entered_; });
    }

    unsigned wakeup_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return wakeup_count_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable entered_cv_;
    std::condition_variable wakeup_cv_;
    bool entered_ = false;
    bool woken_ = false;
    unsigned wakeup_count_ = 0;
};

BlockingIoConfig worker_config() {
    BlockingIoConfig config;
    config.thread_name = "stage4-worker";
    config.startup_timeout = std::chrono::milliseconds(1000);
    return config;
}

bool test_worker_handle_preserves_lifecycle_contract() {
    Executor executor;
    auto worker = std::make_unique<WakeableWorker>();
    auto* worker_ptr = worker.get();
    BlockingWorkerSpec spec{"io-worker", worker_config(), std::move(worker)};
    auto handle = executor.start_worker(std::move(spec));

    TEST_ASSERT(handle.started(), "start_worker should report successful startup");
    TEST_ASSERT(handle.name() == "io-worker", "handle should retain worker name");
    worker_ptr->wait_until_entered();
    TEST_ASSERT(handle.status().is_running && handle.status().ready,
                "handle status should expose running ready worker");

    handle.request_stop();
    handle.stop();
    const auto status = handle.status();
    TEST_ASSERT(!status.is_running && status.stop_requested,
                "handle stop should request wakeup and join worker");
    TEST_ASSERT(status.stop_reason == BlockingIoStopReason::Requested,
                "requested stop reason must remain visible");
    TEST_ASSERT(worker_ptr->wakeup_count() == 1,
                "request_stop must retain worker wakeup behavior");
    executor.shutdown();
    return true;
}

bool test_worker_handle_reports_start_failure() {
    Executor executor;
    BlockingWorkerSpec spec;
    spec.name = "invalid-worker";
    spec.config = worker_config();
    spec.config.thread_name.clear();
    spec.worker = std::make_unique<WakeableWorker>();
    const auto handle = executor.start_worker(std::move(spec));
    TEST_ASSERT(!handle.started(), "invalid worker spec must not report started");
    TEST_ASSERT(handle.start_result().error_code == ExecutorErrorCode::InvalidConfig,
                "handle should preserve startup validation failure");
    executor.shutdown();
    return true;
}

bool test_realtime_dispatch_is_explicit_and_bounded() {
    Executor executor;
    RealtimeThreadConfig config;
    config.thread_name = "stage4-realtime";
    config.cycle_period_ns = 1'000'000;
    config.enable_process_memory_lock = false;
    config.timer_slack_ns = 0;
    config.cycle_callback = [] {};
    TEST_ASSERT(executor.register_realtime_task("realtime", config),
                "realtime executor should register");

    TaskOptions options;
    options.intent = ExecutionIntent::RealtimeQueue;
    options.preferred_executor = "realtime";
    auto stopped = executor.dispatch_auto(options, [] {});
    TEST_ASSERT(!stopped.accepted && stopped.backend == ExecutionBackend::Realtime &&
                    stopped.decision.reason == RoutingReason::BackendNotRunning,
                "stopped realtime backend must reject without fallback");

    TEST_ASSERT(executor.start_realtime_task("realtime"), "realtime executor should start");
    std::atomic<int> runs{0};
    std::mutex run_mutex;
    std::condition_variable run_cv;
    auto accepted = executor.dispatch_auto(options, [&] {
        ++runs;
        run_cv.notify_all();
    });
    TEST_ASSERT(accepted.accepted && accepted.backend == ExecutionBackend::Realtime,
                "explicit running realtime backend should accept bounded dispatch");
    {
        std::unique_lock<std::mutex> lock(run_mutex);
        TEST_ASSERT(run_cv.wait_for(lock, std::chrono::milliseconds(250), [&] {
                        return runs.load() == 1;
                    }),
                    "accepted realtime task should run on a subsequent cycle");
    }
    executor.stop_realtime_task("realtime");
    TEST_ASSERT(runs == 1, "realtime task should execute exactly once");
    executor.shutdown();
    return true;
}

bool test_capabilities_enumerate_all_registered_backends() {
    Executor executor;
    TEST_ASSERT(executor.register_lockfree_executor(
                    "lockfree", std::make_unique<LockFreeTaskExecutor>(8)),
                "lock-free executor should register");
    auto worker = std::make_unique<WakeableWorker>();
    BlockingWorkerSpec spec{"blocking", worker_config(), std::move(worker)};
    auto handle = executor.start_worker(std::move(spec));
    TEST_ASSERT(handle.started(), "blocking worker should start");

    const auto capabilities = executor.get_executor_capabilities();
    bool saw_default = false;
    bool saw_lockfree = false;
    bool saw_blocking = false;
    for (const auto& capability : capabilities) {
        saw_default |= capability.backend == ExecutionBackend::DefaultAsync;
        saw_lockfree |= capability.backend == ExecutionBackend::LockFree &&
                        capability.name == "lockfree" && capability.registered;
        saw_blocking |= capability.backend == ExecutionBackend::BlockingIo &&
                        capability.name == "blocking" && capability.running;
    }
    TEST_ASSERT(saw_default && saw_lockfree && saw_blocking,
                "facade capability query should enumerate each registered backend type");
    handle.stop();
    executor.shutdown();
    return true;
}

}  // namespace

int main() {
    bool passed = true;
    passed &= test_worker_handle_preserves_lifecycle_contract();
    passed &= test_worker_handle_reports_start_failure();
    passed &= test_realtime_dispatch_is_explicit_and_bounded();
    passed &= test_capabilities_enumerate_all_registered_backends();
    return passed ? 0 : 1;
}
