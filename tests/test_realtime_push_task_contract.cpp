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

    ExecutorResult push_task(std::function<void()> task) override {
        ++push_task_count_;
        last_task_valid_ = static_cast<bool>(task);
        if (!task) {
            return ExecutorResult::failure(
                ExecutorErrorCode::InvalidConfig, "null task");
        }
        return ExecutorResult::success();
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

static bool test_push_task_returns_diagnostic_result() {
    MinimalRealtimeExecutor executor;

    const auto accepted = executor.push_task([]() noexcept {});

    TEST_ASSERT(accepted.ok, "accepted push_task should return ok");
    TEST_ASSERT(executor.push_task_count() == 1,
                "push_task should be invoked exactly once");
    TEST_ASSERT(executor.last_task_valid(),
                "push_task should receive the task object");

    const auto rejected = executor.push_task(nullptr);
    TEST_ASSERT(!rejected.ok, "null task should be rejected");
    TEST_ASSERT(executor.push_task_count() == 2,
                "rejected push_task should still reach the derived implementation");

    return true;
}

int main() {
    std::cout << "Testing IRealtimeExecutor::push_task diagnostic contract...\n";

    if (!test_push_task_returns_diagnostic_result()) {
        return 1;
    }

    std::cout << "All push_task contract tests PASSED\n";
    return 0;
}
