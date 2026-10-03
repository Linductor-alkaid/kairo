#include <atomic>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <latch>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>

#include <kairo/blocking_io.hpp>
#include <kairo/executor.hpp>

#define private public
#include "kairo/blocking_io_executor.hpp"
#undef private

using namespace kairo;

#define TEST_ASSERT(condition, message)                                      \
    do {                                                                     \
        if (!(condition)) {                                                  \
            std::cerr << "FAILED: " << message << " at " << __FILE__      \
                      << ':' << __LINE__ << '\n';                          \
            return false;                                                    \
        }                                                                    \
    } while (0)

namespace {

struct WorkerState {
    std::latch entered{1};
    std::latch exited{1};
    std::mutex mutex;
    std::condition_variable cv;
    bool woken = false;
    std::atomic<unsigned> wakeup_count{0};
};

class BlockingWorker final : public IBlockingIoWorker {
public:
    explicit BlockingWorker(std::shared_ptr<WorkerState> state)
        : state_(std::move(state)) {}

    void run(StopToken stop_token) override {
        state_->entered.count_down();
        std::unique_lock<std::mutex> lock(state_->mutex);
        state_->cv.wait(lock, [this, stop_token] {
            return state_->woken || stop_token.stop_requested();
        });
        state_->exited.count_down();
    }

    void wakeup() noexcept override {
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->woken = true;
        }
        state_->wakeup_count.fetch_add(1, std::memory_order_relaxed);
        state_->cv.notify_all();
    }

private:
    std::shared_ptr<WorkerState> state_;
};

class ThrowingWorker final : public IBlockingIoWorker {
public:
    explicit ThrowingWorker(std::shared_ptr<std::latch> failed)
        : failed_(std::move(failed)) {}

    void run(StopToken) override {
        failed_->count_down();
        throw std::runtime_error("transport failed");
    }

    void wakeup() noexcept override {}

private:
    std::shared_ptr<std::latch> failed_;
};

class SelfStoppingWorker final : public IBlockingIoWorker {
public:
    explicit SelfStoppingWorker(std::shared_ptr<std::latch> stopped)
        : stopped_(std::move(stopped)) {}

    void set_executor(BlockingIoExecutor* executor) {
        executor_ = executor;
    }

    void run(StopToken) override {
        executor_->stop();
        stopped_->count_down();
    }

    void wakeup() noexcept override {}

private:
    BlockingIoExecutor* executor_ = nullptr;
    std::shared_ptr<std::latch> stopped_;
};

class ReturningWorker final : public IBlockingIoWorker {
public:
    explicit ReturningWorker(std::shared_ptr<std::latch> returned)
        : returned_(std::move(returned)) {}

    void run(StopToken) override {
        returned_->count_down();
    }

    void wakeup() noexcept override {}

private:
    std::shared_ptr<std::latch> returned_;
};

BlockingIoConfig valid_config() {
    BlockingIoConfig config;
    config.thread_name = "io_test";
    return config;
}

bool test_facade_lifecycle_and_wakeup() {
    std::cout << "Testing blocking I/O facade lifecycle...\n";

    Executor executor;
    auto state = std::make_shared<WorkerState>();
    TEST_ASSERT(executor.register_blocking_io_worker(
                    "lcm_rx", valid_config(), std::make_unique<BlockingWorker>(state)),
                "worker registration should succeed");
    TEST_ASSERT(executor.start_blocking_io_worker("lcm_rx"),
                "worker start should succeed");
    state->entered.wait();

    auto status = executor.get_blocking_io_worker_status("lcm_rx");
    TEST_ASSERT(status.name == "lcm_rx", "status name should match");
    TEST_ASSERT(status.is_running, "worker should be running");
    TEST_ASSERT(status.ready, "worker should be ready after entering run");
    TEST_ASSERT(!executor.start_blocking_io_worker("lcm_rx"),
                "second start should be rejected");

    executor.stop_blocking_io_worker("lcm_rx");
    state->exited.wait();
    status = executor.get_blocking_io_worker_status("lcm_rx");
    TEST_ASSERT(!status.is_running, "worker should stop after wakeup and join");
    TEST_ASSERT(status.stop_requested, "stop should be observable");
    TEST_ASSERT(status.stop_reason == BlockingIoStopReason::Requested,
                "requested stop should retain its reason");
    TEST_ASSERT(status.wakeup_count >= 1, "executor should invoke worker wakeup");
    TEST_ASSERT(state->wakeup_count.load(std::memory_order_relaxed) >= 1,
                "worker should observe at least one wakeup");

    const uint64_t wakeups_after_stop = status.wakeup_count;
    executor.stop_blocking_io_worker("lcm_rx");
    TEST_ASSERT(executor.get_blocking_io_worker_status("lcm_rx").wakeup_count ==
                    wakeups_after_stop,
                "repeated stop should not wake an already joined worker again");

    executor.shutdown();
    return true;
}

