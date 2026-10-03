// A1 验收测试（mira_feedback_update_plan.md，对齐 docs/design/bounded_admission.md）：
// 总量有界 admission 的拒绝语义、各终态恰好一次释放、shutdown 交错、
// capacity rejection 与 stopping/invalid input 的可区分性、串行 ticket 释放。
//
// 断言使用 ADM_CHECK 而非 assert：Release(-DNDEBUG) 下 assert 会被剥离，
// ex.initialize() 将不执行、facade 懒初始化为默认配置（admission 关闭），
// 测试既空转也可能与自身的 blocker 释放顺序死锁。ADM_CHECK 显式求值并在
// 失败时打印退出，保证 Release 运行同等地验证验收标准。
#include <kairo/executor.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

#define ADM_CHECK(cond)                                                     \
    do {                                                                    \
        if (!(cond)) {                                                      \
            std::fprintf(stderr, "test_bounded_admission: FAILED %s (%s:%d)\n", \
                         #cond, __FILE__, __LINE__);                        \
            std::exit(1);                                                   \
        }                                                                   \
    } while (0)

// 期望立即就绪的 future 用有界等待：即使实现回归也不把 CI 挂满 120s。
template <typename Future>
bool settled_soon(Future& future, std::chrono::seconds budget) {
    return future.wait_for(budget) == std::future_status::ready;
}

}  // namespace

