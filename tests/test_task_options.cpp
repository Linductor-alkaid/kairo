#include <kairo/executor.hpp>

#include <chrono>
#include <iostream>
#include <string>
#include <type_traits>

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

bool test_task_options_defaults() {
    TaskOptions options;

    TEST_ASSERT(options.name.empty(), "name should default to empty");
    TEST_ASSERT(options.priority == TaskPriority::NORMAL,
                "priority should default to normal");
    TEST_ASSERT(options.intent == ExecutionIntent::Auto,
                "intent should default to auto");
    TEST_ASSERT(!options.preferred_executor.has_value(),
                "preferred executor should be unset");
    TEST_ASSERT(options.fallback == FallbackPolicy::NoFallback,
                "fallback should default to no fallback");
    TEST_ASSERT(!options.deadline.has_value(), "deadline should be unset");
    return true;
}

bool test_task_builder_preserves_callable_and_options() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    auto configured = task([value = 41] { return value + 1; })
                          .name("decode-frame")
                          .priority(TaskPriority::HIGH)
                          .intent(ExecutionIntent::GeneralCpu)
                          .preferred_executor("cpu-main")
                          .fallback(FallbackPolicy::AllowCpu)
                          .deadline(deadline);

    static_assert(std::is_lvalue_reference_v<decltype(configured.function())>);
    const auto& options = configured.options();
    TEST_ASSERT(options.name == "decode-frame", "builder should set name");
    TEST_ASSERT(options.priority == TaskPriority::HIGH, "builder should set priority");
    TEST_ASSERT(options.intent == ExecutionIntent::GeneralCpu, "builder should set intent");
    TEST_ASSERT(options.preferred_executor == "cpu-main",
                "builder should set preferred executor");
    TEST_ASSERT(options.fallback == FallbackPolicy::AllowCpu,
                "builder should set fallback policy");
    TEST_ASSERT(options.deadline == deadline, "builder should set deadline");
    TEST_ASSERT(configured.function()() == 42, "builder should preserve callable");
    return true;
}

bool test_public_enums_are_available_from_facade_header() {
    const auto backend = ExecutionBackend::DefaultAsync;
    const auto reason = RoutingReason::DefaultPolicy;
    TEST_ASSERT(backend == ExecutionBackend::DefaultAsync,
                "execution backend should be available from facade header");
    TEST_ASSERT(reason == RoutingReason::DefaultPolicy,
                "routing reason should be available from facade header");
    return true;
}

bool test_cpu_gpu_task_builder_preserves_both_paths() {
    bool cpu_called = false;
    bool gpu_called = false;
    auto configured = cpu_gpu_task(
                          [&cpu_called] { cpu_called = true; },
                          [&gpu_called](void*) { gpu_called = true; })
                          .name("segment")
                          .priority(TaskPriority::CRITICAL)
                          .preferred_executor("cuda0")
                          .fallback(FallbackPolicy::AllowCpu)
                          .data_size(2 * 1024 * 1024)
                          .compute_intensity(3.0F)
                          .prefer_gpu();

    TEST_ASSERT(configured.options().intent == ExecutionIntent::CpuOrGpu,
                "CPU/GPU task should always declare CpuOrGpu intent");
    TEST_ASSERT(configured.options().priority == TaskPriority::CRITICAL,
                "CPU/GPU task should retain task priority");
    TEST_ASSERT(configured.gpu_config().priority == 3,
                "CPU/GPU task should map priority to GPU config");
    TEST_ASSERT(configured.characteristics().data_size_bytes == 2 * 1024 * 1024,
                "CPU/GPU task should retain data size");
    TEST_ASSERT(configured.characteristics().compute_intensity == 3.0F,
                "CPU/GPU task should retain compute intensity");
    TEST_ASSERT(configured.characteristics().prefer_gpu,
                "CPU/GPU task should retain GPU preference");

    configured.take_cpu()();
    configured.take_gpu()(nullptr);
    TEST_ASSERT(cpu_called, "CPU path should remain independently callable");
    TEST_ASSERT(gpu_called, "GPU path should remain independently callable");
    return true;
}

}  // namespace

int main() {
    bool passed = true;
    passed &= test_task_options_defaults();
    passed &= test_task_builder_preserves_callable_and_options();
    passed &= test_public_enums_are_available_from_facade_header();
    passed &= test_cpu_gpu_task_builder_preserves_both_paths();

    if (!passed) {
        return 1;
    }

    std::cout << "Task options tests passed" << std::endl;
    return 0;
}
