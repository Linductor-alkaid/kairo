#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <kairo/executor.hpp>
#include <kairo/executor_manager.hpp>
#include <kairo/interfaces.hpp>
#include <kairo/monitor/executor_monitor.hpp>
#include <kairo/monitor/executor_snapshot_formatter.hpp>

using namespace kairo;

#define TEST_ASSERT(condition, message)                                      \
    do {                                                                     \
        if (!(condition)) {                                                  \
            std::cerr << "FAILED: " << message << " at " << __FILE__       \
                      << ":" << __LINE__ << std::endl;                     \
            return false;                                                  \
        }                                                                   \
    } while (0)

// Worker 在结算 future 之后才清理诊断表（record_task_complete/timeout 的
// 擦除发生在包装层 promise 结算之后）：future 就绪不代表快照已排水。极端
// 负载下该间隙可达毫秒级，用有界等待替代立即断言（Phase 2 同款模式）。
bool wait_in_flight_drained(Executor& executor, int max_attempts = 50) {
    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        if (executor.get_snapshot().in_flight_count == 0) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return executor.get_snapshot().in_flight_count == 0;
}

class IdleBlockingWorker final : public IBlockingIoWorker {
public:
    void run(StopToken stop_token) override {
        while (!stop_token.stop_requested()) {
            std::this_thread::yield();
        }
    }

    void wakeup() noexcept override {}
};

class SnapshotMockGpuExecutor final : public IGpuExecutor {
public:
    explicit SnapshotMockGpuExecutor(std::string name) : name_(std::move(name)) {}

    std::string get_name() const override { return name_; }
    gpu::GpuDeviceInfo get_device_info() const override {
        gpu::GpuDeviceInfo info;
        info.name = name_ + "_device";
        info.backend = gpu::GpuBackend::CUDA;
        return info;
    }
    gpu::GpuExecutorStatus get_status() const override {
        gpu::GpuExecutorStatus status;
        status.name = name_;
        status.backend = gpu::GpuBackend::CUDA;
        status.is_running = running_.load(std::memory_order_acquire);
        status.active_kernels = 2;
        status.queue_size = 3;
        status.failed_kernels = 1;
        return status;
    }
    bool start() override {
        running_.store(true, std::memory_order_release);
        return true;
    }
    void stop() override { running_.store(false, std::memory_order_release); }
    void wait_for_completion() override {}
    void* allocate_device_memory(size_t) override { return nullptr; }
    void free_device_memory(void*) override {}
    bool copy_to_device(void*, const void*, size_t, bool, int) override { return false; }
    bool copy_to_host(void*, const void*, size_t, bool, int) override { return false; }
    bool copy_device_to_device(void*, const void*, size_t, bool, int) override { return false; }
    void synchronize() override {}
    void synchronize_stream(int) override {}
    int create_stream() override { return -1; }
    void destroy_stream(int) override {}
    bool add_stream_callback(int, std::function<void()>) override { return false; }

protected:
    std::future<void> submit_kernel_impl(
        std::function<void(void*)>, const gpu::GpuTaskConfig&) override {
        std::promise<void> promise;
        promise.set_value();
        return promise.get_future();
    }

private:
    std::string name_;
    std::atomic<bool> running_{false};
};

bool test_snapshot_does_not_lazy_initialize() {
    Executor executor;
    const auto before = executor.get_snapshot();
    const auto after = executor.get_snapshot();

    TEST_ASSERT(before.lifecycle == ExecutorLifecycleState::Created,
                "uninitialized executor must report Created");
    TEST_ASSERT(!before.completion.is_initialized,
                "snapshot must not initialize the default async executor");
    TEST_ASSERT(!after.async.is_running,
                "default async executor must remain stopped after snapshot");
    TEST_ASSERT(after.snapshot_sequence == before.snapshot_sequence + 1,
                "snapshot sequence must increase monotonically");
    return true;
}

