#include <iostream>
#include <string>

#include <kairo/executor.hpp>

using namespace kairo;

#define TEST_ASSERT(condition, message)                                      \
    do {                                                                    \
        if (!(condition)) {                                                  \
            std::cerr << "FAILED: " << message << " at " << __FILE__      \
                      << ":" << __LINE__ << std::endl;                     \
            return false;                                                    \
        }                                                                   \
    } while (0)

namespace {

RealtimeThreadConfig make_rt_config(const std::string& thread_name) {
    RealtimeThreadConfig config;
    config.thread_name = thread_name;
    config.cycle_period_ns = 5'000'000;
    config.thread_priority = 0;
    config.enable_process_memory_lock = false;
    config.timer_slack_ns = 0;
    config.cycle_callback = []() {};
    return config;
}

gpu::GpuExecutorConfig make_gpu_config(const std::string& name) {
    gpu::GpuExecutorConfig config;
    config.name = name;
    config.backend = gpu::GpuBackend::SYCL;
    config.device_id = 0;
    config.max_queue_size = 16;
    config.default_stream_count = 1;
    return config;
}

bool recent_message_contains(const Executor& executor, const std::string& needle) {
    auto recent = executor.get_recent_failures(1);
    return recent.size() == 1 && recent[0].message.find(needle) != std::string::npos;
}

}  // namespace

static bool test_initialize_ex_reports_specific_reasons() {
    std::cout << "Testing initialize diagnostics..." << std::endl;

    Executor invalid_executor;
    ExecutorConfig invalid_config;
    invalid_config.min_threads = 4;
    invalid_config.max_threads = 2;
    auto invalid = invalid_executor.initialize(invalid_config);
    TEST_ASSERT(!invalid.ok, "invalid config should fail");
    TEST_ASSERT(invalid.error_code == ExecutorErrorCode::InvalidConfig,
                "invalid config should return InvalidConfig");
    TEST_ASSERT(recent_message_contains(invalid_executor, "InvalidConfig"),
                "invalid initialize should record diagnostic event");

    Executor executor;
    ExecutorConfig config;
    config.min_threads = 1;
    config.max_threads = 1;
    auto first = executor.initialize(config);
    TEST_ASSERT(first.ok, "first initialize should succeed");

    auto duplicate = executor.initialize(config);
    TEST_ASSERT(!duplicate.ok, "second initialize should fail");
    TEST_ASSERT(duplicate.error_code == ExecutorErrorCode::AlreadyInitialized,
                "second initialize should report AlreadyInitialized");

    executor.shutdown();
    auto after_shutdown = executor.initialize(config);
    TEST_ASSERT(!after_shutdown.ok, "initialize after shutdown should fail");
    TEST_ASSERT(after_shutdown.error_code == ExecutorErrorCode::AlreadyShutdown,
                "initialize after shutdown should report AlreadyShutdown");

    auto status = executor.get_failure_status();
    TEST_ASSERT(status.submit_rejected_count == 2,
                "duplicate and after-shutdown initialize should be visible");
    TEST_ASSERT(status.task_exception_count == 0,
                "diagnostic failures must not count as task exceptions");

    std::cout << "  initialize diagnostics: PASSED" << std::endl;
    return true;
}

static bool test_realtime_registration_and_start_diagnostics() {
    std::cout << "Testing realtime _ex diagnostics..." << std::endl;

    Executor executor;

    auto invalid = executor.register_realtime_task(
        "bad_rt", RealtimeThreadConfig{});
    TEST_ASSERT(!invalid.ok, "invalid realtime config should fail");
    TEST_ASSERT(invalid.error_code == ExecutorErrorCode::InvalidConfig,
                "invalid realtime config should report InvalidConfig");
    TEST_ASSERT(recent_message_contains(executor, "InvalidConfig"),
                "invalid realtime registration should record diagnostic event");

    auto missing_start = executor.start_realtime_task("missing_rt");
    TEST_ASSERT(!missing_start.ok, "starting missing realtime executor should fail");
    TEST_ASSERT(missing_start.error_code == ExecutorErrorCode::NotFound,
                "missing realtime start should report NotFound");

    auto registered = executor.register_realtime_task(
        "rt_diag", make_rt_config("rt_diag_thread"));
    TEST_ASSERT(registered.ok, "valid realtime registration should succeed");

    auto duplicate = executor.register_realtime_task(
        "rt_diag", make_rt_config("rt_diag_duplicate_thread"));
    TEST_ASSERT(!duplicate.ok, "duplicate realtime name should fail");
    TEST_ASSERT(duplicate.error_code == ExecutorErrorCode::DuplicateName,
                "duplicate realtime name should report DuplicateName");

    auto started = executor.start_realtime_task("rt_diag");
    TEST_ASSERT(started.ok, "valid realtime executor should start");

    auto started_again = executor.start_realtime_task("rt_diag");
    TEST_ASSERT(!started_again.ok, "starting realtime twice should fail");
    TEST_ASSERT(started_again.error_code == ExecutorErrorCode::AlreadyInitialized,
                "starting realtime twice should report AlreadyInitialized");

    executor.stop_realtime_task("rt_diag");

    auto status = executor.get_failure_status();
    TEST_ASSERT(status.submit_rejected_count == 4,
                "realtime diagnostic failures should be visible");
    TEST_ASSERT(status.task_exception_count == 0,
                "realtime diagnostic failures must not count as task exceptions");

    std::cout << "  realtime _ex diagnostics: PASSED" << std::endl;
    return true;
}

static bool test_gpu_registration_diagnostics() {
    std::cout << "Testing GPU _ex diagnostics..." << std::endl;

    Executor executor;

    auto invalid_config = make_gpu_config("");
    auto invalid = executor.register_gpu_executor("gpu_invalid", invalid_config);
    TEST_ASSERT(!invalid.ok, "invalid GPU config should fail");
    TEST_ASSERT(invalid.error_code == ExecutorErrorCode::InvalidConfig,
                "invalid GPU config should report InvalidConfig");
    TEST_ASSERT(recent_message_contains(executor, "InvalidConfig"),
                "invalid GPU registration should record diagnostic event");

    auto unavailable = executor.register_gpu_executor(
        "gpu_sycl", make_gpu_config("gpu_sycl"));
    TEST_ASSERT(!unavailable.ok, "unimplemented GPU backend should fail");
    TEST_ASSERT(unavailable.error_code == ExecutorErrorCode::BackendUnavailable,
                "unimplemented GPU backend should report BackendUnavailable");
    TEST_ASSERT(recent_message_contains(executor, "BackendUnavailable"),
                "unavailable GPU backend should record diagnostic event");

    auto status = executor.get_failure_status();
    TEST_ASSERT(status.gpu_failure_count == 2,
                "GPU diagnostic failures should be counted as GPU failures");
    TEST_ASSERT(status.task_exception_count == 0,
                "GPU diagnostic failures must not count as task exceptions");

    std::cout << "  GPU _ex diagnostics: PASSED" << std::endl;
    return true;
}

int main() {
    std::cout << "========== Executor result diagnostics tests ==========\n\n";

    bool all_passed = true;
    all_passed &= test_initialize_ex_reports_specific_reasons();
    all_passed &= test_realtime_registration_and_start_diagnostics();
    all_passed &= test_gpu_registration_diagnostics();

    std::cout << "\n=======================================================\n";
    if (all_passed) {
        std::cout << "All executor result diagnostics tests PASSED!" << std::endl;
        return 0;
    }

    std::cout << "Some executor result diagnostics tests FAILED!" << std::endl;
    return 1;
}