bool test_registration_validation_and_worker_failure() {
    std::cout << "Testing blocking I/O validation and worker failure...\n";

    Executor executor;
    BlockingIoConfig invalid;
    invalid.thread_name = "";
    auto invalid_failed = std::make_shared<std::latch>(1);
    auto invalid_result = executor.register_blocking_io_worker_ex(
        "invalid", invalid, std::make_unique<ThrowingWorker>(invalid_failed));
    TEST_ASSERT(!invalid_result.ok && invalid_result.error_code == ExecutorErrorCode::InvalidConfig,
                "empty thread name should fail validation");
    TEST_ASSERT(executor.get_failure_status().submit_rejected_count == 1,
                "registration validation failure should be visible through facade diagnostics");
    const auto missing_result = executor.start_blocking_io_worker_ex("missing_io");
    TEST_ASSERT(!missing_result.ok && missing_result.error_code == ExecutorErrorCode::NotFound,
                "starting an unknown I/O worker should report NotFound");
    TEST_ASSERT(executor.get_blocking_io_worker_status("missing_io").name == "missing_io",
                "missing I/O worker status should retain the requested name");

    auto failed = std::make_shared<std::latch>(1);
    auto result = executor.register_blocking_io_worker_ex(
        "throwing", valid_config(), std::make_unique<ThrowingWorker>(failed));
    TEST_ASSERT(result.ok, "throwing worker should register");
    TEST_ASSERT(executor.start_blocking_io_worker("throwing"),
                "worker thread creation should still succeed");

    failed->wait();
    executor.stop_blocking_io_worker("throwing");
    const auto status = executor.get_blocking_io_worker_status("throwing");
    TEST_ASSERT(!status.is_running, "throwing worker should stop itself");
    TEST_ASSERT(status.stop_reason == BlockingIoStopReason::WorkerException,
                "worker exception should be recorded separately from task exceptions");
    TEST_ASSERT(status.last_error_message == "transport failed",
                "worker exception message should be retained");
    TEST_ASSERT(executor.get_failure_status().task_exception_count == 0,
                "runtime transport failure must not become a task exception");

    executor.shutdown();
    return true;
}

bool test_cross_executor_name_conflicts() {
    std::cout << "Testing blocking I/O cross-executor name conflicts...\n";

    Executor executor;
    auto state = std::make_shared<WorkerState>();
    TEST_ASSERT(executor.register_blocking_io_worker(
                    "shared_name", valid_config(), std::make_unique<BlockingWorker>(state)),
                "I/O worker registration should succeed");

    RealtimeThreadConfig realtime_config;
    realtime_config.thread_name = "name_conflict_rt";
    realtime_config.cycle_period_ns = 1'000'000;
    realtime_config.cycle_callback = [] {};
    const auto realtime_result =
        executor.register_realtime_task_ex("shared_name", realtime_config);
    TEST_ASSERT(!realtime_result.ok &&
                    realtime_result.error_code == ExecutorErrorCode::DuplicateName,
                "realtime registration must reject an I/O worker name");

    executor.stop_blocking_io_worker("shared_name");
    executor.shutdown();
    return true;
}