bool test_snapshot_text_is_stable_and_complete() {
    Executor executor;
    const auto snapshot = executor.get_snapshot();
    const auto text = monitor::format_executor_snapshot(snapshot);

    TEST_ASSERT(text.rfind("executor_snapshot\n", 0) == 0,
                "snapshot text must have a stable format marker");
    TEST_ASSERT(text.find("schema_version=3\n") != std::string::npos,
                "snapshot text must include schema version");
    TEST_ASSERT(text.find("state_epoch=") != std::string::npos,
                "snapshot text must include the consistency epoch");
    TEST_ASSERT(text.find("snapshot_sequence=") != std::string::npos,
                "snapshot text must include snapshot sequence");
    TEST_ASSERT(text.find("captured_at_steady_ns=") != std::string::npos,
                "snapshot text must use an explicit steady-clock unit");
    TEST_ASSERT(text.find("collection_duration_ns=") != std::string::npos,
                "snapshot text must include collection duration in nanoseconds");
    TEST_ASSERT(text.find("lifecycle=Created\n") != std::string::npos,
                "snapshot text must use lifecycle strings rather than integers");
    TEST_ASSERT(text.find("partial=false\n") != std::string::npos,
                "snapshot text must include partial state");
    TEST_ASSERT(text.find("gpu.count=0\n") != std::string::npos,
                "empty GPU backends must be represented readably");
    TEST_ASSERT(executor.get_snapshot_text().rfind("executor_snapshot\n", 0) == 0,
                "facade text API must use the stable formatter");
    const auto export_result = monitor::format_executor_snapshot_with_metrics(snapshot);
    TEST_ASSERT(export_result.text == text,
                "metric formatter must preserve the stable text output");
    TEST_ASSERT(export_result.metrics.formatting_duration.count() >= 0,
                "metric formatter must report a duration");
    TEST_ASSERT(export_result.metrics.formatting_allocation_count > 0,
                "metric formatter must report its output allocations");
    return true;
}

bool test_snapshot_text_handles_partial_providers() {
    ExecutorManager manager;
    std::atomic<ExecutorLifecycleState> lifecycle{ExecutorLifecycleState::Created};
    monitor::ExecutorMonitor monitor(
        manager, lifecycle,
        [] { return CompletionStatus{}; },
        []() -> ExecutorFailureStatus { throw std::runtime_error("test provider failure"); },
        [] { return std::vector<ExecutorFailureEvent>{}; },
        [] { return std::map<std::string, TaskStatistics>{}; });

    const auto snapshot = monitor.collect();
    const auto text = monitor::format_executor_snapshot(snapshot);
    TEST_ASSERT(snapshot.partial, "provider failure must create a partial snapshot");
    TEST_ASSERT(text.find("partial=true\n") != std::string::npos,
                "partial snapshot text must preserve its consistency marker");
    TEST_ASSERT(text.find("consistency_note=failures\n") != std::string::npos,
                "partial snapshot text must identify the unavailable provider");
    TEST_ASSERT(text.find("gpu.count=0\n") != std::string::npos,
                "empty GPU map must remain readable in partial snapshot text");
    return true;
}

