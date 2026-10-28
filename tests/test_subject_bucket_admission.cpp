// A1 验收测试（docs/design/subject_bucket_admission.md「## 验收」1–8，issue #222）：
// 分主体在途份额 admission 的接纳/拒绝语义、与总量维度的叠加与独立性、
// 各终态恰好一次释放、future 就绪前计数已释放、份额热更新与解除、
// shutdown 语义与零回归锚。
//
// 块编号 // 验收 N 对应设计文档「## 验收」第 N 条。
//
// 断言使用 ADM_CHECK 而非 assert：Release(-DNDEBUG) 下 assert 会被剥离，
// ex.initialize() 将不执行、facade 懒初始化为默认配置（admission 关闭），
// 测试既空转也可能与自身的 blocker 释放顺序死锁。ADM_CHECK 显式求值并在
// 失败时打印退出，保证 Release 运行同等地验证验收标准。
//
// 带键路径说明（设计文档 D-4，一期覆盖面）：submit_auto(TaskBuilder) 无
// 句柄、无法排队取消；因此「排队取消」「执行前超时」终态经同样带键的
// submit_after_with_handle(TaskBuilder) 路径构造（与 submit_auto 的
// submit_priority_scheduled 同属 try_admit_submission 单一接纳点与同一
// AdmissionReleaser 释放链）。池严格排空语义（thread_pool worker 在 stop_
// 下仍排空队列）使得「池丢弃」终态无法经公开 API 确定性构造为"不执行"；
// 验收 3 末块改为验证 shutdown(false) 终局下分桶计数无泄漏归零（执行/
// 丢弃两种实现路径都必须释放）。
#include <kairo/executor.hpp>
#include <kairo/task_options.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

#define ADM_CHECK(cond)                                                        \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr,                                               \
                         "test_subject_bucket_admission: FAILED %s (%s:%d)\n", \
                         #cond, __FILE__, __LINE__);                           \
            std::exit(1);                                                      \
        }                                                                      \
    } while (0)

// 期望立即就绪的 future 用有界等待：即使实现回归也不把 CI 挂满 120s。
template <typename Future>
bool settled_soon(Future& future, std::chrono::seconds budget) {
    return future.wait_for(budget) == std::future_status::ready;
}

// 取出 future 的拒绝消息：期望 CapacityExhaustedException；其他终态以
// 可读文本返回（后续 find 断言必然失败并携带实际消息，便于定位）。
template <typename Future>
std::string capacity_rejection_message(Future& future) {
    try {
        (void)future.get();
    } catch (const kairo::CapacityExhaustedException& exception) {
        return exception.what();
    } catch (const std::exception& other) {
        return std::string("wrong exception: ") + other.what();
    }
    return "<future did not throw>";
}

}  // namespace

