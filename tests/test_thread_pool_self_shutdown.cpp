#include "kairo/thread_pool/thread_pool.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <future>

namespace {

using namespace std::chrono_literals;

TEST(ThreadPoolShutdown, ShutdownFromWorkerDoesNotDeadlock) {
    kairo::ThreadPool pool;
    kairo::ThreadPoolConfig config;
    config.min_threads = 1;
    config.max_threads = 1;
    config.enable_work_stealing = false;
    ASSERT_TRUE(pool.initialize(config));

    auto worker_result = pool.submit([&pool]() {
        const auto first = pool.shutdown(false);
        const auto second = pool.shutdown(true);
        return first == kairo::ShutdownResult::RequestedFromWorker &&
               second == kairo::ShutdownResult::RequestedFromWorker;
    });

    ASSERT_EQ(worker_result.wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(worker_result.get());

    auto join_result = std::async(std::launch::async, [&pool]() {
        return pool.shutdown(true);
    });
    ASSERT_EQ(join_result.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(join_result.get(), kairo::ShutdownResult::Completed);

    EXPECT_TRUE(pool.is_stopped());
    EXPECT_FALSE(pool.try_submit([] {}));
}

} // namespace