bool test_snapshot_diagnostic_callback_on_timeout_and_start_failure() {
    Executor executor;
    ExecutorConfig config;
    config.min_threads = 1;
    config.max_threads = 1;
    TEST_ASSERT(executor.initialize(config), "executor initialization must succeed");

    std::atomic<uint64_t> callback_count{0};
    std::atomic<uint64_t> last_sequence{0};
    std::atomic<bool> saw_pending_work{false};
    executor.set_snapshot_diagnostic_callback(
        [&callback_count, &last_sequence, &saw_pending_work](const ExecutorSnapshot& snapshot) {
            callback_count.fetch_add(1, std::memory_order_relaxed);
            last_sequence.store(snapshot.snapshot_sequence, std::memory_order_relaxed);
            saw_pending_work.store(snapshot.completion.pending_tasks != 0,
                                   std::memory_order_relaxed);
        });

    std::promise<void> release;
    auto gate = release.get_future().share();
    std::promise<void> started;
    auto task = executor.submit([gate, &started]() {
        started.set_value();
        gate.wait();
    });
    started.get_future().wait();

    const auto wait_result = executor.wait_for_completion(std::chrono::milliseconds{1});
    TEST_ASSERT(wait_result.timed_out, "blocked task must produce a wait timeout");
    TEST_ASSERT(wait_result.diagnostic_snapshot.has_value(),
                "wait timeout must retain a full lifecycle snapshot");
    TEST_ASSERT(wait_result.diagnostic_snapshot->completion.pending_tasks != 0,
                "retained timeout snapshot must include pending work evidence");
    TEST_ASSERT(callback_count.load(std::memory_order_relaxed) == 1,
                "wait timeout must invoke the snapshot diagnostic callback once");
    TEST_ASSERT(last_sequence.load(std::memory_order_relaxed) != 0,
                "timeout callback must receive a collected snapshot");
    TEST_ASSERT(last_sequence.load(std::memory_order_relaxed) ==
                    wait_result.diagnostic_snapshot->snapshot_sequence,
                "timeout callback and WaitResult must share the same snapshot");
    TEST_ASSERT(saw_pending_work.load(std::memory_order_relaxed),
                "timeout callback snapshot must retain pending work evidence");

    executor.set_snapshot_diagnostic_callback(
        [](const ExecutorSnapshot&) { throw std::runtime_error("diagnostic callback failure"); });
    const auto second_wait_result = executor.wait_for_completion(std::chrono::milliseconds{1});
    TEST_ASSERT(second_wait_result.timed_out,
                "diagnostic callback failure must not change timeout result");
    TEST_ASSERT(second_wait_result.diagnostic_snapshot.has_value(),
                "callback failure must not discard the timeout snapshot");

    release.set_value();
    task.get();
    executor.shutdown();

    Executor invalid_executor;
    std::atomic<bool> saw_failed_lifecycle{false};
    invalid_executor.set_snapshot_diagnostic_callback(
        [&saw_failed_lifecycle](const ExecutorSnapshot& snapshot) {
            saw_failed_lifecycle.store(snapshot.lifecycle == ExecutorLifecycleState::Failed,
                                       std::memory_order_relaxed);
        });
    ExecutorConfig invalid_config;
    invalid_config.min_threads = 2;
    invalid_config.max_threads = 1;
    TEST_ASSERT(!invalid_executor.initialize(invalid_config),
                "invalid configuration must fail initialization");
    TEST_ASSERT(saw_failed_lifecycle.load(std::memory_order_relaxed),
                "initialization failure callback must include Failed lifecycle snapshot");
    return true;
}

bool test_snapshot_reports_async_work_and_shutdown() {
    Executor executor;
    ExecutorConfig config;
    config.min_threads = 1;
    config.max_threads = 1;
    TEST_ASSERT(executor.initialize(config), "executor initialization must succeed");

    const auto running = executor.get_snapshot();
    TEST_ASSERT(running.lifecycle == ExecutorLifecycleState::Running,
                "initialized executor must report Running");
    TEST_ASSERT(running.async.is_running,
                "snapshot must include the running async backend");
    TEST_ASSERT(running.async.is_running == executor.get_async_executor_status().is_running,
                "snapshot async status must agree with the existing status API");
    TEST_ASSERT(running.running_backend_count >= 1,
                "running backend count must include async backend");

    std::promise<void> release;
    auto gate = release.get_future().share();
    std::promise<void> started;
    auto task = executor.submit([gate, &started]() {
        started.set_value();
        gate.wait();
    });
    started.get_future().wait();

    const auto active = executor.get_snapshot();
    TEST_ASSERT(active.active_task_count >= 1,
                "snapshot must include active async tasks");
    TEST_ASSERT(active.completion.pending_tasks >= 1,
                "completion snapshot must agree that work is pending");

    release.set_value();
    task.get();
    (void)executor.wait_for_completion(std::chrono::seconds{300});
    executor.shutdown();

    const auto stopped = executor.get_snapshot();
    TEST_ASSERT(stopped.lifecycle == ExecutorLifecycleState::Stopped,
                "completed shutdown must report Stopped");
    TEST_ASSERT(!stopped.async.is_running,
                "async backend must be stopped after shutdown");
    return true;
}

