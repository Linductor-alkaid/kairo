#include <functional>
#include <iostream>
#include <string>

#include <kairo/interfaces.hpp>

using namespace kairo;

#define TEST_ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            std::cerr << "FAILED: " << message << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
            return false; \
        } \
    } while (0)

class MinimalRealtimeExecutor final : public IRealtimeExecutor {
public:
    bool start() override {
        started_ = true;
        return true;
    }

    void stop() override {
        started_ = false;
    }

    void push_task(std::function<void()> task) override {
        ++push_task_count_;
        last_task_valid_ = static_cast<bool>(task);
    }

    std::string get_name() const override {
        return "minimal_realtime_executor";
    }

    RealtimeExecutorStatus get_status() const override {
        RealtimeExecutorStatus status;
        status.is_running = started_;
        return status;
    }

    int push_task_count() const {
        return push_task_count_;
    }

    bool last_task_valid() const {
        return last_task_valid_;
    }

private:
    bool started_{false};
    int push_task_count_{0};
    bool last_task_valid_{false};
};

static bool test_push_task_ex_default_delegates_to_push_task() {
    MinimalRealtimeExecutor executor;

    const bool accepted = executor.push_task_ex([]() noexcept {});

    TEST_ASSERT(accepted, "default push_task_ex should return true");
    TEST_ASSERT(executor.push_task_count() == 1,
                "default push_task_ex should call push_task exactly once");
    TEST_ASSERT(executor.last_task_valid(),
                "default push_task_ex should forward the task object");

    return true;
}

int main() {
    std::cout << "Testing IRealtimeExecutor::push_task_ex default implementation...\n";

    if (!test_push_task_ex_default_delegates_to_push_task()) {
        return 1;
    }

    std::cout << "All push_task_ex default tests PASSED\n";
    return 0;
}
