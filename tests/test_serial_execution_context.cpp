#include <kairo/executor.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

int main() {
    kairo::Executor ex;
    kairo::ExecutorConfig config;
    config.min_threads = 2;
    config.max_threads = 2;
    assert(ex.initialize(config));

    kairo::SerialExecutionContext context;
    std::mutex mutex;
    std::vector<int> order;
    auto first = ex.submit_on(context, [&] { std::lock_guard<std::mutex> lock(mutex); order.push_back(1); });
    auto second = ex.submit_on(context, [&] { std::lock_guard<std::mutex> lock(mutex); order.push_back(2); });
    first.get();
    second.get();
    assert((order == std::vector<int>{1, 2}));

    // The facade workers may start wrappers in a different order from the
    // caller's submissions.  Context tickets must preserve the API order.
    order.clear();
    constexpr int kFifoTasks = 128;
    std::vector<std::future<void>> fifo_futures;
    fifo_futures.reserve(kFifoTasks);
    for (int i = 0; i < kFifoTasks; ++i) {
        fifo_futures.push_back(ex.submit_on(context, [&, i] {
            std::lock_guard<std::mutex> lock(mutex);
            order.push_back(i);
        }));
    }
    for (auto& future : fifo_futures) future.get();
    assert(order.size() == static_cast<size_t>(kFifoTasks));
    for (int i = 0; i < kFifoTasks; ++i) assert(order[static_cast<size_t>(i)] == i);

    auto failed = ex.submit_on(context, []() -> int { throw std::runtime_error("context failure"); });
    bool saw_failure = false;
    try { (void)failed.get(); } catch (const std::runtime_error&) { saw_failure = true; }
    assert(saw_failure);

    // A queued facade cancellation must release its context ticket so a
    // later submission cannot remain behind a cancelled wrapper.
    kairo::Executor blocked_executor;
    kairo::ExecutorConfig blocked_config;
    blocked_config.min_threads = 1;
    blocked_config.max_threads = 1;
    assert(blocked_executor.initialize(blocked_config));
    std::promise<void> blocker_started;
    auto blocker_started_future = blocker_started.get_future();
    std::promise<void> release_blocker;
    auto release_blocker_future = release_blocker.get_future().share();
    auto blocker = blocked_executor.submit([&] {
        blocker_started.set_value();
        release_blocker_future.wait();
    });
    blocker_started_future.wait();

    kairo::SerialExecutionContext cancellation_context;
    std::atomic<int> ran_after_cancel{0};
    auto cancelled = blocked_executor.submit_on_with_handle(
        cancellation_context, [&] { ran_after_cancel.fetch_add(100); });
    auto after_cancel = blocked_executor.submit_on_with_handle(
        cancellation_context, [&] { ran_after_cancel.fetch_add(1); });
    auto cancel_response = blocked_executor.request_task_cancel(cancelled.handle);
    assert(cancel_response.result == kairo::TaskCancellationResult::RequestedBeforeStart);
    bool saw_cancel = false;
    try { (void)cancelled.future.get(); } catch (const kairo::TaskCancelled&) { saw_cancel = true; }
    assert(saw_cancel);
    release_blocker.set_value();
    blocker.get();
    after_cancel.future.get();
    assert(ran_after_cancel.load() == 1);
    cancellation_context.shutdown();

    // Shutting down the context before its facade wrapper is dispatched must
    // still settle the wrapper's future instead of leaving it behind a ticket.
    std::promise<void> shutdown_blocker_release;
    auto shutdown_blocker_release_future = shutdown_blocker_release.get_future().share();
    std::promise<void> shutdown_blocker_started;
    auto shutdown_blocker_started_future = shutdown_blocker_started.get_future();
    auto shutdown_blocker = blocked_executor.submit([&] {
        shutdown_blocker_started.set_value();
        shutdown_blocker_release_future.wait();
    });
    shutdown_blocker_started_future.wait();
    kairo::SerialExecutionContext shutdown_context;
    auto pending_shutdown = blocked_executor.submit_on_with_handle(
        shutdown_context, [] { assert(false && "shutdown context task must not run"); });
    shutdown_context.shutdown();
    shutdown_blocker_release.set_value();
    shutdown_blocker.get();
    bool saw_context_shutdown = false;
    try { (void)pending_shutdown.future.get(); }
    catch (const kairo::ExecutorStopping&) { saw_context_shutdown = true; }
    assert(saw_context_shutdown);
    blocked_executor.shutdown();

    context.shutdown();
    auto rejected = ex.submit_on(context, [] { return 7; });
    bool saw_stopped = false;
    try { (void)rejected.get(); } catch (const kairo::ExecutorStopping&) { saw_stopped = true; }
    assert(saw_stopped);
    ex.shutdown();
    return 0;
}