bool test_snapshot_reports_bounded_in_flight_tasks() {
    Executor executor;
    ExecutorConfig config;
    config.min_threads = 1;
    config.max_threads = 1;
    config.enable_work_stealing = false;
    TEST_ASSERT(executor.initialize(config), "executor initialization must succeed");

    executor.set_in_flight_task_capacity(2);
    executor.set_in_flight_task_sampling_rate(1.0);
    std::promise<void> running;
    auto release = std::make_shared<std::promise<void>>();
    auto release_future = release->get_future().share();
    auto first = executor.submit([&running, release_future] {
        running.set_value();
        release_future.wait();
    });
    running.get_future().wait();
    auto second = executor.submit([] {});

    const auto active = executor.get_snapshot();
    TEST_ASSERT(active.in_flight_count == 2,
                "snapshot must retain the sampled running and queued tasks");
    TEST_ASSERT(active.in_flight_state_counts.at(TaskLifecycleState::Running) == 1,
                "snapshot must identify the running task");
    TEST_ASSERT(active.in_flight_state_counts.at(TaskLifecycleState::Queued) == 1,
                "snapshot must identify the queued task");
    TEST_ASSERT(active.oldest_in_flight_age.count() >= 0,
                "snapshot must include an oldest in-flight age");
    TEST_ASSERT(monitor::format_executor_snapshot(active).find("in_flight.state[Queued]=1\n") !=
                    std::string::npos,
                "text snapshot must use stable in-flight state strings");

    release->set_value();
    first.get();
    second.get();
    TEST_ASSERT(wait_in_flight_drained(executor),
                "completed tasks must be removed from the diagnostic table");

    executor.set_in_flight_task_capacity(1);
    std::promise<void> running_again;
    auto release_again = std::make_shared<std::promise<void>>();
    auto release_again_future = release_again->get_future().share();
    auto third = executor.submit([&running_again, release_again_future] {
        running_again.set_value();
        release_again_future.wait();
    });
    running_again.get_future().wait();
    auto fourth = executor.submit([] {});
    const auto overflow = executor.get_snapshot();
    TEST_ASSERT(overflow.in_flight_diagnostics_incomplete,
                "capacity overflow must explicitly mark diagnostics incomplete");
    TEST_ASSERT(overflow.in_flight_dropped_count >= 1,
                "capacity overflow must retain a dropped diagnostic count");
    release_again->set_value();
    third.get();
    fourth.get();
    // 排水 third/fourth 的诊断条目，使下方 monitoring-off 断言只反映
    // "禁用后不新增滞留"，不掺入前一批任务的 future-vs-cleanup 竞态。
    TEST_ASSERT(wait_in_flight_drained(executor),
                "capacity-overflow tasks must eventually leave the diagnostic table");

    executor.enable_monitoring(false);
    auto unmonitored = executor.submit([] {});
    unmonitored.get();
    TEST_ASSERT(executor.get_snapshot().in_flight_count == 0,
                "disabled monitoring must not retain in-flight task diagnostics");
    executor.enable_monitoring(true);
    executor.shutdown();
    return true;
}

