// v0.6.1 Scheduling Runtime 稳定化契约测试
// (docs/design/scheduling_runtime.md §3 / §6)。
//
// 覆盖（独立验证代理编写，不修改库代码）：
//  A. 结构化路由决策：DefaultScheduler 单元级 status/reason/diagnostics
//     （deadline 过期/边界锚点、GPU device/memory 资源拒绝、未知内存宽容、
//     affinity 结构化降级、拒绝不附加 affinity 诊断）+ Executor 端到端。
//  B. SchedulingMetrics：accepted / rejected / deadline_rejected /
//     accepted_degraded / affinity_mismatch / 观测开关解耦 /
//     deadline miss 计数（任务仍执行）。
//  C. SchedulingFeedback 契约：wants_feedback 开关、成功/失败反馈字段、
//     回调异常隔离、默认零开销路径、set_scheduler(nullptr) 后停止。
//  D. 生命周期与并发：注入时序、nullptr 恢复、调度器所有权与析构、
//     高并发 route() 精确计数、shutdown 与在途提交竞争、
//     0.6.0 旧式拒绝 reason 的 status 归一化。
//  E. EDF 与 deadline 稳定性：交错 deadline 升序执行、无 deadline 任务
//     排后；同 deadline 批次 FIFO 稳定、无死锁。

#include <gtest/gtest.h>

#include <kairo/executor.hpp>
#include <kairo/scheduler.hpp>
#include <kairo/scheduling.hpp>
#include <kairo/task_options.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace kairo;

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

ExecutorCapability make_cpu_capability() {
    ExecutorCapability capability;
    capability.backend = ExecutionBackend::DefaultAsync;
    capability.name = "default";
    capability.registered = true;
    capability.running = true;
    capability.supports_future_submission = true;
    return capability;
}

ExecutorCapability make_gpu_capability(int device,
                                       size_t total_bytes = 0,
                                       size_t free_bytes = 0) {
    ExecutorCapability capability;
    capability.backend = ExecutionBackend::Gpu;
    capability.name = "gpu" + std::to_string(device);
    capability.registered = true;
    capability.running = true;
    capability.supports_gpu_kernel = true;
    capability.gpu_device = device;
    capability.gpu_memory_total_bytes = total_bytes;
    capability.gpu_memory_free_bytes = free_bytes;
    return capability;
}

TaskRouter::Request make_request(TaskOptions options, bool cpu_gpu_task) {
    return TaskRouter::Request{std::move(options), cpu_gpu_task, std::nullopt};
}

// 初始化带指定线程数的默认池（遵循 tests/test_scheduling_runtime.cpp 约定）。
void init_executor(Executor& executor, size_t threads) {
    ExecutorConfig config;
    config.min_threads = threads;
    config.max_threads = threads;
    ASSERT_TRUE(executor.initialize(config));
}

// 有界轮询等待一个 atomic 标志（防止门控任务未启动时无限自旋）。
bool wait_for_flag(const std::atomic<bool>& flag, milliseconds timeout) {
    const auto deadline = Clock::now() + timeout;
    while (!flag.load(std::memory_order_acquire)) {
        if (Clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(milliseconds(1));
    }
    return true;
}

// route() 计数 + 委托 DefaultScheduler 的记录调度器（并发安全）。
class RecordingScheduler final : public IScheduler {
public:
    RoutingDecision route(const TaskRouter::Request& request,
                          const std::vector<ExecutorCapability>& capabilities) override {
        routes_.fetch_add(1, std::memory_order_relaxed);
        return default_.route(request, capabilities);
    }

    int route_count() const noexcept {
        return routes_.load(std::memory_order_relaxed);
    }

private:
    DefaultScheduler default_;
    std::atomic<int> routes_{0};
};

// 记录 SchedulingFeedback 到加锁 vector 的调度器（wants_feedback = true）。
class FeedbackScheduler final : public IScheduler {
public:
    bool wants_feedback() const noexcept override { return true; }

    RoutingDecision route(const TaskRouter::Request& request,
                          const std::vector<ExecutorCapability>& capabilities) override {
        return default_.route(request, capabilities);
    }

    void on_task_completed(const SchedulingFeedback& feedback) override {
        std::lock_guard<std::mutex> lock(mutex_);
        feedbacks_.push_back(feedback);
    }

    std::vector<SchedulingFeedback> snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return feedbacks_;
    }

private:
    DefaultScheduler default_;
    mutable std::mutex mutex_;
    std::vector<SchedulingFeedback> feedbacks_;
};

// 反馈回调抛异常的调度器：验证异常隔离与 feedback 计数不受影响。
class ThrowingFeedbackScheduler final : public IScheduler {
public:
    bool wants_feedback() const noexcept override { return true; }

    RoutingDecision route(const TaskRouter::Request& request,
                          const std::vector<ExecutorCapability>& capabilities) override {
        return default_.route(request, capabilities);
    }

    void on_task_completed(const SchedulingFeedback&) override {
        throw std::runtime_error("feedback consumer is broken");
    }

private:
    DefaultScheduler default_;
};

