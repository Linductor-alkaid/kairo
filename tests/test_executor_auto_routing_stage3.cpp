#include <kairo/executor.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>

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

TaskOptions low_latency_options(const std::string& name) {
    TaskOptions options;
    options.name = "low-latency-task";
    options.intent = ExecutionIntent::LowLatency;
    options.preferred_executor = name;
    return options;
}

bool test_low_latency_dispatch_requires_running_named_executor() {
    Executor executor;
    TEST_ASSERT(executor.register_lockfree_executor(
                    "low-latency", std::make_unique<LockFreeTaskExecutor>(8)),
                "lock-free executor should register");

    auto stopped = executor.dispatch_auto(low_latency_options("low-latency"), [] {});
    TEST_ASSERT(!stopped.accepted, "stopped lock-free executor must reject dispatch");
    TEST_ASSERT(stopped.decision.reason == RoutingReason::BackendNotRunning,
                "stopped rejection should state BackendNotRunning");

    TEST_ASSERT(executor.start_lockfree_executor("low-latency"),
                "lock-free executor should start");
    std::atomic<int> executions{0};
    auto accepted = executor.dispatch_auto(low_latency_options("low-latency"), [&] {
        ++executions;
    });
    TEST_ASSERT(accepted.accepted, "running lock-free executor should accept dispatch");
    TEST_ASSERT(accepted.backend == ExecutionBackend::LockFree,
                "dispatch result should identify lock-free backend");

    executor.stop_lockfree_executor("low-latency");
    TEST_ASSERT(executions == 1, "accepted dispatch should execute before stop returns");
    executor.shutdown();
    return true;
}

bool test_dispatch_rejections_are_observable() {
    Executor executor;
    auto missing = executor.dispatch_auto(low_latency_options("missing"), [] {});
    TEST_ASSERT(!missing.accepted && missing.decision.reason == RoutingReason::BackendUnavailable,
                "missing executor should produce BackendUnavailable");

    auto invalid_options = TaskOptions{};
    auto invalid = executor.dispatch_auto(invalid_options, [] {});
    TEST_ASSERT(!invalid.accepted && invalid.decision.reason == RoutingReason::Rejected,
                "Auto must not select lock-free implicitly");

    TEST_ASSERT(executor.register_lockfree_executor(
                    "empty-check", std::make_unique<LockFreeTaskExecutor>(8)),
                "second executor should register");
    TEST_ASSERT(executor.start_lockfree_executor("empty-check"),
                "second executor should start");
    auto empty = executor.dispatch_auto(low_latency_options("empty-check"), {});
    TEST_ASSERT(!empty.accepted && empty.decision.reason == RoutingReason::Rejected,
                "empty task should be rejected");
    TEST_ASSERT(executor.get_failure_status().submit_rejected_count == 3,
                "each dispatch rejection should record a failure event");
    executor.shutdown();
    return true;
}

bool test_full_lockfree_queue_rejects_dispatch() {
    Executor executor;
    TEST_ASSERT(executor.register_lockfree_executor(
                    "bounded", std::make_unique<LockFreeTaskExecutor>(2)),
                "bounded executor should register");
    TEST_ASSERT(executor.start_lockfree_executor("bounded"), "bounded executor should start");

    std::promise<void> started;
    std::promise<void> unblock;
    auto unblock_future = unblock.get_future().share();
    auto first = executor.dispatch_auto(low_latency_options("bounded"), [&] {
        started.set_value();
        unblock_future.wait();
    });
    TEST_ASSERT(first.accepted, "first dispatch should occupy the worker");
    started.get_future().wait();
    auto queued = executor.dispatch_auto(low_latency_options("bounded"), [] {});
    TEST_ASSERT(queued.accepted, "second dispatch should occupy the bounded queue");
    auto full = executor.dispatch_auto(low_latency_options("bounded"), [] {});
    TEST_ASSERT(!full.accepted && full.decision.reason == RoutingReason::Rejected,
                "full queue should reject at actual dispatch");
    unblock.set_value();
    executor.stop_lockfree_executor("bounded");
    executor.shutdown();
    return true;
}

bool test_manager_enforces_cross_backend_lockfree_name_uniqueness() {
    Executor executor;
    TEST_ASSERT(executor.register_lockfree_executor(
                    "shared-name", std::make_unique<LockFreeTaskExecutor>(8)),
                "lock-free executor should register");
    RealtimeThreadConfig config;
    config.thread_name = "unique-test-thread";
    config.cycle_period_ns = 1'000'000;
    TEST_ASSERT(!executor.register_realtime_task("shared-name", config),
                "realtime registration must reject a lock-free name collision");
    executor.shutdown();
    return true;
}

}  // namespace

int main() {
    bool passed = true;
    passed &= test_low_latency_dispatch_requires_running_named_executor();
    passed &= test_dispatch_rejections_are_observable();
    passed &= test_full_lockfree_queue_rejects_dispatch();
    passed &= test_manager_enforces_cross_backend_lockfree_name_uniqueness();
    return passed ? 0 : 1;
}