bool test_in_flight_diagnostics_do_not_change_soft_timeout() {
    Executor executor;
    ExecutorConfig config;
    config.min_threads = 1;
    config.max_threads = 1;
    config.enable_work_stealing = false;
    config.task_timeout_ms = 1;
    TEST_ASSERT(executor.initialize(config), "executor initialization must succeed");
    executor.set_in_flight_task_capacity(8);

    // 环境负载（CPU 超订阅/满载测试机）可能把任务拾取拖过 1ms 软超时：
    // 池侧 pre-execution 检查会跳过任务体（既定语义，外层 future 以
    // TimedOutException 结算），任务体内置位的 promise 永不满足——直接
    // wait 内层 promise 会永久挂死（曾以 ~1/20 概率挂死并发回归）。
    // 内层 promise 等待必须与外层 future 赛跑，环境诱发跳过时有界重试。
    auto wait_started_or_skipped = [](std::future<void>& started,
                                      std::future<void>& task_future) -> bool {
        while (started.wait_for(std::chrono::milliseconds(20)) !=
               std::future_status::ready) {
            if (task_future.wait_for(std::chrono::milliseconds(0)) ==
                std::future_status::ready) {
                return false;  // 环境诱发 pre-execution 软超时跳过
            }
        }
        return true;
    };

    std::promise<void> running;
    auto release = std::make_shared<std::promise<void>>();
    const auto release_future = release->get_future().share();
    std::future<void> first;
    bool first_started = false;
    for (int attempt = 0; attempt < 5 && !first_started; ++attempt) {
        running = std::promise<void>();
        first = executor.submit([&running, release_future] {
            running.set_value();
            release_future.wait();
        });
        std::future<void> started = running.get_future();
        first_started = wait_started_or_skipped(started, first);
    }
    TEST_ASSERT(first_started,
                "task body must start under normal load; 1ms soft timeout "
                "kept skipping it (environment too loaded)");
    auto expired = executor.submit([] {});
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    release->set_value();
    first.get();
    bool timed_out = false;
    try {
        expired.get();
    } catch (const TimedOutException&) {
        timed_out = true;
    }
    TEST_ASSERT(timed_out, "soft timeout future semantics must remain unchanged");
    TEST_ASSERT(wait_in_flight_drained(executor),
                "soft-timed-out task must leave the in-flight table");

    executor.set_in_flight_task_sampling_rate(0.0);
    std::promise<void> sampled_running;
    auto sampled_release = std::make_shared<std::promise<void>>();
    const auto sampled_release_future = sampled_release->get_future().share();
    std::future<void> unsampled;
    bool sampled_started = false;
    for (int attempt = 0; attempt < 5 && !sampled_started; ++attempt) {
        sampled_running = std::promise<void>();
        unsampled = executor.submit([&sampled_running, sampled_release_future] {
            sampled_running.set_value();
            sampled_release_future.wait();
        });
        std::future<void> sampled_started_future = sampled_running.get_future();
        sampled_started =
            wait_started_or_skipped(sampled_started_future, unsampled);
    }
    TEST_ASSERT(sampled_started,
                "task body must start under normal load; 1ms soft timeout "
                "kept skipping it (environment too loaded)");
    TEST_ASSERT(executor.get_snapshot().in_flight_count == 0,
                "zero in-flight sampling must not retain a task");
    sampled_release->set_value();
    unsampled.get();
    executor.shutdown();
    return true;
}

bool test_snapshot_reports_failure_and_failed_initialization() {
    Executor invalid_executor;
    ExecutorConfig invalid_config;
    invalid_config.min_threads = 2;
    invalid_config.max_threads = 1;
    TEST_ASSERT(!invalid_executor.initialize(invalid_config),
                "invalid configuration must fail initialization");
    const auto failed = invalid_executor.get_snapshot();
    TEST_ASSERT(failed.lifecycle == ExecutorLifecycleState::Failed,
                "initialization failure must report Failed");
    TEST_ASSERT(failed.failures.submit_rejected_count >= 1,
                "initialization failure must be visible in failure counters");
    TEST_ASSERT(!failed.recent_failures.empty(),
                "initialization failure must retain a recent failure event");

    Executor executor;
    TEST_ASSERT(!executor.start_realtime_task("missing"),
                "missing realtime backend must fail visibly");
    const auto snapshot = executor.get_snapshot();
    TEST_ASSERT(snapshot.failures.submit_rejected_count >= 1,
                "snapshot must include facade failure status");
    TEST_ASSERT(!snapshot.recent_failures.empty(),
                "snapshot must include recent facade failures");
    return true;
}

bool test_snapshot_includes_registered_backends() {
    Executor executor;
    RealtimeThreadConfig realtime_config;
    realtime_config.thread_name = "snapshot-rt";
    realtime_config.cycle_period_ns = 1'000'000;
    TEST_ASSERT(executor.register_realtime_task("snapshot_rt", realtime_config),
                "realtime backend registration must succeed");

    BlockingIoConfig blocking_config;
    blocking_config.thread_name = "snapshot-io";
    TEST_ASSERT(executor.register_blocking_io_worker(
                    "snapshot_io", blocking_config,
                    std::make_unique<IdleBlockingWorker>()),
                "blocking I/O backend registration must succeed");

    const auto snapshot = executor.get_snapshot();
    TEST_ASSERT(snapshot.realtime.contains("snapshot_rt"),
                "snapshot must include registered realtime backend");
    TEST_ASSERT(snapshot.blocking_io.contains("snapshot_io"),
                "snapshot must include registered Blocking I/O backend");
    executor.shutdown();
    return true;
}