// 0.6.0 风格自定义调度器：只设拒绝类 reason，不设 status（默认 Accepted）。
class LegacyRejectingScheduler final : public IScheduler {
public:
    RoutingDecision route(const TaskRouter::Request& request,
                          const std::vector<ExecutorCapability>&) override {
        RoutingDecision decision;
        decision.task_name = request.options.name;  // 与 DefaultScheduler 对齐
        decision.reason = RoutingReason::BackendUnavailable;  // 拒绝类 reason
        decision.status = RoutingStatus::Accepted;            // 0.6.0 风格：未设 status
        decision.detail = "legacy scheduler lost its gpu backend";
        return decision;
    }
};

// 析构时置位外部标志（观察 Executor 对调度器的所有权与生命周期）。
class DestroyFlagScheduler final : public IScheduler {
public:
    explicit DestroyFlagScheduler(std::shared_ptr<std::atomic<bool>> destroyed)
        : destroyed_(std::move(destroyed)) {}

    ~DestroyFlagScheduler() override {
        destroyed_->store(true, std::memory_order_release);
    }

    RoutingDecision route(const TaskRouter::Request& request,
                          const std::vector<ExecutorCapability>& capabilities) override {
        return default_.route(request, capabilities);
    }

private:
    DefaultScheduler default_;
    std::shared_ptr<std::atomic<bool>> destroyed_;
};

}  // namespace

// ---------------------------------------------------------------------------
// A.1 结构化决策：DefaultScheduler 单元级（不经过 Executor）
// ---------------------------------------------------------------------------

TEST(StructuredDecision, DefaultOptionsAcceptedWithDefaultPolicy) {
    DefaultScheduler scheduler;
    TaskOptions options;
    options.name = "plain-auto";

    const auto decision = scheduler.route(make_request(options, false), {});
    EXPECT_EQ(decision.status, RoutingStatus::Accepted);
    EXPECT_EQ(decision.reason, RoutingReason::DefaultPolicy);
    EXPECT_EQ(decision.diagnostics, RoutingDiagnostics::None);
    EXPECT_EQ(decision.selected_backend, ExecutionBackend::DefaultAsync);
}

TEST(StructuredDecision, ExpiredDeadlineRejectedWithDeadlineExpired) {
    DefaultScheduler scheduler;
    TaskOptions options;
    options.name = "expired";
    options.deadline = Clock::now() - milliseconds(1);

    const auto decision = scheduler.route(make_request(options, false), {});
    EXPECT_EQ(decision.status, RoutingStatus::Rejected);
    EXPECT_EQ(decision.reason, RoutingReason::DeadlineExpired);
    EXPECT_NE(decision.detail.find("deadline"), std::string::npos)
        << "detail: " << decision.detail;
}

// 边界说明：契约是"严格大于才拒"（now > deadline 才拒绝，恰好相等接受）。
// deadline == now 的用例在 route() 内部会再次读时钟，测量耗时本身决定
// 结果，属于固有竞态；这里用两个确定性锚点代替边界本身：
//   now + 50ms 一定接受；now - 1ms 一定以 DeadlineExpired 拒绝。
TEST(StructuredDecision, DeadlineBoundaryAnchorsAreDeterministic) {
    DefaultScheduler scheduler;

    TaskOptions in_time;
    in_time.name = "in-time-anchor";
    in_time.deadline = Clock::now() + milliseconds(50);
    const auto accepted = scheduler.route(make_request(in_time, false), {});
    EXPECT_EQ(accepted.status, RoutingStatus::Accepted);
    EXPECT_NE(accepted.reason, RoutingReason::DeadlineExpired);

    TaskOptions expired;
    expired.name = "expired-anchor";
    expired.deadline = Clock::now() - milliseconds(1);
    const auto rejected = scheduler.route(make_request(expired, false), {});
    EXPECT_EQ(rejected.status, RoutingStatus::Rejected);
    EXPECT_EQ(rejected.reason, RoutingReason::DeadlineExpired);
}

TEST(StructuredDecision, GpuDeviceMismatchRejectedWithResourceBit) {
    DefaultScheduler scheduler;
    TaskOptions options;
    options.name = "gpu-wrong-device";
    options.intent = ExecutionIntent::CpuOrGpu;
    options.resources.gpu_device = 1;

    const std::vector<ExecutorCapability> capabilities{
        make_cpu_capability(), make_gpu_capability(0)};
    const auto decision = scheduler.route(make_request(options, true), capabilities);
    EXPECT_EQ(decision.status, RoutingStatus::Rejected);
    EXPECT_EQ(decision.reason, RoutingReason::BackendUnavailable);
    EXPECT_NE(decision.diagnostics & RoutingDiagnostics::ResourceInfeasible, 0u)
        << "device mismatch must carry ResourceInfeasible diagnostic bit";
}

