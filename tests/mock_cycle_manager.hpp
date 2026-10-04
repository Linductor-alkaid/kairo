#pragma once

#include <kairo/interfaces.hpp>
#include <kairo/types.hpp>
#include <chrono>
#include <atomic>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>

namespace kairo {
namespace test {

/**
 * @brief Mock ICycleManager for integration tests
 *
 * Implements ICycleManager using sleep_until for cycle timing.
 * start_cycle() blocks in the calling thread; stop_cycle() can be called
 * from another thread to signal exit. Does not hold mutex during the loop
 * to avoid deadlock with stop_cycle().
 */
class MockCycleManager : public kairo::ICycleManager {
public:
    struct CycleInfo {
        std::string name;
        int64_t period_ns = 0;
        std::function<void()> callback;
    };

    /** If true, register_cycle returns false (for fallback testing). */
    bool fail_register = false;

    /** If true, start_cycle returns false (for fallback testing). */
    bool fail_start = false;

    bool throw_register = false;
    bool throw_start = false;
    bool throw_stop = false;

    bool register_cycle(const std::string& name, int64_t period_ns,
                       std::function<void()> callback) override {
        if (throw_register) {
            throw std::runtime_error("register_cycle failure");
        }
        if (fail_register) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        cycles_[name] = {name, period_ns, std::move(callback)};
        stop_requested_[name] = false;
        return true;
    }

    bool start_cycle(const std::string& name) override {
        if (throw_start) {
            throw std::runtime_error("start_cycle failure");
        }
        if (fail_start) {
            return false;
        }
        CycleInfo info;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = cycles_.find(name);
            if (it == cycles_.end()) {
                return false;
            }
            info = it->second;
            stop_requested_[name] = false;
        }

        // Run loop without holding mutex so stop_cycle() can acquire it
        auto next_cycle_time = std::chrono::steady_clock::now();
        const auto period_ns = std::chrono::nanoseconds(info.period_ns);

        while (true) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (stop_requested_[name]) {
                    break;
                }
            }

            if (info.callback) {
                info.callback();
            }

            next_cycle_time += period_ns;
            std::this_thread::sleep_until(next_cycle_time);
        }

        return true;
    }

    void stop_cycle(const std::string& name) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_requested_[name] = true;
        }
        if (throw_stop) {
            throw std::runtime_error("stop_cycle failure");
        }
    }

    CycleStatistics get_statistics(const std::string& name) const override {
        CycleStatistics stats;
        stats.name = name;
        return stats;
    }

private:
    std::unordered_map<std::string, CycleInfo> cycles_;
    std::unordered_map<std::string, bool> stop_requested_;
    mutable std::mutex mutex_;
};

}  // namespace test
}  // namespace kairo
