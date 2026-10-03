#include <iostream>
#include <stdexcept>

#include <kairo/executor.hpp>

int main() {
    kairo::Executor executor;
    kairo::gpu::GpuExecutorConfig config;
    config.name = "tutorial_gpu";
    config.backend = kairo::gpu::GpuBackend::SYCL;

    const auto registration = executor.register_gpu_executor_ex("tutorial_gpu", config);
    bool submit_rejected = false;
    try {
        kairo::gpu::GpuTaskConfig task_config;
        auto future = executor.submit_gpu("tutorial_gpu", [] {}, task_config);
        future.get();
    } catch (const std::runtime_error&) {
        submit_rejected = true;
    }

    const auto status = executor.get_failure_status();
    std::cout << "gpu backend=" << (registration ? "available" : "unavailable")
              << ", submit=" << (submit_rejected ? "diagnosed" : "unexpected")
              << ", failures=" << status.total_count << '\n';
    executor.shutdown();
    return !registration && submit_rejected && status.total_count >= 2 ? 0 : 1;
}
