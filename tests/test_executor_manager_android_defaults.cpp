#include <kairo/config.hpp>
#include <kairo/executor_manager.hpp>

#include <future>
#include <iostream>

#if defined(__ANDROID__)
#define private public
#include "kairo/thread_pool_executor.hpp"
#undef private
#include "kairo/thread_pool/thread_pool.hpp"
#include "kairo/util/thread_utils.hpp"
#endif

#define TEST_ASSERT(condition, message)                                      \
    do {                                                                     \
        if (!(condition)) {                                                  \
            std::cerr << "FAILED: " << message << " at " << __FILE__      \
                      << ':' << __LINE__ << '\n';                          \
            return false;                                                    \
        }                                                                    \
    } while (0)

#if defined(__ANDROID__)
bool test_android_default_async_executor_budget() {
    kairo::ExecutorManager manager;
    kairo::ExecutorConfig config;
    config.enable_monitoring = false;

    TEST_ASSERT(manager.initialize_async_executor(config),
                "default async executor should initialize on Android");

    kairo::IAsyncExecutor* async = manager.get_default_async_executor();
    TEST_ASSERT(async != nullptr, "initialized manager should expose async executor");

    auto* thread_pool_executor = dynamic_cast<kairo::ThreadPoolExecutor*>(async);
    TEST_ASSERT(thread_pool_executor != nullptr,
                "default async executor should be a ThreadPoolExecutor");
    const auto pool = thread_pool_executor->thread_pool_;
    TEST_ASSERT(pool != nullptr, "thread pool should be created after start");

    const kairo::ThreadPoolStatus pool_status = pool->get_status();
    TEST_ASSERT(pool_status.total_threads > 0,
                "Android default thread pool should have started workers");
    TEST_ASSERT(pool_status.total_threads <= 4,
                "Android default thread pool should not exceed 4 workers");

    auto future = async->submit([] { return 42; });
    TEST_ASSERT(future.get() == 42, "submission should complete after Android default start");

    manager.shutdown(true);
    return true;
}
#endif

int main() {
#if defined(__ANDROID__)
    const bool ok = test_android_default_async_executor_budget();
    std::cout << (ok ? "All Android executor manager default tests PASSED\n"
                     : "Android executor manager default tests FAILED\n");
    return ok ? 0 : 1;
#else
    std::cout << "Android executor manager default tests skipped (desktop build)\n";
    return 0;
#endif
}