bool test_snapshot_uses_backend_specific_work_states() {
    Executor executor;
    RealtimeThreadConfig realtime_config;
    realtime_config.thread_name = "snapshot-backend-rt";
    realtime_config.cycle_period_ns = 1'000'000;
    TEST_ASSERT(executor.register_realtime_task("snapshot_backend_rt", realtime_config),
                "realtime backend registration must succeed");
    TEST_ASSERT(executor.start_realtime_task("snapshot_backend_rt"),
                "realtime backend must start");

    std::promise<void> realtime_ran;
    TEST_ASSERT(executor.push_realtime_task("snapshot_backend_rt", [&realtime_ran] {
                    realtime_ran.set_value();
                }),
                "running realtime backend must accept a task");
    realtime_ran.get_future().wait();
    const auto running = executor.get_snapshot();
    const auto realtime = running.realtime.find("snapshot_backend_rt");
    TEST_ASSERT(realtime != running.realtime.end() && realtime->second.is_running,
                "snapshot must preserve realtime running state");

    executor.stop_realtime_task("snapshot_backend_rt");
    TEST_ASSERT(!executor.push_realtime_task("snapshot_backend_rt", [] {}),
                "stopped realtime backend must reject a task");
    const auto dropped = executor.get_snapshot();
    TEST_ASSERT(dropped.dropped_work_count >= 1,
                "realtime drops must contribute to snapshot aggregate counters");

    BlockingIoConfig blocking_config;
    blocking_config.thread_name = "snapshot-backend-io";
    TEST_ASSERT(executor.register_blocking_io_worker(
                    "snapshot_backend_io", blocking_config,
                    std::make_unique<IdleBlockingWorker>()),
                "Blocking I/O backend registration must succeed");
    TEST_ASSERT(executor.start_blocking_io_worker("snapshot_backend_io"),
                "Blocking I/O backend must start");
    const auto io_running = executor.get_snapshot();
    TEST_ASSERT(io_running.blocking_io.at("snapshot_backend_io").is_running,
                "snapshot must preserve Blocking I/O running state");
    executor.stop_blocking_io_worker("snapshot_backend_io");
    const auto io_stopped = executor.get_snapshot();
    TEST_ASSERT(!io_stopped.blocking_io.at("snapshot_backend_io").is_running,
                "snapshot must preserve Blocking I/O stop state");
    executor.shutdown();
    return true;
}