TEST(StructuredDecision, GpuMemoryOverAvailabilityRejectedWithResourceBit) {
    DefaultScheduler scheduler;
    TaskOptions options;
    options.name = "gpu-memory-over";
    options.intent = ExecutionIntent::CpuOrGpu;
    options.resources.memory_bytes = 2048;  // > free 1024

    const std::vector<ExecutorCapability> capabilities{
        make_cpu_capability(), make_gpu_capability(0, /*total=*/4096, /*free=*/1024)};
    const auto decision = scheduler.route(make_request(options, true), capabilities);
    EXPECT_EQ(decision.status, RoutingStatus::Rejected);
    EXPECT_EQ(decision.reason, RoutingReason::CapacityPressure);
    EXPECT_NE(decision.diagnostics & RoutingDiagnostics::ResourceInfeasible, 0u)
        << "memory over-claim must carry ResourceInfeasible diagnostic bit";
}

TEST(StructuredDecision, UnknownGpuMemoryTotalIsPermissive) {
    DefaultScheduler scheduler;
    TaskOptions options;
    options.name = "gpu-memory-unknown";
    options.intent = ExecutionIntent::CpuOrGpu;
    options.resources.memory_bytes = 1u << 30;  // 巨额声明；总量未知须跳过检查

    const std::vector<ExecutorCapability> capabilities{
        make_cpu_capability(), make_gpu_capability(0, /*total=*/0, /*free=*/0)};
    const auto decision = scheduler.route(make_request(options, true), capabilities);
    EXPECT_EQ(decision.status, RoutingStatus::Accepted)
        << "unknown gpu memory total must not reject (permissive fallback)";
    EXPECT_EQ(decision.diagnostics, RoutingDiagnostics::None);
}

TEST(StructuredDecision, DisjointAffinityAcceptedDegradedWithBit) {
    DefaultScheduler scheduler;
    TaskOptions options;
    options.name = "affinity-disjoint";
    options.affinity = AffinityHint{{9999}};

    ExecutorCapability cpu = make_cpu_capability();
    cpu.bound_cpus = {0, 1};
    const std::vector<ExecutorCapability> capabilities{cpu};

    const auto decision = scheduler.route(make_request(options, false), capabilities);
    EXPECT_NE(decision.status, RoutingStatus::Rejected)
        << "affinity mismatch is advisory: task must still be accepted";
    EXPECT_EQ(decision.status, RoutingStatus::AcceptedDegraded);
    EXPECT_EQ(decision.reason, RoutingReason::AffinityMismatch);
    EXPECT_NE(decision.diagnostics & RoutingDiagnostics::AffinityMismatch, 0u);
    EXPECT_NE(decision.detail.find("AffinityMismatch"), std::string::npos)
        << "detail: " << decision.detail;
}

TEST(StructuredDecision, IntersectingAffinityAcceptedWithoutDiagnostics) {
    DefaultScheduler scheduler;
    TaskOptions options;
    options.name = "affinity-intersect";
    options.affinity = AffinityHint{{0}};

    ExecutorCapability cpu = make_cpu_capability();
    cpu.bound_cpus = {0, 1};
    const std::vector<ExecutorCapability> capabilities{cpu};

    const auto decision = scheduler.route(make_request(options, false), capabilities);
    EXPECT_EQ(decision.status, RoutingStatus::Accepted);
    EXPECT_EQ(decision.reason, RoutingReason::DefaultPolicy);
    EXPECT_EQ(decision.diagnostics, RoutingDiagnostics::None);
}

TEST(StructuredDecision, RejectedDecisionGainsNoAffinityDiagnostics) {
    DefaultScheduler scheduler;
    TaskOptions options;
    options.name = "expired-with-affinity";
    options.deadline = Clock::now() - milliseconds(1);
    options.affinity = AffinityHint{{9999}};  // 与 bound {0,1} 不相交

    ExecutorCapability cpu = make_cpu_capability();
    cpu.bound_cpus = {0, 1};
    const std::vector<ExecutorCapability> capabilities{cpu};

    const auto decision = scheduler.route(make_request(options, false), capabilities);
    EXPECT_EQ(decision.status, RoutingStatus::Rejected);
    EXPECT_EQ(decision.reason, RoutingReason::DeadlineExpired);
    EXPECT_EQ(decision.diagnostics, RoutingDiagnostics::None)
        << "rejected decisions must not carry affinity diagnostics";
}

TEST(StructuredDecision, StatusAndReasonStableNames) {
    EXPECT_STREQ(routing_status_to_string(RoutingStatus::Accepted), "Accepted");
    EXPECT_STREQ(routing_status_to_string(RoutingStatus::AcceptedDegraded),
                 "AcceptedDegraded");
    EXPECT_STREQ(routing_status_to_string(RoutingStatus::Rejected), "Rejected");

    EXPECT_STREQ(routing_reason_to_string(RoutingReason::DeadlineExpired),
                 "DeadlineExpired");
    EXPECT_STREQ(routing_reason_to_string(RoutingReason::AffinityMismatch),
                 "AffinityMismatch");
    EXPECT_STREQ(routing_reason_to_string(RoutingReason::DefaultPolicy),
                 "DefaultPolicy");
    EXPECT_STREQ(routing_reason_to_string(RoutingReason::BackendUnavailable),
                 "BackendUnavailable");
}

