#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <kairo/executor.hpp>

class SimpleCycleManager : public kairo::ICycleManager {
public:
    struct CycleInfo {
        std::string name;
        int64_t period_ns = 0;
        std::function<void()> callback;
    };

    ~SimpleCycleManager() override {
        stop_all_cycles();
    }

    bool register_cycle(const std::string& name, int64_t period_ns,
                        std::function<void()> callback) override {
        std::lock_guard<std::mutex> lock(mutex_);
        cycles_[name] = {name, period_ns, std::move(callback)};
        stop_requested_[name] = false;
        return true;
    }

    bool start_cycle(const std::string& name) override {
        CycleInfo info;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto cycle = cycles_.find(name);
            if (cycle == cycles_.end()) {
                return false;
            }
            const auto worker = std::find_if(cycle_threads_.begin(), cycle_threads_.end(),
                [&name](const auto& entry) { return entry.first == name; });
            if (worker != cycle_threads_.end()) {
                return false;
            }

            info = cycle->second;
            stop_requested_[name] = false;
            cycle_threads_.emplace_back(name, std::thread([this, name, info]() {
                auto next_cycle_time = std::chrono::steady_clock::now();
                const auto period = std::chrono::nanoseconds(info.period_ns);
                while (true) {
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        if (stop_requested_[name]) {
                            return;
                        }
                    }
                    if (info.callback) {
                        info.callback();
                    }
                    next_cycle_time += period;
                    std::this_thread::sleep_until(next_cycle_time);
                }
            }));
        }
        return true;
    }

    void stop_cycle(const std::string& name) override {
        std::thread cycle_thread;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_requested_[name] = true;
            const auto worker = std::find_if(cycle_threads_.begin(), cycle_threads_.end(),
                [&name](const auto& entry) { return entry.first == name; });
            if (worker != cycle_threads_.end()) {
                cycle_thread = std::move(worker->second);
                cycle_threads_.erase(worker);
            }
        }
        if (cycle_thread.joinable()) {
            cycle_thread.join();
        }
    }

    kairo::CycleStatistics get_statistics(const std::string& name) const override {
        kairo::CycleStatistics stats;
        stats.name = name;
        return stats;
    }

private:
    void stop_all_cycles() {
        std::vector<std::thread> cycle_threads;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& stop_requested : stop_requested_) {
                stop_requested.second = true;
            }
            for (auto& worker : cycle_threads_) {
                cycle_threads.push_back(std::move(worker.second));
            }
            cycle_threads_.clear();
        }
        for (auto& cycle_thread : cycle_threads) {
            if (cycle_thread.joinable()) {
                cycle_thread.join();
            }
        }
    }

    std::unordered_map<std::string, CycleInfo> cycles_;
    std::unordered_map<std::string, bool> stop_requested_;
    std::vector<std::pair<std::string, std::thread>> cycle_threads_;
    mutable std::mutex mutex_;
};

int CycleManagerStopJoinsWorker() {
    std::atomic<int> callback_count{0};
    std::atomic<bool> callback_entered{false};
    std::atomic<bool> allow_callback_exit{false};
    {
        SimpleCycleManager manager;
        if (!manager.register_cycle("smoke", 1000000, [&callback_count]() {
                callback_count.fetch_add(1, std::memory_order_relaxed);
            })) {
            return 1;
        }
        if (!manager.register_cycle("join", 1000000,
                [&callback_entered, &allow_callback_exit]() {
                    callback_entered.store(true, std::memory_order_release);
                    while (!allow_callback_exit.load(std::memory_order_acquire)) {
                        std::this_thread::yield();
                    }
                })) {
            return 1;
        }
        if (!manager.start_cycle("smoke") || !manager.start_cycle("join")) {
            return 1;
        }
        while (!callback_entered.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        std::atomic<bool> stop_returned{false};
        std::thread stopper([&manager, &stop_returned]() {
            manager.stop_cycle("join");
            stop_returned.store(true, std::memory_order_release);
        });
        manager.stop_cycle("smoke");

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (stop_returned.load(std::memory_order_acquire)) {
            allow_callback_exit.store(true, std::memory_order_release);
            stopper.join();
            return 1;
        }
        allow_callback_exit.store(true, std::memory_order_release);
        stopper.join();
        if (!stop_returned.load(std::memory_order_acquire)) {
            return 1;
        }
    }

    const int count_after_destruction = callback_count.load(std::memory_order_relaxed);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return callback_count.load(std::memory_order_relaxed) == count_after_destruction ? 0 : 1;
}

int main() {
    return CycleManagerStopJoinsWorker();
}