bool test_in_flight_capacity_is_bounded_under_concurrent_submission() {
    Executor executor;
    ExecutorConfig config;
    config.min_threads = 4;
    config.max_threads = 4;
    config.queue_capacity = 256;
    config.enable_work_stealing = false;
    TEST_ASSERT(executor.initialize(config), "executor initialization must succeed");
    executor.set_in_flight_task_capacity(8);

    auto release = std::make_shared<std::promise<void>>();
    const auto release_future = release->get_future().share();
    std::mutex futures_mutex;
    std::vector<std::future<void>> futures;
    std::vector<std::thread> producers;
    for (size_t producer = 0; producer < 4; ++producer) {
        producers.emplace_back([&] {
            for (size_t index = 0; index < 32; ++index) {
                auto future = executor.submit([release_future] { release_future.wait(); });
                std::lock_guard<std::mutex> lock(futures_mutex);
                futures.push_back(std::move(future));
            }
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }

    const auto snapshot = executor.get_snapshot();
    TEST_ASSERT(snapshot.in_flight_count <= 8,
                "in-flight diagnostics must remain within their configured capacity");
    TEST_ASSERT(snapshot.in_flight_diagnostics_incomplete,
                "capacity pressure must mark the sampled table incomplete");
    TEST_ASSERT(snapshot.in_flight_dropped_count >= 120,
                "capacity pressure must count omitted diagnostics");

    release->set_value();
    for (auto& future : futures) {
        future.get();
    }
    executor.shutdown();
    return true;
}

bool test_monitor_includes_registered_gpu_backend() {
    ExecutorManager manager;
    auto gpu_executor = std::make_unique<SnapshotMockGpuExecutor>("snapshot_gpu");
    TEST_ASSERT(gpu_executor->start(), "mock GPU backend must start");
    TEST_ASSERT(manager.register_gpu_executor("snapshot_gpu", std::move(gpu_executor)),
                "mock GPU backend registration must succeed");

    std::atomic<ExecutorLifecycleState> lifecycle{ExecutorLifecycleState::Running};
    monitor::ExecutorMonitor monitor(
        manager, lifecycle,
        [] { return CompletionStatus{}; },
        [] { return ExecutorFailureStatus{}; },
        [] { return std::vector<ExecutorFailureEvent>{}; },
        [] { return std::map<std::string, TaskStatistics>{}; });
    const auto snapshot = monitor.collect();

    const auto gpu = snapshot.gpu.find("snapshot_gpu");
    TEST_ASSERT(gpu != snapshot.gpu.end(),
                "snapshot must include the registered GPU backend");
    TEST_ASSERT(gpu->second.is_running,
                "snapshot must preserve GPU running state");
    TEST_ASSERT(snapshot.active_task_count == 2 && snapshot.queued_task_count == 3,
                "GPU work counters must contribute to aggregate counters");
    TEST_ASSERT(snapshot.failed_task_count == 1,
                "GPU failed kernels must contribute to aggregate counters");
    manager.shutdown();
    return true;
}

bool test_snapshot_marks_persistent_epoch_changes_partial() {
    ExecutorManager manager;
    std::atomic<ExecutorLifecycleState> lifecycle{ExecutorLifecycleState::Running};
    std::atomic<unsigned> registrations{0};
    monitor::ExecutorMonitor monitor(
        manager, lifecycle,
        [&manager, &registrations] {
            const auto index = registrations.fetch_add(1, std::memory_order_relaxed);
            auto backend = std::make_unique<SnapshotMockGpuExecutor>(
                "epoch_gpu_" + std::to_string(index));
            (void)manager.register_gpu_executor(
                "epoch_gpu_" + std::to_string(index), std::move(backend));
            return CompletionStatus{};
        },
        [] { return ExecutorFailureStatus{}; },
        [] { return std::vector<ExecutorFailureEvent>{}; },
        [] { return std::map<std::string, TaskStatistics>{}; });

    const auto snapshot = monitor.collect();
    TEST_ASSERT(snapshot.partial,
                "persistent Manager changes during collection must mark snapshot partial");
    TEST_ASSERT(snapshot.consistency_note.find("epoch_changed") != std::string::npos,
                "partial snapshot must identify exhausted epoch retries");
    TEST_ASSERT(snapshot.state_epoch == manager.get_state_epoch(),
                "snapshot must report the final observed Manager epoch");
    TEST_ASSERT(registrations.load(std::memory_order_relaxed) == 3,
                "Monitor must use its bounded epoch retry budget");
    manager.shutdown();
    return true;
}

bool test_snapshot_is_safe_during_shutdown() {
    Executor executor;
    ExecutorConfig config;
    config.min_threads = 1;
    config.max_threads = 1;
    TEST_ASSERT(executor.initialize(config), "executor initialization must succeed");

    std::atomic<bool> stop{false};
    std::thread reader([&executor, &stop]() {
        while (!stop.load(std::memory_order_acquire)) {
            (void)executor.get_snapshot();
        }
    });

    executor.shutdown();
    stop.store(true, std::memory_order_release);
    reader.join();

    TEST_ASSERT(executor.get_snapshot().lifecycle == ExecutorLifecycleState::Stopped,
                "snapshot must remain usable after shutdown");
    return true;
}

bool test_snapshot_observes_draining() {
    Executor executor;
    ExecutorConfig config;
    config.min_threads = 1;
    config.max_threads = 1;
    TEST_ASSERT(executor.initialize(config), "executor initialization must succeed");

    std::promise<void> release;
    auto gate = release.get_future().share();
    std::promise<void> started;
    auto task = executor.submit([gate, &started]() {
        started.set_value();
        gate.wait();
    });
    started.get_future().wait();

    std::atomic<bool> shutdown_complete{false};
    std::thread shutdown_thread([&executor, &shutdown_complete]() {
        executor.shutdown();
        shutdown_complete.store(true, std::memory_order_release);
    });

    bool observed_draining = false;
    for (int attempt = 0; attempt < 100 && !shutdown_complete.load(std::memory_order_acquire);
         ++attempt) {
        observed_draining |= executor.get_snapshot().lifecycle == ExecutorLifecycleState::Draining;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    release.set_value();
    task.get();
    shutdown_thread.join();

    TEST_ASSERT(observed_draining,
                "snapshot must expose Draining while shutdown waits for accepted work");
    return true;
}

bool test_snapshot_concurrent_registration_stop_and_query() {
    Executor executor;
    std::atomic<bool> keep_querying{true};
    std::thread reader([&executor, &keep_querying]() {
        while (keep_querying.load(std::memory_order_acquire)) {
            (void)executor.get_snapshot();
        }
    });

    std::thread registrar([&executor]() {
        for (int i = 0; i < 32; ++i) {
            RealtimeThreadConfig config;
            config.thread_name = "snapshot-race-" + std::to_string(i);
            config.cycle_period_ns = 1'000'000;
            (void)executor.register_realtime_task("snapshot_race_" + std::to_string(i), config);
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    executor.shutdown(false);
    registrar.join();
    keep_querying.store(false, std::memory_order_release);
    reader.join();

    TEST_ASSERT(executor.get_snapshot().lifecycle == ExecutorLifecycleState::Stopped,
                "snapshot must remain valid after concurrent registration and shutdown");
    return true;
}

bool test_snapshot_safe_with_concurrent_shared_ownership_destruction() {
    auto executor = std::make_shared<Executor>();
    std::weak_ptr<Executor> weak_executor = executor;
    std::atomic<bool> keep_querying{true};
    std::atomic<uint64_t> snapshots{0};
    std::thread reader([&weak_executor, &keep_querying, &snapshots]() {
        while (keep_querying.load(std::memory_order_acquire)) {
            if (auto current = weak_executor.lock()) {
                (void)current->get_snapshot();
                snapshots.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    executor.reset();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    keep_querying.store(false, std::memory_order_release);
    reader.join();

    TEST_ASSERT(snapshots.load(std::memory_order_relaxed) != 0,
                "reader must successfully collect snapshots before destruction");
    TEST_ASSERT(weak_executor.expired(),
                "executor must be destroyed after the reader releases its last snapshot owner");
    return true;
}

int main() {
    bool success = true;
    success &= test_snapshot_does_not_lazy_initialize();
    success &= test_snapshot_text_is_stable_and_complete();
    success &= test_snapshot_text_handles_partial_providers();
    success &= test_snapshot_marks_persistent_epoch_changes_partial();
    success &= test_snapshot_diagnostic_callback_on_timeout_and_start_failure();
    success &= test_snapshot_reports_async_work_and_shutdown();
    success &= test_snapshot_reports_bounded_in_flight_tasks();
    success &= test_in_flight_diagnostics_do_not_change_soft_timeout();
    success &= test_snapshot_reports_failure_and_failed_initialization();
    success &= test_snapshot_includes_registered_backends();
    success &= test_snapshot_uses_backend_specific_work_states();
    success &= test_monitor_includes_registered_gpu_backend();
    success &= test_in_flight_capacity_is_bounded_under_concurrent_submission();
    success &= test_snapshot_is_safe_during_shutdown();
    success &= test_snapshot_observes_draining();
    success &= test_snapshot_concurrent_registration_stop_and_query();
    success &= test_snapshot_safe_with_concurrent_shared_ownership_destruction();
    return success ? 0 : 1;
}