// ---------------------------------------------------------------------------
// A.2 结构化决策：Executor 端到端
// ---------------------------------------------------------------------------

TEST(StructuredDecisionEndToEnd, DisjointAffinitySubmitsAndRecordsDegraded) {
    // 默认池绑核 auto-allocate 为 [0..hw-1]；hw == 0 时能力快照 bound_cpus
    // 为空、affinity 检查跳过，本用例的前提即不成立。
    ASSERT_GT(std::thread::hardware_concurrency(), 0u);

    Executor executor;
    init_executor(executor, 2);

    auto future = executor.submit_auto(
        task([] {}).name("e2e-affinity-far").affinity(AffinityHint{{9999}}));
    EXPECT_NO_THROW(future.get()) << "advisory affinity mismatch must not reject";

    bool found = false;
    for (const auto& decision : executor.get_recent_routing_decisions()) {
        if (decision.task_name != "e2e-affinity-far") {
            continue;
        }
        found = true;
        EXPECT_EQ(decision.status, RoutingStatus::AcceptedDegraded);
        EXPECT_EQ(decision.reason, RoutingReason::AffinityMismatch);
        EXPECT_NE(decision.diagnostics & RoutingDiagnostics::AffinityMismatch, 0u);
    }
    EXPECT_TRUE(found) << "routing decision for e2e-affinity-far not recorded";
}

TEST(StructuredDecisionEndToEnd, ExpiredDeadlineRejectedViaFuture) {
    Executor executor;
    init_executor(executor, 2);

    auto future = executor.submit_auto(
        task([] { FAIL() << "rejected task must never execute"; })
            .name("e2e-expired")
            .deadline(Clock::now() - milliseconds(10)));

    bool threw = false;
    std::string message;
    try {
        future.get();
    } catch (const std::runtime_error& error) {
        threw = true;
        message = error.what();
    }
    EXPECT_TRUE(threw) << "expired deadline must reject via exception-ready future";
    EXPECT_NE(message.find("deadline"), std::string::npos) << "message: " << message;
}

// ---------------------------------------------------------------------------
// B. SchedulingMetrics 计数（Executor 端到端，基线差值断言）
// ---------------------------------------------------------------------------

TEST(SchedulingMetricsContract, PlainSubmissionsCountAccepted) {
    Executor executor;
    init_executor(executor, 2);
    const auto before = executor.get_scheduling_metrics();

    constexpr int kSubmissions = 5;
    std::vector<std::future<void>> futures;
    for (int i = 0; i < kSubmissions; ++i) {
        futures.push_back(executor.submit_auto(task([] {})));
    }
    for (auto& future : futures) {
        EXPECT_NO_THROW(future.get());
    }

    const auto after = executor.get_scheduling_metrics();
    EXPECT_EQ(after.accepted_count - before.accepted_count, 5u);
    EXPECT_EQ(after.rejected_count, before.rejected_count);
    EXPECT_EQ(after.accepted_degraded_count, before.accepted_degraded_count);
}

TEST(SchedulingMetricsContract, ExpiredDeadlineSubmissionsCountRejected) {
    Executor executor;
    init_executor(executor, 2);
    const auto before = executor.get_scheduling_metrics();

    constexpr int kSubmissions = 3;
    for (int i = 0; i < kSubmissions; ++i) {
        auto future = executor.submit_auto(
            task([] {}).deadline(Clock::now() - milliseconds(10)));
        EXPECT_THROW(future.get(), std::runtime_error);
    }

    const auto after = executor.get_scheduling_metrics();
    EXPECT_EQ(after.rejected_count - before.rejected_count, 3u);
    EXPECT_EQ(after.deadline_rejected_count - before.deadline_rejected_count, 3u);
    EXPECT_EQ(after.accepted_count, before.accepted_count);
}

TEST(SchedulingMetricsContract, AffinityMismatchCountsDegradedAndDiagnostics) {
    ASSERT_GT(std::thread::hardware_concurrency(), 0u);

    Executor executor;
    init_executor(executor, 2);
    const auto before = executor.get_scheduling_metrics();

    auto future = executor.submit_auto(
        task([] {}).name("metrics-affinity").affinity(AffinityHint{{9999}}));
    EXPECT_NO_THROW(future.get());

    const auto after = executor.get_scheduling_metrics();
    EXPECT_EQ(after.accepted_degraded_count - before.accepted_degraded_count, 1u);
    EXPECT_EQ(after.affinity_mismatch_count - before.affinity_mismatch_count, 1u);
    EXPECT_EQ(after.accepted_count, before.accepted_count);
    EXPECT_EQ(after.rejected_count, before.rejected_count);
}