bool test_concurrent_stop_is_serialized() {
    std::cout << "Testing concurrent blocking I/O stop...\n";

    Executor executor;
    auto state = std::make_shared<WorkerState>();
    TEST_ASSERT(executor.register_blocking_io_worker(
                    "can_rx", valid_config(), std::make_unique<BlockingWorker>(state)),
                "worker registration should succeed");
    TEST_ASSERT(executor.start_blocking_io_worker("can_rx"),
                "worker start should succeed");
    state->entered.wait();

    std::thread first([&] { executor.stop_blocking_io_worker("can_rx"); });
    std::thread second([&] { executor.stop_blocking_io_worker("can_rx"); });
    first.join();
    second.join();
    state->exited.wait();

    const auto status = executor.get_blocking_io_worker_status("can_rx");
    TEST_ASSERT(!status.is_running, "concurrent stop should leave worker stopped");
    TEST_ASSERT(status.wakeup_count == 1,
                "concurrent stop should issue one wakeup for the active worker");
    executor.shutdown();
    return true;
}

bool test_start_failure_and_timeout_rollback() {
    std::cout << "Testing blocking I/O start rollback...\n";

    auto state = std::make_shared<WorkerState>();
    BlockingIoExecutor creation_failure(
        "creation_failure", valid_config(), std::make_unique<BlockingWorker>(state));
    creation_failure.thread_factory_ = [](std::function<void(StopToken)>) -> kairo::detail::JThread {
        throw std::system_error(
            std::make_error_code(std::errc::resource_unavailable_try_again),
            "test thread creation failure");
    };
    TEST_ASSERT(!creation_failure.start(), "thread creation failure should reject start");
    auto status = creation_failure.get_status();
    TEST_ASSERT(!status.is_running && status.stop_reason == BlockingIoStopReason::StartFailed,
                "thread creation failure should roll status back to StartFailed");

    auto timeout_state = std::make_shared<WorkerState>();
    auto timeout_config = valid_config();
    timeout_config.startup_timeout = std::chrono::milliseconds(1);
    BlockingIoExecutor timeout_executor(
        "startup_timeout", timeout_config, std::make_unique<BlockingWorker>(timeout_state));
    timeout_executor.thread_factory_ = [](std::function<void(StopToken)>) {
        return kairo::detail::JThread([](StopToken stop_token) {
            while (!stop_token.stop_requested()) {
                std::this_thread::yield();
            }
        });
    };
    TEST_ASSERT(!timeout_executor.start(), "missing ready signal should time out startup");
    status = timeout_executor.get_status();
    TEST_ASSERT(!status.is_running && status.stop_reason == BlockingIoStopReason::StartFailed,
                "startup timeout should stop and roll status back");
    TEST_ASSERT(status.wakeup_count == 1,
                "startup timeout should wake the owned worker before join");
    return true;
}

bool test_worker_self_stop_defers_join() {
    std::cout << "Testing blocking I/O worker self-stop...\n";

    auto stopped = std::make_shared<std::latch>(1);
    auto worker = std::make_unique<SelfStoppingWorker>(stopped);
    auto* worker_ptr = worker.get();
    BlockingIoExecutor executor("self_stop", valid_config(), std::move(worker));
    worker_ptr->set_executor(&executor);

    TEST_ASSERT(executor.start(), "self-stopping worker should start");
    stopped->wait();
    executor.stop();
    const auto status = executor.get_status();
    TEST_ASSERT(!status.is_running, "external stop should join a self-stopped worker");
    TEST_ASSERT(status.stop_reason == BlockingIoStopReason::Requested,
                "self-stop should retain the requested stop reason");
    return true;
}

bool test_unrequested_worker_return_is_observable() {
    std::cout << "Testing blocking I/O unexpected worker return...\n";

    Executor executor;
    auto returned = std::make_shared<std::latch>(1);
    TEST_ASSERT(executor.register_blocking_io_worker(
                    "returning", valid_config(), std::make_unique<ReturningWorker>(returned)),
                "returning worker registration should succeed");
    TEST_ASSERT(executor.start_blocking_io_worker("returning"),
                "returning worker should start");
    returned->wait();
    executor.stop_blocking_io_worker("returning");
    const auto status = executor.get_blocking_io_worker_status("returning");
    TEST_ASSERT(status.stop_reason == BlockingIoStopReason::WorkerReturned,
                "unrequested worker return should remain observable after join");
    executor.shutdown();
    return true;
}