int main() {
    using kairo::CapacityExhaustedException;
    using kairo::Executor;
    using kairo::ExecutorConfig;

    // 验收 1：A/B 两主体各自份额内提交均被接纳、互不影响；A 超自身份额被拒
    // （CapacityExhaustedException + subject_quota_exhausted_count 递增 +
    // 事件携带 subject）且 B 仍可接纳。快照按 key 字典序。
    {
        Executor ex;
        ExecutorConfig config;
        config.min_threads = 1;
        config.max_threads = 1;
        // 全局上限不启用（默认 0）：本场景只验证分桶维度本身。
        ADM_CHECK(ex.initialize(config));

        // 未配置时的观测面：空快照、"未知即宽容"。
        ADM_CHECK(ex.get_subject_quota_status().empty());
        ADM_CHECK(ex.get_subject_max_in_flight_tasks("tenant-A") == 0);
        ADM_CHECK(ex.get_subject_in_flight_submissions("tenant-A") == 0);

        ex.set_subject_max_in_flight_tasks("tenant-B", 3);
        ex.set_subject_max_in_flight_tasks("tenant-A", 2);
        ADM_CHECK(ex.get_subject_max_in_flight_tasks("tenant-A") == 2);

        std::promise<void> release;
        auto release_future = release.get_future().share();
        std::promise<void> started;
        auto started_future = started.get_future();

        // A 份额 2：两个阻塞任务均接纳（1 运行 + 1 排队）。
        std::vector<std::future<void>> a_blocked;
        a_blocked.push_back(ex.submit_auto(kairo::task([&] {
            started.set_value();
            release_future.wait();
        }).subject("tenant-A")));
        a_blocked.push_back(ex.submit_auto(
            kairo::task([&] { release_future.wait(); }).subject("tenant-A")));
        started_future.wait();
        ADM_CHECK(ex.get_subject_in_flight_submissions("tenant-A") == 2);

        // A 第 3 个：分桶拒绝，future 立即就绪，消息可区分（含 key 与份额）。
        auto a3 = ex.submit_auto(kairo::task([] {
                                     ADM_CHECK(false && "rejected task must not run");
                                 }).subject("tenant-A"));
        ADM_CHECK(settled_soon(a3, std::chrono::seconds(2)));
        const std::string a3_message = capacity_rejection_message(a3);
        ADM_CHECK(a3_message.find("key=tenant-A") != std::string::npos);
        ADM_CHECK(a3_message.find("subject_max_in_flight=2") != std::string::npos);
        // 拒绝不改变在途计数。
        ADM_CHECK(ex.get_subject_in_flight_submissions("tenant-A") == 2);

        // B 未受 A 影响：份额 3 内继续全部接纳，满后同样按分桶拒绝。
        std::vector<std::future<void>> b_blocked;
        for (int i = 0; i < 3; ++i) {
            b_blocked.push_back(ex.submit_auto(
                kairo::task([&] { release_future.wait(); }).subject("tenant-B")));
        }
        ADM_CHECK(ex.get_subject_in_flight_submissions("tenant-B") == 3);
        auto b4 = ex.submit_auto(kairo::task([] {
                                     ADM_CHECK(false && "rejected task must not run");
                                 }).subject("tenant-B"));
        ADM_CHECK(settled_soon(b4, std::chrono::seconds(2)));
        const std::string b4_message = capacity_rejection_message(b4);
        ADM_CHECK(b4_message.find("key=tenant-B") != std::string::npos);
        ADM_CHECK(ex.get_subject_in_flight_submissions("tenant-B") == 3);

        // failure 可观测：分桶拒绝同时递增两个计数（前者是后者子集），
        // recent 事件携带分桶键。
        const auto failure_status = ex.get_failure_status();
        ADM_CHECK(failure_status.subject_quota_exhausted_count == 2);
        ADM_CHECK(failure_status.capacity_exhausted_count == 2);
        const auto recent = ex.get_recent_failures(2);
        ADM_CHECK(recent.size() == 2);
        ADM_CHECK(recent.front().kind == kairo::FailureKind::CapacityExhausted);
        ADM_CHECK(recent.front().subject == "tenant-A");
        ADM_CHECK(recent.back().subject == "tenant-B");

        // 快照按 key 字典序，数值与份额/在途一致。
        const auto snapshot = ex.get_subject_quota_status();
        ADM_CHECK(snapshot.size() == 2);
        ADM_CHECK(snapshot[0].subject == "tenant-A");
        ADM_CHECK(snapshot[0].max_in_flight_tasks == 2);
        ADM_CHECK(snapshot[0].in_flight_submissions == 2);
        ADM_CHECK(snapshot[1].subject == "tenant-B");
        ADM_CHECK(snapshot[1].max_in_flight_tasks == 3);
        ADM_CHECK(snapshot[1].in_flight_submissions == 3);

        release.set_value();
        for (auto& future : a_blocked) future.get();
        for (auto& future : b_blocked) future.get();
        ADM_CHECK(ex.get_subject_in_flight_submissions("tenant-A") == 0);
        ADM_CHECK(ex.get_subject_in_flight_submissions("tenant-B") == 0);
        ex.shutdown();
    }

    // 验收 2：A+B 合计超全局上限时按既有总量语义拒绝（事件 subject 为空、
    // 消息为总量文本、只递增 capacity_exhausted_count）。
    {
        Executor ex;
        ExecutorConfig config;
        config.min_threads = 1;
        config.max_threads = 1;
        config.max_in_flight_tasks = 2;
        ADM_CHECK(ex.initialize(config));
        ex.set_subject_max_in_flight_tasks("tenant-A", 10);

        std::promise<void> release;
        auto release_future = release.get_future().share();
        std::promise<void> started;
        auto started_future = started.get_future();

        std::vector<std::future<void>> blocked;
        blocked.push_back(ex.submit_auto(kairo::task([&] {
            started.set_value();
            release_future.wait();
        }).subject("tenant-A")));
        blocked.push_back(ex.submit_auto(
            kairo::task([&] { release_future.wait(); }).subject("tenant-A")));
        started_future.wait();
        ADM_CHECK(ex.get_in_flight_submissions() == 2);
        ADM_CHECK(ex.get_subject_in_flight_submissions("tenant-A") == 2);

        // 主体份额（10）未触，全局上限（2）先拒：总量语义优先。
        auto third = ex.submit_auto(
            kairo::task([] { ADM_CHECK(false && "rejected task must not run"); })
                .subject("tenant-A"));
        ADM_CHECK(settled_soon(third, std::chrono::seconds(2)));
        const std::string message = capacity_rejection_message(third);
        ADM_CHECK(message.find("In-flight submission capacity exhausted") !=
                  std::string::npos);

        const auto failure_status = ex.get_failure_status();
        ADM_CHECK(failure_status.capacity_exhausted_count == 1);
        ADM_CHECK(failure_status.subject_quota_exhausted_count == 0);
        const auto recent = ex.get_recent_failures(1);
        ADM_CHECK(recent.size() == 1);
        ADM_CHECK(recent.front().kind == kairo::FailureKind::CapacityExhausted);
        ADM_CHECK(recent.front().subject.empty());
        ADM_CHECK(recent.front().message.find("max_in_flight_tasks=2") !=
                  std::string::npos);
        // 两维计数均未被拒绝改变。
        ADM_CHECK(ex.get_in_flight_submissions() == 2);
        ADM_CHECK(ex.get_subject_in_flight_submissions("tenant-A") == 2);

        release.set_value();
        for (auto& future : blocked) future.get();
        ADM_CHECK(ex.get_in_flight_submissions() == 0);
        ADM_CHECK(ex.get_subject_in_flight_submissions("tenant-A") == 0);
        ex.shutdown();
    }

    // 验收 3（前半）+ 验收 4：恰好一次接纳/释放——正常完成与任务异常两个
    // 终态后该主体在途计数归零且同份额可再接纳。
    {
        Executor ex;
        ExecutorConfig config;
        config.min_threads = 1;
        config.max_threads = 1;
        ADM_CHECK(ex.initialize(config));
        ex.set_subject_max_in_flight_tasks("A", 1);

        // 正常完成。
        ADM_CHECK(ex.submit_auto(kairo::task([] { return 42; }).subject("A"))
                      .get() == 42);
        // 验收 4：future 就绪后立即可观测最终值（分桶计数先于 future 释放，
        // 无需等待即可读到终值）。
        ADM_CHECK(ex.get_subject_in_flight_submissions("A") == 0);
        // 同份额可立即再接纳。
        ex.submit_auto(kairo::task([] {}).subject("A")).get();
        ADM_CHECK(ex.get_subject_in_flight_submissions("A") == 0);

        // 任务异常：future 以业务异常就绪，计数归零、可再接纳。
        bool saw_error = false;
        try {
            (void)ex.submit_auto(kairo::task([]() -> int {
                throw std::runtime_error("boom");
            }).subject("A")).get();
        } catch (const std::runtime_error&) {
            saw_error = true;
        }
        ADM_CHECK(saw_error);
        // 验收 4：异常 future 就绪后计数同样已释放。
        ADM_CHECK(ex.get_subject_in_flight_submissions("A") == 0);
        auto again = ex.submit_auto(kairo::task([] { return 7; }).subject("A"));
        ADM_CHECK(again.get() == 7);
        ADM_CHECK(ex.get_subject_in_flight_submissions("A") == 0);
        // 全程无容量类误报。
        ADM_CHECK(ex.get_failure_status().capacity_exhausted_count == 0);
        ex.shutdown();
    }

    // 验收 3（中）：排队取消与依赖失败级联两个终态（带键路径经
    // submit_after_with_handle / submit_after 的 TaskBuilder 重载）——
    // 任务不执行、分桶计数释放、同份额可再接纳。
    {
        Executor ex;
        ExecutorConfig config;
        config.min_threads = 1;
        config.max_threads = 1;
        ADM_CHECK(ex.initialize(config));
        // blocker（占 worker）与依赖门（park keyed 任务）不带键，不占 A 桶。
        ex.set_subject_max_in_flight_tasks("A", 2);

        std::promise<void> release;
        auto release_future = release.get_future().share();
        std::promise<void> started;
        auto started_future = started.get_future();
        auto blocker = ex.submit([&] {
            started.set_value();
            release_future.wait();
        });
        started_future.wait();

        // 排队取消：依赖未满足前 keyed 任务 parked（已接纳计数 1），
        // 请求取消后任务不执行、计数释放。
        std::atomic<int> ran{0};
        auto gate = ex.submit_with_handle([&] { release_future.wait(); });
        auto queued = ex.submit_after_with_handle(
            std::vector<kairo::TaskHandle>{gate.handle},
            kairo::task([&] { ++ran; }).subject("A"));
        ADM_CHECK(ex.get_subject_in_flight_submissions("A") == 1);
        const auto response = ex.request_task_cancel(queued.handle);
        ADM_CHECK(response.result ==
                  kairo::TaskCancellationResult::RequestedBeforeStart);
        ADM_CHECK(settled_soon(queued.future, std::chrono::seconds(2)));
        bool saw_cancel = false;
        try {
            (void)queued.future.get();
        } catch (const kairo::TaskCancelled&) {
            saw_cancel = true;
        }
        ADM_CHECK(saw_cancel);
        ADM_CHECK(ran.load() == 0);
        ADM_CHECK(ex.get_subject_in_flight_submissions("A") == 0);

        // 放行 blocker 与依赖门，后续场景在空闲 worker 上确定性执行。
        release.set_value();
        blocker.get();
        gate.future.get();
        ADM_CHECK(ex.get_subject_in_flight_submissions("A") == 0);

        // 依赖失败级联（上游排队取消 → 带键下游 DependencyCancelled）：
        // 下游不执行且计数释放（接纳后 settle_without_run 回滚）。
        // 注：非取消类上游异常按既有语义保持原异常
        //（reclassify_dependency_exception 只改写取消类），因此级联场景
        // 使用排队取消的上游。
        std::promise<void> release2;
        auto release2_future = release2.get_future().share();
        auto blocker2 = ex.submit([&] { release2_future.wait(); });
        auto upstream = ex.submit_with_handle([]
                                              { ADM_CHECK(false && "cancelled upstream must not run"); });
        const auto upstream_cancel =
            ex.request_task_cancel(upstream.handle);
        ADM_CHECK(upstream_cancel.result ==
                  kairo::TaskCancellationResult::RequestedBeforeStart);
        ADM_CHECK(settled_soon(upstream.future, std::chrono::seconds(2)));
        bool upstream_cancelled = false;
        try {
            (void)upstream.future.get();
        } catch (const kairo::TaskCancelled&) {
            upstream_cancelled = true;
        }
        ADM_CHECK(upstream_cancelled);

        auto downstream = ex.submit_after(
            std::vector<kairo::TaskHandle>{upstream.handle},
            kairo::task([&] {
                ++ran;
                return 1;
            }).subject("A"));
        ADM_CHECK(settled_soon(downstream, std::chrono::seconds(2)));
        bool saw_dependency_cancel = false;
        try {
            (void)downstream.get();
        } catch (const kairo::TaskCancelled& cancelled) {
            saw_dependency_cancel =
                cancelled.reason() ==
                kairo::TaskCancellationReason::DependencyCancelled;
        }
        ADM_CHECK(saw_dependency_cancel);
        ADM_CHECK(ran.load() == 0);
        ADM_CHECK(ex.get_subject_in_flight_submissions("A") == 0);

        // 两个终态后同份额可再接纳（blocker2 占住 worker，接纳后排队）。
        auto followup = ex.submit_auto(kairo::task([&] {
            ++ran;
            return 5;
        }).subject("A"));
        ADM_CHECK(ex.get_subject_in_flight_submissions("A") == 1);
        release2.set_value();
        blocker2.get();
        ADM_CHECK(followup.get() == 5);
        ADM_CHECK(ran.load() == 1);
        ADM_CHECK(ex.get_subject_in_flight_submissions("A") == 0);
        ex.shutdown();
    }

    // 验收 3（后）：执行前超时终态——keyed 任务 parked 期间 queued soft
    // timeout 先赢，计数释放且同份额可再接纳。
    {
        Executor ex;
        ExecutorConfig config;
        config.min_threads = 1;
        config.max_threads = 1;
        config.task_timeout_ms = 250;
        ADM_CHECK(ex.initialize(config));
        ex.set_subject_max_in_flight_tasks("A", 2);

        std::promise<void> release;
        auto release_future = release.get_future().share();
        std::promise<void> started;
        auto started_future = started.get_future();
        // 依赖门即占 worker 的阻塞任务（不带键）：keyed 任务 parked 期间
        // 门保持未决；运行中任务不受 queued 超时约束。
        auto gate = ex.submit_with_handle([&] {
            started.set_value();
            release_future.wait();
        });
        started_future.wait();

        auto timed_out = ex.submit_after_with_handle(
            std::vector<kairo::TaskHandle>{gate.handle},
            kairo::task([] { ADM_CHECK(false && "timed-out task must not run"); })
                .subject("A"));
        ADM_CHECK(ex.get_subject_in_flight_submissions("A") == 1);

        ADM_CHECK(settled_soon(timed_out.future, std::chrono::seconds(2)));
        bool saw_timeout = false;
        try {
            (void)timed_out.future.get();
        } catch (const kairo::TimedOutException&) {
            saw_timeout = true;
        }
        ADM_CHECK(saw_timeout);
        ADM_CHECK(ex.get_subject_in_flight_submissions("A") == 0);

        release.set_value();
        gate.future.get();
        // 超时释放后同份额可再接纳。
        ADM_CHECK(ex.submit_auto(kairo::task([] { return 3; }).subject("A"))
                      .get() == 3);
        ADM_CHECK(ex.get_subject_in_flight_submissions("A") == 0);
        ex.shutdown();
    }

    // 验收 3（末）：shutdown(false) 终局——池严格排空语义下排队中的 keyed
    // 任务可能仍被执行（正常释放）或被丢弃（AdmissionReleaser 析构兜底
    // 释放）；不变式：终局后分桶计数无泄漏归零，两种实现路径皆然。
    {
        Executor ex;
        ExecutorConfig config;
        config.min_threads = 1;
        config.max_threads = 1;
        ADM_CHECK(ex.initialize(config));
        ex.set_subject_max_in_flight_tasks("A", 2);

        // 运行中的有界 sleep 占住 worker，keyed 任务提交后保持排队。
        auto sleeper = ex.submit([] {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        });
        auto queued = ex.submit_auto(kairo::task([] {}).subject("A"));
        ADM_CHECK(ex.get_subject_in_flight_submissions("A") == 1);

        // stop(false) 立即返回，退休池由 detached 终结线程排空/join。
        const auto result = ex.shutdown(false);
        ADM_CHECK(result == kairo::ShutdownResult::Completed ||
                  result == kairo::ShutdownResult::RequestedFromWorker);
        for (int i = 0; i < 100 &&
                        ex.get_subject_in_flight_submissions("A") != 0;
             ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ADM_CHECK(ex.get_subject_in_flight_submissions("A") == 0);
        (void)sleeper;
        (void)queued;  // 池终局路径不承诺 queued future 的可用形态。
    }

    // 验收 5：份额热更新——调小不驱逐已接纳任务；set(subject, 0) 解除后
    // 不再约束；解除后重新配置产生新代计数（从 0 起计）。
    {
        Executor ex;
        ExecutorConfig config;
        config.min_threads = 1;
        config.max_threads = 1;
        ADM_CHECK(ex.initialize(config));

        ex.set_subject_max_in_flight_tasks("A", 2);
        std::promise<void> release;
        auto release_future = release.get_future().share();
        std::promise<void> started;
        auto started_future = started.get_future();

        std::vector<std::future<void>> blocked;
        blocked.push_back(ex.submit_auto(kairo::task([&] {
            started.set_value();
            release_future.wait();
        }).subject("A")));
        blocked.push_back(ex.submit_auto(
            kairo::task([&] { release_future.wait(); }).subject("A")));
        started_future.wait();
        ADM_CHECK(ex.get_subject_in_flight_submissions("A") == 2);

        // 调小到 1：已接纳的 2 个不被驱逐，只约束后续提交。
        ex.set_subject_max_in_flight_tasks("A", 1);
        ADM_CHECK(ex.get_subject_max_in_flight_tasks("A") == 1);
        ADM_CHECK(ex.get_subject_in_flight_submissions("A") == 2);
        auto a3 = ex.submit_auto(kairo::task([] {
                                     ADM_CHECK(false && "rejected task must not run");
                                 }).subject("A"));
        ADM_CHECK(settled_soon(a3, std::chrono::seconds(2)));
        const std::string a3_message = capacity_rejection_message(a3);
        ADM_CHECK(a3_message.find("subject_max_in_flight=1") != std::string::npos);

        // set(A, 0) 解除：不再约束（全局未启用，第 3 个可接纳）；键解除后
        // 不可查询，get 返回 0。
        ex.set_subject_max_in_flight_tasks("A", 0);
        ADM_CHECK(ex.get_subject_max_in_flight_tasks("A") == 0);
        ADM_CHECK(ex.get_subject_quota_status().empty());
        auto a4 = ex.submit_auto(
            kairo::task([&] { release_future.wait(); }).subject("A"));
        ADM_CHECK(ex.get_subject_in_flight_submissions("A") == 0);

        // 解除后重新配置：新代计数从 0 起（孤儿释放不回落新代）。
        ex.set_subject_max_in_flight_tasks("A", 1);
        ADM_CHECK(ex.get_subject_in_flight_submissions("A") == 0);
        auto a5 = ex.submit_auto(
            kairo::task([&] { release_future.wait(); }).subject("A"));
        ADM_CHECK(ex.get_subject_in_flight_submissions("A") == 1);
        auto a6 = ex.submit_auto(kairo::task([] {
                                     ADM_CHECK(false && "rejected task must not run");
                                 }).subject("A"));
        ADM_CHECK(settled_soon(a6, std::chrono::seconds(2)));
        const std::string a6_message = capacity_rejection_message(a6);
        ADM_CHECK(a6_message.find("key=A") != std::string::npos);

        release.set_value();
        for (auto& future : blocked) future.get();
        a4.get();
        a5.get();
        ADM_CHECK(ex.get_subject_in_flight_submissions("A") == 0);
        ex.shutdown();
    }

    // 验收 6：未配置主体仅受全局约束（"未知即宽容"）；全局启用而份额未触
    // 时带键提交不受未配置键限制；全局未启用（另一场景见验收 1）或低位
    // 全局下已配置主体的份额独立生效；未带键路径不受分桶约束。
    {
        Executor ex;
        ExecutorConfig config;
        config.min_threads = 1;
        config.max_threads = 1;
        config.max_in_flight_tasks = 10;
        ADM_CHECK(ex.initialize(config));

        // 未配置主体：无份额限制（连续 5 个带未配置键的提交全部接纳）。
        for (int i = 0; i < 5; ++i) {
            auto future = ex.submit_auto(
                kairo::task([i] { return i; }).subject("never-configured"));
            ADM_CHECK(future.get() == i);
        }
        ADM_CHECK(ex.get_subject_in_flight_submissions("never-configured") == 0);
        ADM_CHECK(ex.get_subject_quota_status().empty());

        // 已配置主体份额独立生效：A 份额 1，第 2 个并发被分桶拒绝
        //（全局上限 10 未触）。
        ex.set_subject_max_in_flight_tasks("A", 1);
        std::promise<void> release;
        auto release_future = release.get_future().share();
        std::promise<void> started;
        auto started_future = started.get_future();
        auto a1 = ex.submit_auto(kairo::task([&] {
            started.set_value();
            release_future.wait();
        }).subject("A"));
        started_future.wait();
        ADM_CHECK(ex.get_subject_in_flight_submissions("A") == 1);
        ADM_CHECK(ex.get_in_flight_submissions() == 1);

        auto a2 = ex.submit_auto(kairo::task([] {
                                     ADM_CHECK(false && "rejected task must not run");
                                 }).subject("A"));
        ADM_CHECK(settled_soon(a2, std::chrono::seconds(2)));
        const std::string a2_message = capacity_rejection_message(a2);
        ADM_CHECK(a2_message.find("key=A") != std::string::npos);
        const auto failure_status = ex.get_failure_status();
        ADM_CHECK(failure_status.subject_quota_exhausted_count == 1);
        ADM_CHECK(failure_status.capacity_exhausted_count == 1);

        // 未带键路径（空键）不触碰分桶注册表：A 桶满时仍被全局接纳。
        auto unkeyed = ex.submit([] { return 99; });
        ADM_CHECK(ex.get_in_flight_submissions() == 2);

        release.set_value();
        a1.get();
        ADM_CHECK(unkeyed.get() == 99);
        ADM_CHECK(ex.get_in_flight_submissions() == 0);
        ADM_CHECK(ex.get_subject_in_flight_submissions("A") == 0);
        ex.shutdown();
    }

    // 验收 7：shutdown(true) 排空后分桶计数归零；shutdown 后带键提交的
    // 拒绝语义与总量一致（stopping 拒绝，无 subject 计数残留）。
    {
        Executor ex;
        ExecutorConfig config;
        config.min_threads = 2;
        config.max_threads = 2;
        ADM_CHECK(ex.initialize(config));
        // 份额 >= 提交数：8 个带键快速任务全部接纳（接纳计数含排队，
        // 异步完成释放前提交数不得超过份额），排空后计数归零。
        ex.set_subject_max_in_flight_tasks("A", 8);

        std::vector<std::future<int>> futures;
        for (int i = 0; i < 8; ++i) {
            futures.push_back(
                ex.submit_auto(kairo::task([i] { return i; }).subject("A")));
        }
        for (auto& future : futures) {
            ADM_CHECK(future.get() >= 0);
        }
        ADM_CHECK(ex.get_subject_in_flight_submissions("A") == 0);

        const auto result = ex.shutdown(true);
        ADM_CHECK(result == kairo::ShutdownResult::Completed);
        ADM_CHECK(ex.get_subject_in_flight_submissions("A") == 0);
        for (const auto& status : ex.get_subject_quota_status()) {
            ADM_CHECK(status.in_flight_submissions == 0);
        }

        // shutdown 后带键提交：与不带键相同的 stopping 拒绝（SubmitRejected
        // 诊断），不产生容量类拒绝、无 subject 计数残留。
        bool keyed_stop_rejected = false;
        try {
            (void)ex.submit_auto(kairo::task([] {}).subject("A"));
        } catch (const std::runtime_error&) {
            keyed_stop_rejected = true;
        }
        bool unkeyed_stop_rejected = false;
        try {
            (void)ex.submit([] {});
        } catch (const std::runtime_error&) {
            unkeyed_stop_rejected = true;
        }
        ADM_CHECK(keyed_stop_rejected);
        ADM_CHECK(unkeyed_stop_rejected);
        ADM_CHECK(ex.get_subject_in_flight_submissions("A") == 0);
        const auto failure_status = ex.get_failure_status();
        ADM_CHECK(failure_status.subject_quota_exhausted_count == 0);
        ADM_CHECK(failure_status.capacity_exhausted_count == 0);
        ADM_CHECK(failure_status.submit_rejected_count >= 2);
    }

    // 验收 8：零回归锚——不配置任何份额、不设 subject 时行为与现状一致
    // （复用 test_bounded_admission 最小场景：容量 3 拒绝第 4 个）。
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
        ADM_CHECK(ex.get_in_flight_submissions() == 3);

        auto rejected = ex.submit([] { return 7; });
        ADM_CHECK(settled_soon(rejected, std::chrono::seconds(2)));
        bool saw_capacity = false;
        try {
            (void)rejected.get();
        } catch (const CapacityExhaustedException&) {
            saw_capacity = true;
        }
        ADM_CHECK(saw_capacity);
        // 带未配置键的提交同样只按总量语义拒绝（分桶维度未参与）。
        auto keyed = ex.submit_auto(
            kairo::task([] { ADM_CHECK(false && "rejected task must not run"); })
                .subject("unconfigured"));
        ADM_CHECK(settled_soon(keyed, std::chrono::seconds(2)));
        const std::string keyed_message = capacity_rejection_message(keyed);
        ADM_CHECK(keyed_message.find("In-flight submission capacity exhausted") !=
                  std::string::npos);
        ADM_CHECK(keyed_message.find("subject_max_in_flight") == std::string::npos);

        const auto failure_status = ex.get_failure_status();
        ADM_CHECK(failure_status.capacity_exhausted_count == 2);
        ADM_CHECK(failure_status.subject_quota_exhausted_count == 0);
        ADM_CHECK(ex.get_subject_quota_status().empty());
        ADM_CHECK(ex.get_subject_max_in_flight_tasks("unconfigured") == 0);

        release.set_value();
        for (auto& future : blocked) future.get();
        ADM_CHECK(ex.get_in_flight_submissions() == 0);
        ex.shutdown();
    }

    std::printf("test_subject_bucket_admission: all acceptance blocks passed\n");
    return 0;
}