TEST(SchedulingMetricsContract, MetricsAccumulateWhenRoutingObservationOff) {
    Executor executor;
    init_executor(executor, 2);
    const auto before = executor.get_scheduling_metrics();

    executor.set_recent_routing_capacity(0);  // CR-106：关闭观测缓冲

    std::vector<std::future<void>> futures;
    for (int i = 0; i < 3; ++i) {
        futures.push_back(executor.submit_auto(task([] {})));
    }
    for (auto& future : futures) {
        EXPECT_NO_THROW(future.get());
    }

    const auto after = executor.get_scheduling_metrics();
    EXPECT_EQ(after.accepted_count - before.accepted_count, 3u)
        << "metrics must accumulate unconditionally (decoupled from observation)";
    EXPECT_TRUE(executor.get_recent_routing_decisions().empty())
        << "capacity 0 must retain no decisions buffer";
}

TEST(SchedulingMetricsContract, DeadlineMissCountsWhileTaskStillExecutes) {
    Executor executor;
    init_executor(executor, 2);
    const auto before = executor.get_scheduling_metrics();
    const auto failures_before = executor.get_failure_status().deadline_missed_count;

    // 两个 30ms 阻塞任务占满 2 个 worker；三者共享 deadline（now+5ms），
    // 同 deadline 退化为 FIFO，最后提交的目标任务必在错过后才开始执行
    //（"记录 miss 但执行"契约）。注意阻塞任务也必须携带 deadline：
    // EDF 下有 deadline 的任务会排到无 deadline 任务之前，否则目标任务
    // 会跳队立即执行、不产生 miss（已用独立探针复现）。时间余量按 ≥2ms
    // 原则放大：deadline 5ms / 阻塞 30ms。
    const auto shared_deadline = Clock::now() + milliseconds(5);
    std::vector<std::future<void>> futures;
    for (int i = 0; i < 2; ++i) {
        futures.push_back(executor.submit_auto(
            task([] { std::this_thread::sleep_for(milliseconds(30)); })
                .name("miss-blocker")
                .deadline(shared_deadline)));
    }
    std::atomic<bool> target_ran{false};
    futures.push_back(executor.submit_auto(
        task([&target_ran] {
            std::this_thread::sleep_for(milliseconds(2));
            target_ran.store(true, std::memory_order_release);
        })
            .name("miss-target")
            .deadline(shared_deadline)));

    for (auto& future : futures) {
        EXPECT_NO_THROW(future.get()) << "missed deadline must not interrupt execution";
    }
    EXPECT_TRUE(target_ran.load());

    const auto after = executor.get_scheduling_metrics();
    EXPECT_GE(after.deadline_missed_count - before.deadline_missed_count, 1u);
    EXPECT_GE(executor.get_failure_status().deadline_missed_count - failures_before, 1u);
}

// ---------------------------------------------------------------------------
// C. Feedback 契约
// ---------------------------------------------------------------------------

namespace {
class BareScheduler final : public IScheduler {
public:
    RoutingDecision route(const TaskRouter::Request&,
                          const std::vector<ExecutorCapability>&) override {
        return RoutingDecision{};
    }
};
}  // namespace

TEST(FeedbackContract, WantsFeedbackDefaultsToFalse) {
    DefaultScheduler default_scheduler;
    EXPECT_FALSE(default_scheduler.wants_feedback());

    BareScheduler bare;  // 未 override wants_feedback 的实现
    EXPECT_FALSE(bare.wants_feedback());
}

TEST(FeedbackContract, SuccessfulTaskReportsCompleteFeedback) {
    Executor executor;
    init_executor(executor, 2);
    auto owned = std::make_unique<FeedbackScheduler>();
    FeedbackScheduler* scheduler = owned.get();
    executor.set_scheduler(std::move(owned));
    const auto before = executor.get_scheduling_metrics();

    auto future = executor.submit_auto(
        task([] {}).name("fb-ok").qos(QosClass::Interactive));
    EXPECT_NO_THROW(future.get());

    const auto feedbacks = scheduler->snapshot();
    ASSERT_EQ(feedbacks.size(), 1u) << "one executed task must produce one feedback";
    const auto& feedback = feedbacks.front();
    EXPECT_EQ(feedback.task_id, "fb-ok");
    EXPECT_EQ(feedback.qos, QosClass::Interactive);
    EXPECT_TRUE(feedback.success);
    EXPECT_EQ(feedback.failure_kind, FailureKind::None);
    EXPECT_EQ(feedback.backend, ExecutionBackend::DefaultAsync);
    EXPECT_EQ(feedback.executor_name, "default");
    EXPECT_GE(feedback.queue_wait_ns, 0);
    EXPECT_GE(feedback.execution_duration_ns, 0);
    EXPECT_FALSE(feedback.had_deadline);
    EXPECT_FALSE(feedback.deadline_missed);

    EXPECT_EQ(executor.get_scheduling_metrics().feedback_reported_count -
                  before.feedback_reported_count,
              1u);
}

