#include <atomic>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

#include <kairo/executor.hpp>

namespace {

struct SensorFrame {
    int id;
    int samples;
};

int score_frame(SensorFrame frame, int weight) {
    return frame.samples * weight;
}

class Planner {
public:
    explicit Planner(std::string name) : name_(std::move(name)) {}

    std::string make_plan(SensorFrame frame) const {
        return name_ + "-frame-" + std::to_string(frame.id);
    }

private:
    std::string name_;
};

}

int main() {
    auto& executor = kairo::Executor::instance();
    SensorFrame frame{7, 21};

    auto score = executor.submit_auto([frame] {
        return score_frame(frame, 2);
    });

    int offset = 5;
    auto adjusted = executor.submit_auto([frame, offset]() noexcept {
        return frame.samples + offset;
    });

    auto payload = std::make_unique<int>(9);
    auto owned = executor.submit_auto([payload = std::move(payload)]() mutable noexcept {
        return *payload;
    });

    auto planner = std::make_shared<Planner>("local");
    auto plan = executor.submit_auto([planner, frame] {
        return planner->make_plan(frame);
    });

    auto processed = std::make_shared<std::atomic<int>>(0);
    auto counted = executor.submit_auto([processed] {
        processed->fetch_add(1);
    });

    std::cout << "score=" << score.get() << ", plan=" << plan.get()
              << ", adjusted=" << adjusted.get() << ", owned=" << owned.get()
              << '\n';
    counted.get();
    std::cout << "processed=" << processed->load() << '\n';

    executor.shutdown();
    return 0;
}
