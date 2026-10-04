#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "kairo/thread_pool/load_balancer.hpp"

namespace kairo {
namespace {

TEST(LoadBalancerConcurrentResizeTest, RoundRobinAndUpdateLoadRaceResize) {
    LoadBalancer balancer(4);
    balancer.set_strategy(LoadBalancer::Strategy::ROUND_ROBIN);

    std::atomic<bool> start{false};
    std::atomic<bool> stop{false};
    std::vector<std::thread> workers;

    for (size_t thread_id = 0; thread_id < 4; ++thread_id) {
        workers.emplace_back([&, thread_id]() {
            while (!start.load(std::memory_order_acquire)) {
            }
            size_t iteration = thread_id;
            while (!stop.load(std::memory_order_acquire)) {
                (void)balancer.select_worker();
                balancer.update_load(iteration % 8, iteration, thread_id);
                ++iteration;
            }
        });
    }

    std::thread resizer([&]() {
        while (!start.load(std::memory_order_acquire)) {
        }
        for (size_t iteration = 0; iteration < 4000; ++iteration) {
            balancer.resize(iteration % 9);
        }
        stop.store(true, std::memory_order_release);
    });

    start.store(true, std::memory_order_release);
    resizer.join();
    for (auto& worker : workers) {
        worker.join();
    }

    const auto loads = balancer.get_all_loads();
    EXPECT_LT(loads.size(), 9U);
}

} // namespace
} // namespace kairo
