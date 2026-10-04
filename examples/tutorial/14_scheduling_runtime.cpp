// 14_scheduling_runtime.cpp — declare scheduling requirements per task
// (0.6.0 Scheduling Runtime): QoS preset, deadline admission, and an
// injectable scheduler. Outputs only stable, checkable facts.

#include <chrono>
#include <iostream>
#include <atomic>

#include <kairo/executor.hpp>
#include <kairo/scheduler.hpp>

using namespace kairo;

int main() {
    Executor executor;
    ExecutorConfig config;
    config.min_threads = 2;
    config.max_threads = 2;
    if (!executor.initialize(config)) {
        return 1;
    }

    // 1. QoS preset: an Interactive task without an explicit priority runs
    //    in the HIGH queue slot. No guarantee beyond ordering is implied.
    auto qos_future = executor.submit_auto(
        kairo::task([] { return 42; }).qos(QosClass::Interactive));
    std::cout << "qos task result=" << qos_future.get() << "\n";

    // 2. deadline admission: a deadline that is already in the past is
    //    rejected up front instead of being queued to miss.
    auto expired = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    auto rejected = executor.submit_auto(
        kairo::task([] { return 0; }).deadline(expired));
    bool rejected_cleanly = false;
    try {
        (void)rejected.get();
    } catch (const std::exception& e) {
        rejected_cleanly =
            std::string(e.what()).find("deadline") != std::string::npos;
    }
    std::cout << "expired deadline rejected=" << (rejected_cleanly ? "yes" : "no")
              << "\n";

    // 3. a feasible deadline executes normally and can be observed.
    auto feasible = executor.submit_auto(
        kairo::task([] { return 7; })
            .deadline(std::chrono::steady_clock::now() +
                      std::chrono::seconds(5)));
    std::cout << "feasible deadline result=" << feasible.get() << "\n";

    // 4. inject a recording scheduler: scheduling decisions are pluggable.
    class RecordingScheduler final : public IScheduler {
    public:
        RoutingDecision route(
            const TaskRouter::Request& request,
            const std::vector<ExecutorCapability>& capabilities) override {
            ++routes;
            return DefaultScheduler{}.route(request, capabilities);
        }
        std::atomic<int> routes{0};
    };
    auto custom = std::make_unique<RecordingScheduler>();
    auto* raw = custom.get();
    executor.set_scheduler(std::move(custom));
    auto delegated = executor.submit_auto(kairo::task([] { return 1; }));
    (void)delegated.get();
    std::cout << "custom scheduler routes=" << raw->routes.load() << "\n";
    executor.set_scheduler(nullptr);  // restore the default scheduler

    (void)executor.wait_for_completion(std::chrono::seconds{5});
    executor.shutdown();
    return 0;
}
