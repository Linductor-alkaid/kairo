#include <kairo/executor.hpp>

#include <atomic>
#include <iostream>
#include <stdexcept>
#include <string>

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

bool future_throws(std::future<void> future) {
    try {
        future.get();
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

bool test_plain_submit_auto_uses_default_async_executor() {
    Executor executor;
    auto future = executor.submit_auto([] { return 42; });
    TEST_ASSERT(future.get() == 42, "plain submit_auto should fulfill CPU future");
    const auto decision = executor.get_last_routing_decision();
    TEST_ASSERT(decision.has_value(), "plain submit_auto should record a routing decision");
    TEST_ASSERT(decision->selected_backend == ExecutionBackend::DefaultAsync,
                "plain submit_auto should select default async");
    TEST_ASSERT(decision->reason == RoutingReason::DefaultPolicy,
                "plain submit_auto should use default policy");
    executor.shutdown();
    return true;
}

bool test_unavailable_gpu_allow_cpu_falls_back() {
    Executor executor;
    std::atomic<int> cpu_runs{0};
    std::atomic<int> gpu_runs{0};
    auto future = executor.submit_auto(
        cpu_gpu_task(
            [&] { ++cpu_runs; },
            [&](void*) { ++gpu_runs; })
            .name("fallback-cpu")
            .preferred_executor("missing-gpu")
            .fallback(FallbackPolicy::AllowCpu)
            .prefer_gpu());
    future.get();

    TEST_ASSERT(cpu_runs == 1, "AllowCpu should execute CPU path when GPU is unavailable");
    TEST_ASSERT(gpu_runs == 0, "unavailable GPU path must not execute");
    TEST_ASSERT(executor.get_failure_status().submit_rejected_count == 0,
                "allowed CPU fallback is not a submission failure");
    const auto decision = executor.get_last_routing_decision();
    TEST_ASSERT(decision.has_value() && decision->fell_back,
                "CPU fallback should be recorded as a fallback decision");
    TEST_ASSERT(decision->reason == RoutingReason::FallbackPolicy,
                "CPU fallback should state the fallback policy");
    executor.shutdown();
    return true;
}

bool test_unavailable_gpu_without_fallback_rejects() {
    Executor executor;
    auto future = executor.submit_auto(
        cpu_gpu_task([] {}, [](void*) {})
            .name("reject-missing-gpu")
            .preferred_executor("missing-gpu")
            .prefer_gpu());
    TEST_ASSERT(future_throws(std::move(future)),
                "NoFallback should reject unavailable GPU");
    TEST_ASSERT(executor.get_failure_status().submit_rejected_count == 1,
                "rejected automatic submission should be observable");
    executor.shutdown();
    return true;
}

bool test_require_requested_backend_requires_running_gpu() {
    Executor executor;
    auto future = executor.submit_auto(
        cpu_gpu_task([] {}, [](void*) {})
            .name("require-gpu")
            .preferred_executor("missing-gpu")
            .fallback(FallbackPolicy::RequireRequestedBackend));
    TEST_ASSERT(future_throws(std::move(future)),
                "required GPU backend should reject when unavailable");
    TEST_ASSERT(executor.get_failure_status().submit_rejected_count == 1,
                "required backend rejection should be observable");
    executor.shutdown();
    return true;
}

bool test_unsupported_generic_intent_rejects() {
    Executor executor;
    auto future = executor.submit_auto(task([] {}).intent(ExecutionIntent::LowLatency));
    TEST_ASSERT(future_throws(std::move(future)),
                "generic submit_auto should reject LowLatency intent");
    TEST_ASSERT(executor.get_failure_status().submit_rejected_count == 1,
                "typed API rejection should be observable");
    const auto decision = executor.get_last_routing_decision();
    TEST_ASSERT(decision.has_value() && decision->reason == RoutingReason::Rejected,
                "unsupported intent should record a rejected decision");
    executor.shutdown();
    return true;
}

bool test_routing_callback_and_buffer_are_isolated() {
    Executor executor;
    int callback_count = 0;
    executor.set_routing_callback([&](const RoutingDecision&) {
        ++callback_count;
        throw std::runtime_error("routing observer failure");
    });
    executor.set_recent_routing_capacity(2);

    TEST_ASSERT(executor.submit_auto([] { return 1; }).get() == 1,
                "callback exception must not affect first task");
    TEST_ASSERT(executor.submit_auto([] { return 2; }).get() == 2,
                "callback exception must not affect second task");
    TEST_ASSERT(executor.submit_auto([] { return 3; }).get() == 3,
                "callback exception must not affect third task");
    TEST_ASSERT(callback_count == 3, "routing callback should receive every decision");
    TEST_ASSERT(executor.get_recent_routing_decisions().size() == 2,
                "routing buffer should retain its configured capacity");

    executor.set_recent_routing_capacity(0);
    TEST_ASSERT(executor.submit_auto([] {}).wait_for(std::chrono::seconds(1)) ==
                    std::future_status::ready,
                "routing callback remains active when retention is disabled");
    TEST_ASSERT(callback_count == 4, "callback remains active with zero retention capacity");
    TEST_ASSERT(executor.get_recent_routing_decisions().empty(),
                "zero routing capacity should disable retention");
    executor.shutdown();
    return true;
}

bool test_routing_buffer_order_clear_and_failure_separation() {
    Executor executor;
    executor.set_recent_routing_capacity(3);
    executor.submit_auto(task([] {}).name("first")).get();
    executor.submit_auto(task([] {}).name("second")).get();

    const auto decisions = executor.get_recent_routing_decisions();
    TEST_ASSERT(decisions.size() == 2 && decisions[0].task_name == "first" &&
                    decisions[1].task_name == "second",
                "routing decisions should retain chronological order");
    TEST_ASSERT(executor.get_failure_status().total_count == 0,
                "successful routing decisions must not become failure events");

    executor.clear_recent_routing_decisions();
    TEST_ASSERT(executor.get_recent_routing_decisions().empty(),
                "clearing routing decisions must only clear the routing buffer");
    executor.shutdown();
    return true;
}

}  // namespace

int main() {
    bool passed = true;
    passed &= test_plain_submit_auto_uses_default_async_executor();
    passed &= test_unavailable_gpu_allow_cpu_falls_back();
    passed &= test_unavailable_gpu_without_fallback_rejects();
    passed &= test_require_requested_backend_requires_running_gpu();
    passed &= test_unsupported_generic_intent_rejects();
    passed &= test_routing_callback_and_buffer_are_isolated();
    passed &= test_routing_buffer_order_clear_and_failure_separation();
    return passed ? 0 : 1;
}