int main() {
    using kairo::CapacityExhaustedException;
    using kairo::Executor;
    using kairo::ExecutorConfig;

    // 验收 1：单 worker、总容量 N 时第 N+1 个未结算 submit 明确拒绝且 future
    // 就绪；解除后恢复。验收 4 前半：failure 计数可观测。
    {
        Executor ex;
        ExecutorConfig config;
        config.min_threads = 1;
        config.max_threads = 1;
        config.max_in_flight_tasks = 3;
        ADM_CHECK(ex.initialize(config));

        std::promise<void> release;
        auto release_future = release.get_future().share();
        std::promise<void> started;
        auto started_future = started.get_future();
        std::vector<std::future<void>> blocked;
        blocked.push_back(ex.submit([&] {
            started.set_value();
            release_future.wait();
        }));
        for (int i = 1; i < 3; ++i) {
            blocked.push_back(ex.submit([&] { release_future.wait(); }));
        }
        started_future.wait();
        // 3 个未结算提交占满容量（1 运行 + 2 排队）。
        ADM_CHECK(ex.get_in_flight_submissions() == 3);
        ADM_CHECK(ex.get_max_in_flight_tasks() == 3);

        auto rejected = ex.submit([] { return 7; });
        ADM_CHECK(settled_soon(rejected, std::chrono::seconds(2)));
        bool saw_capacity = false;
        try { (void)rejected.get(); }
        catch (const CapacityExhaustedException&) { saw_capacity = true; }
        ADM_CHECK(saw_capacity);
        // 拒绝不改变在途计数，不产生额外堆积。
        ADM_CHECK(ex.get_in_flight_submissions() == 3);

        release.set_value();
        for (auto& future : blocked) future.get();
        ADM_CHECK(ex.get_in_flight_submissions() == 0);
        ADM_CHECK(ex.submit([] { return 11; }).get() == 11);

        const auto failure_status = ex.get_failure_status();
        ADM_CHECK(failure_status.capacity_exhausted_count == 1);
        ADM_CHECK(failure_status.total_count >= 1);
        const auto recent = ex.get_recent_failures(8);
        ADM_CHECK(!recent.empty());
        ADM_CHECK(recent.front().kind == kairo::FailureKind::CapacityExhausted);
        ex.shutdown();
    }

    // 验收 2：各终态恰好一次释放（完成 / 异常 / 排队取消 / 执行前超时）。
    {
        Executor ex;
        ExecutorConfig config;
        config.min_threads = 1;
        config.max_threads = 1;
        config.max_in_flight_tasks = 4;
        config.task_timeout_ms = 50;
        ADM_CHECK(ex.initialize(config));

        // 完成。
        ADM_CHECK(ex.submit([] { return 1; }).get() == 1);
        ADM_CHECK(ex.get_in_flight_submissions() == 0);
        // 异常。
        bool saw_error = false;
        try { (void)ex.submit([] { throw std::runtime_error("boom"); }).get(); }
        catch (const std::runtime_error&) { saw_error = true; }
        ADM_CHECK(saw_error);
        ADM_CHECK(ex.get_in_flight_submissions() == 0);

        // 排队取消：先占住唯一 worker，tracked 任务排队后被取消。
        std::promise<void> release_cancel;
        auto release_cancel_future = release_cancel.get_future().share();
        std::promise<void> cancel_blocker_started;
        auto cancel_blocker_started_future = cancel_blocker_started.get_future();
        auto cancel_blocker = ex.submit([&] {
            cancel_blocker_started.set_value();
            release_cancel_future.wait();
        });
        cancel_blocker_started_future.wait();
        auto cancelled = ex.submit_with_handle([] { ADM_CHECK(false && "cancelled before start"); });
        const auto response = ex.request_task_cancel(cancelled.handle);
        ADM_CHECK(response.result == kairo::TaskCancellationResult::RequestedBeforeStart);
        bool saw_cancel = false;
        try { (void)cancelled.future.get(); }
        catch (const kairo::TaskCancelled&) { saw_cancel = true; }
        ADM_CHECK(saw_cancel);
        ADM_CHECK(ex.get_in_flight_submissions() == 1);  // 仅剩 blocker
        release_cancel.set_value();
        cancel_blocker.get();
        ADM_CHECK(ex.get_in_flight_submissions() == 0);

        // 执行前超时：sleep 占满 worker，排队任务出队时已过期。
        auto sleeper = ex.submit([] {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        });
        auto timed_out = ex.submit([] { ADM_CHECK(false && "must time out"); });
        bool saw_timeout = false;
        try { (void)timed_out.get(); }
        catch (const kairo::TimedOutException&) { saw_timeout = true; }
        ADM_CHECK(saw_timeout);
        sleeper.get();
        ADM_CHECK(ex.get_in_flight_submissions() == 0);
        ex.shutdown();
    }

    // 验收 5：submit_on_with_handle 拒绝时释放 context ticket，后续 FIFO 不阻塞。
    {
        Executor ex;
        ExecutorConfig config;
        config.min_threads = 1;
        config.max_threads = 1;
        config.max_in_flight_tasks = 2;
        ADM_CHECK(ex.initialize(config));
        kairo::SerialExecutionContext context;

        std::promise<void> release;
        auto release_future = release.get_future().share();
        std::promise<void> started;
        auto started_future = started.get_future();
        auto blocker = ex.submit([&] {
            started.set_value();
            release_future.wait();
        });
        started_future.wait();

        // 容量 2：blocker + 第一个串行提交占满；第二个串行提交被拒，
        // 其 ticket 必须被释放，不得阻塞后续 ticket 的顺序执行。
        auto first = ex.submit_on_with_handle(context, [] { return 1; });
        auto rejected = ex.submit_on_with_handle(context, [] { return 2; });
        ADM_CHECK(settled_soon(rejected.future, std::chrono::seconds(2)));
        bool saw_capacity = false;
        try { (void)rejected.future.get(); }
        catch (const CapacityExhaustedException&) { saw_capacity = true; }
        ADM_CHECK(saw_capacity);

        release.set_value();
        blocker.get();
        ADM_CHECK(first.future.get() == 1);
        auto third = ex.submit_on_with_handle(context, [] { return 3; });
        ADM_CHECK(third.future.get() == 3);
        ADM_CHECK(ex.get_in_flight_submissions() == 0);
        context.shutdown();
        ex.shutdown();
    }

    // 验收 4：capacity rejection 与 invalid input 可区分（异常类型 + failure kind）。
    {
        Executor ex;
        ExecutorConfig config;
        config.min_threads = 1;
        config.max_threads = 1;
        config.max_in_flight_tasks = 1;
        ADM_CHECK(ex.initialize(config));

        std::promise<void> release;
        auto release_future = release.get_future().share();
        auto blocker = ex.submit([&] { release_future.wait(); });

        auto rejected = ex.submit([] { return 1; });
        ADM_CHECK(settled_soon(rejected, std::chrono::seconds(2)));
        bool saw_capacity = false;
        try { (void)rejected.get(); }
        catch (const CapacityExhaustedException&) { saw_capacity = true; }
        ADM_CHECK(saw_capacity);

        // invalid input：空任务的拒绝是 std::invalid_argument。
        auto empty = ex.submit(std::function<int()>());
        bool saw_invalid = false;
        try { (void)empty.get(); }
        catch (const std::invalid_argument&) { saw_invalid = true; }
        ADM_CHECK(saw_invalid);

        release.set_value();
        blocker.get();

        const auto status = ex.get_failure_status();
        ADM_CHECK(status.capacity_exhausted_count == 1);
        ADM_CHECK(status.submit_rejected_count >= 1);  // 空 task 走 SubmitRejected
        ex.shutdown();
    }

    // 验收 3：shutdown 与并发 submit 不越过容量，也不留下未就绪 future。
    {
        Executor ex;
        ExecutorConfig config;
        config.min_threads = 2;
        config.max_threads = 2;
        config.max_in_flight_tasks = 8;
        ADM_CHECK(ex.initialize(config));

        std::atomic<uint64_t> capacity_rejections{0};
        ex.set_failure_callback([&](const kairo::ExecutorFailureEvent& event) {
            if (event.kind == kairo::FailureKind::CapacityExhausted) {
                capacity_rejections.fetch_add(1, std::memory_order_relaxed);
            }
        });

        std::mutex futures_mutex;
        std::vector<std::future<void>> futures;
        std::atomic<bool> stopping{false};
        std::thread submitter([&] {
            for (int i = 0; i < 4000; ++i) {
                auto future = ex.submit([] {});
                std::lock_guard<std::mutex> lock(futures_mutex);
                futures.push_back(std::move(future));
                if (stopping.load(std::memory_order_acquire)) break;
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        stopping.store(true, std::memory_order_release);
        submitter.join();
        ex.shutdown();
        // 并发提交期间在途计数从未超过容量上限（由计数器语义保证：接纳数
        // - 终态数 <= 上限）；shutdown 后所有 future 均已就绪。
        for (auto& future : futures) {
            bool settled = false;
            try { future.get(); settled = true; }
            catch (const CapacityExhaustedException&) { settled = true; }
            catch (const kairo::ExecutorStopping&) { settled = true; }
            catch (const std::runtime_error&) {
                // 池停止后的 SubmitRejected 结算路径。
                settled = true;
            }
            ADM_CHECK(settled);
        }
        ADM_CHECK(ex.get_in_flight_submissions() == 0);
        // 峰值不超过容量的必要条件：拒绝只在容量满时发生，且每条拒绝都有
        // 对应 failure 事件（回读计数与回调计数一致）。
        ADM_CHECK(capacity_rejections.load() ==
                  ex.get_failure_status().capacity_exhausted_count);
    }

    // 运行期调整：调小不驱逐已接纳任务，只约束后续提交。
    {
        Executor ex;
        ExecutorConfig config;
        config.min_threads = 1;
        config.max_threads = 1;
        ADM_CHECK(ex.initialize(config));
        ADM_CHECK(ex.get_max_in_flight_tasks() == 0);
        ADM_CHECK(ex.get_in_flight_submissions() == 0);  // 未启用恒为 0

        ex.set_max_in_flight_tasks(1);
        std::promise<void> release;
        auto release_future = release.get_future().share();
        auto blocker = ex.submit([&] { release_future.wait(); });
        auto rejected = ex.submit([] {});
        ADM_CHECK(settled_soon(rejected, std::chrono::seconds(2)));
        bool saw_capacity = false;
        try { (void)rejected.get(); }
        catch (const CapacityExhaustedException&) { saw_capacity = true; }
        ADM_CHECK(saw_capacity);
        release.set_value();
        blocker.get();
        ex.shutdown();
    }

    return 0;
}
