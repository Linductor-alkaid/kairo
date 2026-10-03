#include <iostream>
#include <stdexcept>
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

gpu::GpuTaskConfig make_task_config() {
    gpu::GpuTaskConfig config;
    return config;
}

bool submit_missing_gpu(Executor& executor, const std::string& executor_name) {
    try {
        auto future = executor.submit_gpu(
            executor_name,
            []() {},
            make_task_config());
        (void)future;
    } catch (const std::runtime_error&) {
        return true;
    }

    return false;
}

}  // namespace

static bool test_submit_gpu_missing_executor_records_failure() {
    std::cout << "Testing submit_gpu missing executor records failure..."
              << std::endl;

    Executor executor;
    const std::string executor_name = "missing_gpu_submit_records_failure";

    TEST_ASSERT(submit_missing_gpu(executor, executor_name),
                "submit_gpu to missing executor should throw runtime_error");

    auto status = executor.get_failure_status();
    TEST_ASSERT(status.submit_rejected_count >= 1,
                "missing GPU executor should increment submit_rejected_count");
    TEST_ASSERT(status.gpu_failure_count == 0,
                "missing GPU executor submission should not count as backend GPU failure");

    auto recent = executor.get_recent_failures(1);
    TEST_ASSERT(recent.size() == 1, "recent failure should record missing GPU submit");
    TEST_ASSERT(recent[0].executor_name == executor_name,
                "recent failure should retain requested GPU executor name");
    TEST_ASSERT(recent[0].message.find(executor_name) != std::string::npos,
                "recent failure message should contain requested GPU executor name");

    std::cout << "  submit_gpu missing executor records failure: PASSED"
              << std::endl;
    return true;
}

static bool test_submit_gpu_missing_executor_recent_failure_message() {
    std::cout << "Testing submit_gpu missing executor recent failure message..."
              << std::endl;

    Executor executor;
    const std::string executor_name = "missing_gpu_recent_failure_name";

    TEST_ASSERT(submit_missing_gpu(executor, executor_name),
                "submit_gpu to missing executor should throw runtime_error");

    auto recent = executor.get_recent_failures(1);
    TEST_ASSERT(recent.size() == 1, "recent failure should include missing GPU submit");
    TEST_ASSERT(recent[0].message.find(executor_name) != std::string::npos,
                "most recent failure message should contain requested GPU executor name");

    std::cout << "  submit_gpu missing executor recent failure message: PASSED"
              << std::endl;
    return true;
}

int main() {
    bool all_passed = true;
    all_passed &= test_submit_gpu_missing_executor_records_failure();
    all_passed &= test_submit_gpu_missing_executor_recent_failure_message();

    if (all_passed) {
        std::cout << "All submit_gpu missing executor failure tests passed."
                  << std::endl;
        return 0;
    }

    std::cerr << "Some submit_gpu missing executor failure tests failed."
              << std::endl;
    return 1;
}
