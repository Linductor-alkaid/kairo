#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

namespace kairo::test::harness {

class Stepper {
public:
    using Duration = std::chrono::milliseconds;

    explicit Stepper(Duration default_timeout = Duration{1000})
        : default_timeout_(default_timeout) {}

    void arrive(std::string_view point) {
        std::lock_guard<std::mutex> lock(mutex_);
        PointState& state = points_[to_string(point)];
        ++state.arrivals;
        condition_.notify_all();
    }

    void wait_for(std::string_view point) {
        if (!wait_for(point, default_timeout_)) {
            throw std::runtime_error("Timed out waiting for point '" + to_string(point) + "'");
        }
    }

    bool wait_for(std::string_view point, Duration timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        const std::string name = to_string(point);
        return condition_.wait_for(lock, timeout, [&] {
            PointState& state = points_[name];
            if (state.arrivals > 0) {
                return true;
            }
            if (state.release_all) {
                return true;
            }
            return state.releases > 0;
        }) && consume_if_released(name);
    }

    void wait_for_arrivals(std::string_view point, std::size_t count) {
        if (!wait_for_arrivals(point, count, default_timeout_)) {
            throw std::runtime_error("Timed out waiting for " + std::to_string(count) +
                                     " arrivals at point '" + to_string(point) + "'");
        }
    }

    bool wait_for_arrivals(std::string_view point, std::size_t count, Duration timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        const std::string name = to_string(point);
        return condition_.wait_for(lock, timeout, [&] {
            return points_[name].arrivals >= count;
        });
    }

    void release(std::string_view point) {
        std::lock_guard<std::mutex> lock(mutex_);
        PointState& state = points_[to_string(point)];
        ++state.releases;
        condition_.notify_all();
    }

    void release_all(std::string_view point) {
        std::lock_guard<std::mutex> lock(mutex_);
        PointState& state = points_[to_string(point)];
        state.release_all = true;
        condition_.notify_all();
    }

    [[nodiscard]] std::size_t arrival_count(std::string_view point) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = points_.find(to_string(point));
        if (it == points_.end()) {
            return 0;
        }
        return it->second.arrivals;
    }

    [[nodiscard]] bool has_arrived(std::string_view point) const {
        return arrival_count(point) > 0;
    }

    void reset(std::string_view point) {
        std::lock_guard<std::mutex> lock(mutex_);
        points_.erase(to_string(point));
    }

    void reset_all() {
        std::lock_guard<std::mutex> lock(mutex_);
        points_.clear();
    }

private:
    struct PointState {
        std::size_t arrivals{0};
        std::size_t releases{0};
        bool release_all{false};
    };

    static std::string to_string(std::string_view point) {
        return std::string(point);
    }

    bool consume_if_released(const std::string& point) {
        PointState& state = points_[point];
        if (state.arrivals > 0 || state.release_all) {
            return true;
        }
        if (state.releases > 0) {
            --state.releases;
            return true;
        }
        return false;
    }

    Duration default_timeout_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::unordered_map<std::string, PointState> points_;
};

} // namespace kairo::test::harness