TEST(FeedbackContract, FailingTaskReportsTaskExceptionAndFutureRethrows) {
    Executor executor;
    init_executor(executor, 2);
    auto owned = std::make_unique<FeedbackScheduler>();
    FeedbackScheduler* scheduler = owned.get();
    executor.set_scheduler(std::move(owned));

    auto future = executor.submit_auto(task([] {
        throw std::runtime_error("kaboom-original");
    }).name("fb-fail"));

    std::string message;
    try {
        future.get();
    } catch (const std::runtime_error& error) {
        message = error.what();
    }
    EXPECT_NE(message.find("kaboom-original"), std::string::npos)
        << "future must rethrow the original task exception";

    const auto feedbacks = scheduler->snapshot();
    ASSERT_EQ(feedbacks.size(), 1u);
    EXPECT_FALSE(feedbacks.front().success);
    EXPECT_EQ(feedbacks.front().failure_kind, FailureKind::TaskException);
    EXPECT_EQ(feedbacks.front().task_id, "fb-fail");
}

TEST(FeedbackContract, ThrowingFeedbackCallbackIsolatedAndStillCounted) {
    Executor executor;
    init_executor(executor, 2);
    executor.set_scheduler(std::make_unique<ThrowingFeedbackScheduler>());
    const auto before = executor.get_scheduling_metrics();

    std::atomic<int> executed{0};
    std::vector<std::future<void>> futures;
    for (int i = 0; i < 2; ++i) {
        futures.push_back(executor.submit_auto(
            task([&executed] { executed.fetch_add(1, std::memory_order_relaxed); })
                .name("fb-throw")));
    }
    for (auto& future : futures) {
        EXPECT_NO_THROW(future.get())
            << "throwing feedback callback must not affect workers";
    }
    EXPECT_EQ(executed.load(), 2);
    EXPECT_EQ(executor.get_scheduling_metrics().feedback_reported_count -
                  before.feedback_reported_count,
              2u)
        << "attempted feedback deliveries must be counted despite callback throw";
}

TEST(FeedbackContract, DefaultSchedulerKeepsFeedbackChannelIdle) {
    Executor executor;  // 未注入调度器：DefaultScheduler（wants_feedback = false）
    init_executor(executor, 2);

    std::vector<std::future<void>> futures;
    for (int i = 0; i < 5; ++i) {
        futures.push_back(executor.submit_auto(task([] {})));
    }
    for (auto& future : futures) {
        EXPECT_NO_THROW(future.get());
    }
    EXPECT_EQ(executor.get_scheduling_metrics().feedback_reported_count, 0u)
        << "default zero-overhead feedback path must report nothing";
}

TEST(FeedbackContract, FeedbackStopsAfterSchedulerReset) {
    Executor executor;
    init_executor(executor, 2);
    executor.set_scheduler(std::make_unique<FeedbackScheduler>());

    auto warm = executor.submit_auto(task([] {}).name("fb-before-reset"));
    EXPECT_NO_THROW(warm.get());
    const auto after_warm = executor.get_scheduling_metrics();
    EXPECT_EQ(after_warm.feedback_reported_count, 1u);

    executor.set_scheduler(nullptr);  // 恢复 DefaultScheduler，开关缓存随之更新

    std::vector<std::future<void>> futures;
    for (int i = 0; i < 2; ++i) {
        futures.push_back(executor.submit_auto(task([] {}).name("fb-after-reset")));
    }
    for (auto& future : futures) {
        EXPECT_NO_THROW(future.get());
    }
    EXPECT_EQ(executor.get_scheduling_metrics().feedback_reported_count,
              after_warm.feedback_reported_count)
        << "feedback must stop after set_scheduler(nullptr)";
}

// ---------------------------------------------------------------------------
// D. 生命周期与并发
// ---------------------------------------------------------------------------

TEST(SchedulerLifecycle, InjectedBeforeFirstSubmitHandlesRouting) {
    Executor executor;
    init_executor(executor, 2);
    auto owned = std::make_unique<RecordingScheduler>();
    RecordingScheduler* recorder = owned.get();
    executor.set_scheduler(std::move(owned));
    ASSERT_EQ(executor.get_scheduler(), recorder);

    for (int i = 0; i < 3; ++i) {
        auto future = executor.submit_auto(task([] {}).name("via-recording"));
        EXPECT_NO_THROW(future.get());
    }
    EXPECT_EQ(recorder->route_count(), 3)
        << "every submit_auto must route through the injected scheduler";
}

TEST(SchedulerLifecycle, NullptrRestoreKeepsDefaultRejectionSemantics) {
    Executor executor;
    init_executor(executor, 2);
    executor.set_scheduler(std::make_unique<RecordingScheduler>());
    executor.set_scheduler(nullptr);
    ASSERT_NE(executor.get_scheduler(), nullptr);

    auto future = executor.submit_auto(
        task([] {}).name("after-restore-expired")
            .deadline(Clock::now() - milliseconds(10)));
    EXPECT_THROW(future.get(), std::runtime_error)
        << "restored DefaultScheduler must still reject expired deadlines";
}