bool test_shutdown_false_joins_io_worker() {
    std::cout << "Testing shutdown(false) joins blocking I/O worker...\n";

    Executor executor;
    auto state = std::make_shared<WorkerState>();
    TEST_ASSERT(executor.register_blocking_io_worker(
                    "serial_rx", valid_config(), std::make_unique<BlockingWorker>(state)),
                "worker registration should succeed");
    TEST_ASSERT(executor.start_blocking_io_worker("serial_rx"),
                "worker start should succeed");
    state->entered.wait();

    executor.shutdown(false);
    state->exited.wait();
    TEST_ASSERT(state->wakeup_count.load(std::memory_order_relaxed) >= 1,
                "shutdown(false) must wake and join owned I/O workers");
    TEST_ASSERT(executor.get_blocking_io_worker_list().empty(),
                "shutdown should remove I/O executors from the registry");
    return true;
}

bool test_instance_destructor_joins_io_worker() {
    std::cout << "Testing blocking I/O instance destructor...\n";

    auto state = std::make_shared<WorkerState>();
    {
        Executor executor;
        TEST_ASSERT(executor.register_blocking_io_worker(
                        "raii_rx", valid_config(), std::make_unique<BlockingWorker>(state)),
                    "worker registration should succeed");
        TEST_ASSERT(executor.start_blocking_io_worker("raii_rx"),
                    "worker start should succeed");
        state->entered.wait();
    }
    state->exited.wait();
    TEST_ASSERT(state->wakeup_count.load(std::memory_order_relaxed) == 1,
                "Executor destructor should wake and join an owned I/O worker once");
    return true;
}

bool test_mixed_executor_shutdown() {
    std::cout << "Testing mixed executor shutdown...\n";

    Executor executor;
    auto state = std::make_shared<WorkerState>();
    TEST_ASSERT(executor.register_blocking_io_worker(
                    "mixed_io", valid_config(), std::make_unique<BlockingWorker>(state)),
                "I/O worker registration should succeed");

    RealtimeThreadConfig realtime_config;
    realtime_config.thread_name = "mixed_rt";
    realtime_config.cycle_period_ns = 1'000'000;
    realtime_config.enable_process_memory_lock = false;
    realtime_config.timer_slack_ns = 0;
    realtime_config.cycle_callback = [] {};
    TEST_ASSERT(executor.register_realtime_task("mixed_rt", realtime_config),
                "realtime executor registration should succeed");
    TEST_ASSERT(executor.start_realtime_task("mixed_rt"),
                "realtime executor start should succeed");
    TEST_ASSERT(executor.start_blocking_io_worker("mixed_io"),
                "I/O worker start should succeed");
    state->entered.wait();
    TEST_ASSERT(!executor.submit_periodic(1, [] {}).empty(),
                "periodic task should start the timer and async executor");

    executor.shutdown(false);
    state->exited.wait();
    TEST_ASSERT(executor.get_blocking_io_worker_list().empty(),
                "mixed shutdown should clear I/O registry");
    return true;
}

} // namespace

int main() {
    bool ok = true;
    ok &= test_facade_lifecycle_and_wakeup();
    ok &= test_registration_validation_and_worker_failure();
    ok &= test_cross_executor_name_conflicts();
    ok &= test_concurrent_stop_is_serialized();
    ok &= test_start_failure_and_timeout_rollback();
    ok &= test_worker_self_stop_defers_join();
    ok &= test_unrequested_worker_return_is_observable();
    ok &= test_shutdown_false_joins_io_worker();
    ok &= test_instance_destructor_joins_io_worker();
    ok &= test_mixed_executor_shutdown();
    std::cout << (ok ? "All blocking I/O tests PASSED\n" : "Blocking I/O tests FAILED\n");
    return ok ? 0 : 1;
}