TEST(SchedulerLifecycle, SchedulerOwnershipTransfersAndDestroys) {
    auto first_destroyed = std::make_shared<std::atomic<bool>>(false);
    auto second_destroyed = std::make_shared<std::atomic<bool>>(false);
    {
        Executor executor;
        init_executor(executor, 2);

        executor.set_scheduler(
            std::make_unique<DestroyFlagScheduler>(first_destroyed));
        ASSERT_FALSE(first_destroyed->load())
            << "live scheduler must not be destroyed while held";

        executor.set_scheduler(
            std::make_unique<DestroyFlagScheduler>(second_destroyed));
        EXPECT_TRUE(first_destroyed->load())
            << "replaced scheduler must be destroyed on replacement";
        EXPECT_FALSE(second_destroyed->load());
    }  // Executor 析构
    EXPECT_TRUE(second_destroyed->load())
        << "Executor destruction must destroy the scheduler it owns";
}

TEST(SchedulerLifecycle, ConcurrentRouteCallsObservedExactly) {
    Executor executor;
    init_executor(executor, 4);
    auto owned = std::make_unique<RecordingScheduler>();
    RecordingScheduler* recorder = owned.get();
    executor.set_scheduler(std::move(owned));

    constexpr int kThreads = 8;
    constexpr int kPerThread = 250;
    constexpr int kTotal = kThreads * kPerThread;

    std::mutex futures_mutex;
    std::vector<std::future<void>> futures;
    std::vector<std::thread> producers;
    for (int t = 0; t < kThreads; ++t) {
        producers.emplace_back([&executor, &futures, &futures_mutex] {
            for (int i = 0; i < kPerThread; ++i) {
                auto future = executor.submit_auto(task([] {}));
                std::lock_guard<std::mutex> lock(futures_mutex);
                futures.push_back(std::move(future));
            }
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }

    EXPECT_EQ(recorder->route_count(), kTotal)
        << "route() must be called exactly once per submission";
    ASSERT_EQ(futures.size(), static_cast<size_t>(kTotal));
    for (auto& future : futures) {
        EXPECT_NO_THROW(future.get());
    }
    EXPECT_EQ(executor.get_scheduling_metrics().accepted_count,
              static_cast<uint64_t>(kTotal));
}

TEST(SchedulerLifecycle, ShutdownRacesWithConcurrentSubmissions) {
    Executor executor;
    init_executor(executor, 4);

    // 先投放 200 个在途任务，再让 shutdown 与持续提交并发。
    std::vector<std::future<void>> base_futures;
    for (int i = 0; i < 200; ++i) {
        base_futures.push_back(
            executor.submit_auto(task([] {}).name("race-base")));
    }

    std::vector<std::future<void>> late_futures;  // 仅提交线程访问
    std::atomic<int> submit_throws{0};
    auto submitter = std::async(std::launch::async, [&executor, &late_futures,
                                                     &submit_throws] {
        for (int i = 0; i < 500; ++i) {
            try {
                late_futures.push_back(
                    executor.submit_auto(task([] {}).name("race-late")));
            } catch (...) {
                // shutdown 后提交被同步拒绝是允许的终态
                submit_throws.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });
    auto stopper = std::async(std::launch::async,
                              [&executor] { executor.shutdown(true); });

    EXPECT_EQ(submitter.wait_for(milliseconds(20000)), std::future_status::ready)
        << "submitting thread must not hang across shutdown";
    EXPECT_EQ(stopper.wait_for(milliseconds(20000)), std::future_status::ready)
        << "shutdown must not hang while submissions are in flight";

    size_t completed = 0;
    size_t failed_ready = 0;
    auto verify = [&](std::future<void>& future) {
        ASSERT_EQ(future.wait_for(milliseconds(5000)), std::future_status::ready)
            << "every future must settle after shutdown (no leaking hang)";
        try {
            future.get();
            ++completed;
        } catch (...) {
            ++failed_ready;  // 异常就绪同样是合法终态
        }
    };
    for (auto& future : base_futures) {
        verify(future);
    }
    for (auto& future : late_futures) {
        verify(future);
    }
    EXPECT_EQ(completed + failed_ready, 200u + late_futures.size());
}

TEST(SchedulerLifecycle, LegacyRejectionReasonNormalizedToRejectedStatus) {
    Executor executor;
    init_executor(executor, 2);
    executor.set_scheduler(std::make_unique<LegacyRejectingScheduler>());
    const auto before = executor.get_scheduling_metrics();

    auto future = executor.submit_auto(task([] {}).name("legacy-reject"));

    std::string message;
    try {
        future.get();
    } catch (const std::runtime_error& error) {
        message = error.what();
    }
    EXPECT_FALSE(message.empty())
        << "0.6.0-style rejection reason must yield an exception-ready future";
    EXPECT_NE(message.find("legacy scheduler"), std::string::npos)
        << "message: " << message;

    const auto after = executor.get_scheduling_metrics();
    EXPECT_EQ(after.rejected_count - before.rejected_count, 1u);
    EXPECT_EQ(after.backend_unavailable_rejected_count -
                  before.backend_unavailable_rejected_count,
              1u);

    bool found = false;
    for (const auto& decision : executor.get_recent_routing_decisions()) {
        if (decision.task_name != "legacy-reject") {
            continue;
        }
        found = true;
        EXPECT_EQ(decision.status, RoutingStatus::Rejected)
            << "route_task() must normalize legacy reject-class reasons";
        EXPECT_EQ(decision.reason, RoutingReason::BackendUnavailable);
    }
    EXPECT_TRUE(found);
}

// ---------------------------------------------------------------------------
// E. EDF 与 deadline 稳定性（单 worker + 门控串行化开始）
// ---------------------------------------------------------------------------

TEST(EdfDeadlineStability, InterleavedDeadlinesExecuteInAscendingOrder) {
    Executor executor;
    init_executor(executor, 1);  // 单 worker：全部任务在全局调度器内排序

    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    bool gate_open = false;
    std::atomic<bool> gate_started{false};

    // 门控任务占住唯一 worker，确保后续任务在提交完成前排入调度器。
    auto gate = executor.submit_auto(task([&] {
        gate_started.store(true, std::memory_order_release);
        std::unique_lock<std::mutex> lock(gate_mutex);
        gate_cv.wait(lock, [&gate_open] { return gate_open; });
    }).name("edf-gate"));
    ASSERT_TRUE(wait_for_flag(gate_started, milliseconds(5000)))
        << "gate task never started; single-worker assumption broken";

    // 10 个提交：offsets 为 deadline 相对 base 的毫秒偏移，-1 表示无
    // deadline。deadline 交错（非单调）。1ms 间隔保证 FIFO 平局裁决的
    // submit_time 严格递增。
    const int offsets_ms[] = {70, 10, -1, 50, 20, -1, 80, 30, 60, 40};
    const auto base = Clock::now() + milliseconds(500);
    std::atomic<int> next_slot{0};
    std::vector<int> exec_order(10, -1);
    std::vector<std::future<void>> futures;
    for (int i = 0; i < 10; ++i) {
        auto builder = task([&next_slot, &exec_order, i] {
            exec_order[next_slot.fetch_add(1, std::memory_order_relaxed)] = i;
        }).name("edf-" + std::to_string(i));
        if (offsets_ms[i] >= 0) {
            builder.deadline(base + milliseconds(offsets_ms[i]));
        }
        futures.push_back(executor.submit_auto(std::move(builder)));
        std::this_thread::sleep_for(milliseconds(1));
    }

    {
        std::lock_guard<std::mutex> lock(gate_mutex);
        gate_open = true;
    }
    gate_cv.notify_all();

    ASSERT_NO_THROW(gate.get());
    for (auto& future : futures) {
        ASSERT_NO_THROW(future.get());
    }

    // deadline 升序：10(i1),20(i4),30(i7),40(i9),50(i3),60(i8),70(i0),80(i6)，
    // 之后无 deadline 任务按提交 FIFO：i2, i5。
    const std::vector<int> expected{1, 4, 7, 9, 3, 8, 0, 6, 2, 5};
    EXPECT_EQ(exec_order, expected)
        << "execution order must follow deadline-ascending EDF then FIFO";
}

TEST(EdfDeadlineStability, SameDeadlineBatchExecutesFifoWithoutDeadlock) {
    Executor executor;
    init_executor(executor, 1);

    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    bool gate_open = false;
    std::atomic<bool> gate_started{false};

    auto gate = executor.submit_auto(task([&] {
        gate_started.store(true, std::memory_order_release);
        std::unique_lock<std::mutex> lock(gate_mutex);
        gate_cv.wait(lock, [&gate_open] { return gate_open; });
    }).name("fifo-gate"));
    ASSERT_TRUE(wait_for_flag(gate_started, milliseconds(5000)));

    constexpr int kTasks = 64;
    const auto shared_deadline = Clock::now() + std::chrono::seconds(2);
    std::atomic<int> next_slot{0};
    std::vector<int> exec_order(kTasks, -1);
    std::vector<std::future<void>> futures;
    for (int i = 0; i < kTasks; ++i) {
        futures.push_back(executor.submit_auto(
            task([&next_slot, &exec_order, i] {
                exec_order[next_slot.fetch_add(1, std::memory_order_relaxed)] = i;
            })
                .name("fifo-" + std::to_string(i))
                .deadline(shared_deadline)));
        std::this_thread::sleep_for(milliseconds(1));  // 平局按提交 FIFO 裁决
    }

    {
        std::lock_guard<std::mutex> lock(gate_mutex);
        gate_open = true;
    }
    gate_cv.notify_all();

    ASSERT_NO_THROW(gate.get());
    for (auto& future : futures) {
        ASSERT_NO_THROW(future.get()) << "same-deadline batch must complete";
    }

    std::vector<int> expected(kTasks);
    for (int i = 0; i < kTasks; ++i) {
        expected[i] = i;
    }
    EXPECT_EQ(exec_order, expected)
        << "same-deadline tasks must execute in submission FIFO order";
}
