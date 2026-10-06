// 0.6.0 Scheduling Runtime 行为测试（docs/design/scheduling_runtime.md §5）。
//
// 覆盖：模型类型与 QoS→priority 映射、TaskBuilder 扩展、EDF 排序
// （Task::operator< + PriorityScheduler + 端到端 meta 进池）、deadline
// 准入拒绝（DefaultScheduler 单元 + submit_auto 端到端）、deadline 错过
// 观测（DeadlineMissed 事件 + deadline_missed_count）、QoS 端到端无回归、
// affinity advisory 诊断（Mismatch / 相交无警告）、resource 检查
// （GPU device / memory / 无 GPU 语义）、自定义 IScheduler 注入。
//
// 独立验证代理编写；不修改库代码。

#include <gtest/gtest.h>

#include <kairo/executor.hpp>
#include <kairo/scheduler.hpp>
#include <kairo/scheduling.hpp>
#include <kairo/task_options.hpp>

#include "kairo/task/task.hpp"
#include "kairo/thread_pool/priority_scheduler.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace kairo;

namespace {

using Clock = std::chrono::steady_clock;

int64_t steady_ns(Clock::time_point tp) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               tp.time_since_epoch())
        .count();
}

// Task 含 std::atomic 不可拷贝/移动，按既有测试约定就地填充。
void init_task(Task& task,
               const std::string& id,
               TaskPriority priority,
               int64_t submit_time_ns,
               int64_t deadline_ns = 0) {
    task.task_id = id;
    task.priority = priority;
    task.submit_time_ns = submit_time_ns;
    task.deadline_ns = deadline_ns;
    task.function = [] {};
}

// 独立验证用的 RecordingScheduler：委托 DefaultScheduler 并计数 route 调用。
// route() 会在多提交线程并发调用（§2.1 线程安全约定），计数用 atomic。
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

// 初始化带指定线程数的默认池（deadline 排队/错过测试需要可控并发度）。
void init_executor(Executor& executor, size_t threads) {
    ExecutorConfig config;
    config.min_threads = threads;
    config.max_threads = threads;
    ASSERT_TRUE(executor.initialize(config));
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. 模型类型（include/kairo/scheduling.hpp）
// ---------------------------------------------------------------------------

TEST(SchedulingModelTypes, QosClassValuesAndStableNames) {
    static_assert(static_cast<uint8_t>(QosClass::BestEffort) == 0);
    static_assert(static_cast<uint8_t>(QosClass::Standard) == 1);
    static_assert(static_cast<uint8_t>(QosClass::Interactive) == 2);
    static_assert(static_cast<uint8_t>(QosClass::Critical) == 3);

    EXPECT_STREQ(qos_class_to_string(QosClass::BestEffort), "BestEffort");
    EXPECT_STREQ(qos_class_to_string(QosClass::Standard), "Standard");
    EXPECT_STREQ(qos_class_to_string(QosClass::Interactive), "Interactive");
    EXPECT_STREQ(qos_class_to_string(QosClass::Critical), "Critical");
}

TEST(SchedulingModelTypes, StructDefaults) {
    AffinityHint affinity;
    EXPECT_TRUE(affinity.cpus.empty());

    ResourceRequirements resources;
    EXPECT_EQ(resources.memory_bytes, 0u);
    EXPECT_EQ(resources.gpu_device, -1);

    TaskSchedulingMeta meta;
    EXPECT_EQ(meta.deadline_ns, 0);
    EXPECT_EQ(meta.qos, QosClass::Standard);

    TaskOptions options;
    EXPECT_FALSE(options.deadline.has_value());
    EXPECT_EQ(options.qos, QosClass::Standard);
    EXPECT_EQ(options.priority, TaskPriority::NORMAL);
    EXPECT_FALSE(options.priority_set);
}

// ---------------------------------------------------------------------------
// 2. QoS → 默认优先级映射（task_options.hpp default_priority_for_qos）
// ---------------------------------------------------------------------------

TEST(SchedulingQosMapping, DefaultPriorityForAllQosClasses) {
    EXPECT_EQ(default_priority_for_qos(QosClass::BestEffort), TaskPriority::LOW);
    EXPECT_EQ(default_priority_for_qos(QosClass::Standard), TaskPriority::NORMAL);
    EXPECT_EQ(default_priority_for_qos(QosClass::Interactive), TaskPriority::HIGH);
    EXPECT_EQ(default_priority_for_qos(QosClass::Critical), TaskPriority::CRITICAL);
}

TEST(SchedulingQosMapping, TaskBuilderQosDoesNotSetPriorityFlag) {
    auto built = task([] {}).qos(QosClass::Interactive);
    EXPECT_FALSE(built.options().priority_set);
    EXPECT_EQ(built.options().qos, QosClass::Interactive);
    // 未显式设置 priority 时保持字段默认值；提交时才按 QoS 映射。
    EXPECT_EQ(built.options().priority, TaskPriority::NORMAL);
}

TEST(SchedulingQosMapping, ExplicitPrioritySetsFlagAndWinsOverQos) {
    auto built = task([] {}).qos(QosClass::Critical).priority(TaskPriority::LOW);
    EXPECT_TRUE(built.options().priority_set);
    EXPECT_EQ(built.options().priority, TaskPriority::LOW);
    EXPECT_EQ(built.options().qos, QosClass::Critical);
}

TEST(SchedulingQosMapping, CpuGpuTaskQosSyncsGpuConfigUnlessExplicitPriority) {
    auto cg = cpu_gpu_task([] {}, [] {});
    EXPECT_EQ(cg.gpu_config().priority, 1);  // GpuTaskConfig 默认 NORMAL

    cg.qos(QosClass::Interactive);
    EXPECT_EQ(cg.gpu_config().priority, 2);  // Interactive → HIGH

    cg.priority(TaskPriority::LOW);  // 显式设置后 QoS 不再覆盖
    EXPECT_TRUE(cg.options().priority_set);
    EXPECT_EQ(cg.gpu_config().priority, 0);
    cg.qos(QosClass::Critical);
    EXPECT_EQ(cg.gpu_config().priority, 0);
}

// ---------------------------------------------------------------------------
// 2b. TaskBuilder 扩展字段
// ---------------------------------------------------------------------------

TEST(SchedulingTaskBuilder, SchedulingSettersPopulateOptions) {
    const auto at = Clock::now() + std::chrono::hours(1);
    auto built = task([] {})
                     .name("builder-extended")
                     .deadline(at)
                     .qos(QosClass::BestEffort)
                     .affinity(AffinityHint{{2, 3}})
                     .resources(ResourceRequirements{4096, 0});

    ASSERT_TRUE(built.options().deadline.has_value());
    EXPECT_EQ(built.options().deadline->time_since_epoch(), at.time_since_epoch());
    EXPECT_EQ(built.options().qos, QosClass::BestEffort);
    EXPECT_EQ(built.options().affinity.cpus, (std::vector<int>{2, 3}));
    EXPECT_EQ(built.options().resources.memory_bytes, 4096u);
    EXPECT_EQ(built.options().resources.gpu_device, 0);
}

// ---------------------------------------------------------------------------
// 3. EDF 排序：Task::operator< 单元 + PriorityScheduler 出队序
// ---------------------------------------------------------------------------

TEST(SchedulingEdfUnit, OperatorLessPrefersDeadlineOverNoDeadline) {
    Task with_deadline;
    init_task(with_deadline, "d", TaskPriority::NORMAL, 100, 5000);
    Task without;
    init_task(without, "n", TaskPriority::NORMAL, 50);
    // 有 deadline 的一方优先：without < with。
    EXPECT_TRUE(without < with_deadline);
    EXPECT_FALSE(with_deadline < without);
}

TEST(SchedulingEdfUnit, OperatorLessOrdersDeadlinesAscending) {
    Task early;
    init_task(early, "early", TaskPriority::NORMAL, 100, 1000);
    Task late;
    init_task(late, "late", TaskPriority::NORMAL, 200, 9000);
    EXPECT_TRUE(late < early);   // deadline 早者先
    EXPECT_FALSE(early < late);
}

TEST(SchedulingEdfUnit, OperatorLessKeepsFifoWithoutDeadline) {
    Task first;
    init_task(first, "first", TaskPriority::NORMAL, 100);
    Task second;
    init_task(second, "second", TaskPriority::NORMAL, 200);
    EXPECT_TRUE(second < first);  // 提交早者先
    EXPECT_FALSE(first < second);
}

TEST(SchedulingEdfUnit, OperatorLessEqualDeadlinesFallBackToFifo) {
    Task first;
    init_task(first, "first", TaskPriority::NORMAL, 100, 7000);
    Task second;
    init_task(second, "second", TaskPriority::NORMAL, 200, 7000);
    EXPECT_TRUE(second < first);
    EXPECT_FALSE(first < second);
}

TEST(SchedulingEdfUnit, PriorityDominatesDeadline) {
    Task critical_plain;
    init_task(critical_plain, "crit", TaskPriority::CRITICAL, 300);
    Task normal_deadline;
    init_task(normal_deadline, "norm", TaskPriority::NORMAL, 100, 1);
    EXPECT_TRUE(normal_deadline < critical_plain);
    EXPECT_FALSE(critical_plain < normal_deadline);
}

TEST(SchedulingEdfUnit, PrioritySchedulerDequeuesInEdfOrder) {
    PriorityScheduler scheduler;
    // 同为 NORMAL：无 deadline A；deadline 远 B；deadline 近 C；无 deadline D。
    Task a;
    init_task(a, "A-no-deadline", TaskPriority::NORMAL, 1000);
    Task b;
    init_task(b, "B-late-deadline", TaskPriority::NORMAL, 2000, 90000);
    Task c;
    init_task(c, "C-early-deadline", TaskPriority::NORMAL, 3000, 50000);
    Task d;
    init_task(d, "D-no-deadline", TaskPriority::NORMAL, 4000);
    scheduler.enqueue(a);
    scheduler.enqueue(b);
    scheduler.enqueue(c);
    scheduler.enqueue(d);
    ASSERT_EQ(scheduler.size(), 4u);

    const char* expected[] = {"C-early-deadline", "B-late-deadline",
                              "A-no-deadline", "D-no-deadline"};
    for (const char* id : expected) {
        Task out;
        ASSERT_TRUE(scheduler.dequeue(out));
        EXPECT_EQ(out.task_id, id) << "EDF dequeue order violated";
    }
    EXPECT_TRUE(scheduler.empty());
}

TEST(SchedulingEdfUnit, PrioritySchedulerStillHonorsPriorityOverDeadline) {
    PriorityScheduler scheduler;
    Task low;
    init_task(low, "low-deadline", TaskPriority::LOW, 100, 100);
    Task critical;
    init_task(critical, "crit-plain", TaskPriority::CRITICAL, 200);
    Task high;
    init_task(high, "high-deadline", TaskPriority::HIGH, 300, 200);
    scheduler.enqueue(low);
    scheduler.enqueue(critical);
    scheduler.enqueue(high);

    const char* expected[] = {"crit-plain", "high-deadline", "low-deadline"};
    for (const char* id : expected) {
        Task out;
        ASSERT_TRUE(scheduler.dequeue(out));
        EXPECT_EQ(out.task_id, id);
    }
}

// ---------------------------------------------------------------------------
// 3b. EDF 端到端：TaskSchedulingMeta 经 try_submit_priority_task 进 Task，
//     默认池（单 worker）按 EDF 出队。
// ---------------------------------------------------------------------------

TEST(SchedulingEdfEndToEnd, MetaDeadlineDrivesPoolOrdering) {
    Executor executor;
    init_executor(executor, 1);

    std::atomic<int> finish_order{0};
    std::atomic<int> edf_slot{-1};
    std::atomic<int> plain_slot{-1};

    // 独占唯一 worker，保证后续两个任务在全局调度器内排队。
    auto blocker = executor.submit_auto(task([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
    }).name("edf-pool-blocker"));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    const auto deadline = Clock::now() + std::chrono::seconds(2);
    auto with_deadline = executor.submit_auto(task([&] {
        edf_slot.store(finish_order.fetch_add(1), std::memory_order_relaxed);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }).name("edf-pool-first").deadline(deadline));
    auto without_deadline = executor.submit_auto(task([&] {
        plain_slot.store(finish_order.fetch_add(1), std::memory_order_relaxed);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }).name("edf-pool-second"));

    ASSERT_TRUE(blocker.valid());
    blocker.get();
    with_deadline.get();
    without_deadline.get();

    // 同优先级（默认 Standard→NORMAL）内，有 deadline 的任务先完成。
    EXPECT_EQ(edf_slot.load(), 0)
        << "task with deadline should complete before task without deadline";
    EXPECT_EQ(plain_slot.load(), 1);
}

// ---------------------------------------------------------------------------
// 4. deadline 准入：DefaultScheduler 拒绝已过期 deadline
// ---------------------------------------------------------------------------

TEST(SchedulingDeadlineAdmission, DefaultSchedulerRejectsExpiredDeadline) {
    DefaultScheduler scheduler;
    TaskOptions options;
    options.name = "expired";
    options.deadline = Clock::now() - std::chrono::milliseconds(1);

    const auto decision = scheduler.route(make_request(options, false), {});
    // 0.6.1：过期 deadline 的拒绝原因细化为结构化 DeadlineExpired。
    EXPECT_EQ(decision.reason, RoutingReason::DeadlineExpired);
    EXPECT_EQ(decision.status, RoutingStatus::Rejected);
    EXPECT_NE(decision.detail.find("deadline already missed"), std::string::npos)
        << "detail: " << decision.detail;
}

TEST(SchedulingDeadlineAdmission, DefaultSchedulerAcceptsFutureDeadline) {
    DefaultScheduler scheduler;
    TaskOptions options;
    options.name = "in-time";
    options.deadline = Clock::now() + std::chrono::hours(1);

    const auto decision = scheduler.route(make_request(options, false), {});
    EXPECT_EQ(decision.reason, RoutingReason::DefaultPolicy);
    EXPECT_EQ(decision.detail.find("deadline already missed"), std::string::npos);
}

TEST(SchedulingDeadlineAdmission, SubmitAutoRejectsMissedDeadlineViaFuture) {
    Executor executor;
    init_executor(executor, 2);

    auto future = executor.submit_auto(
        task([] {}).name("missed-at-submit")
            .deadline(Clock::now() - std::chrono::milliseconds(10)));

    bool threw = false;
    std::string message;
    try {
        future.get();
    } catch (const std::exception& error) {
        threw = true;
        message = error.what();
    }
    EXPECT_TRUE(threw) << "expired deadline must be rejected, not executed";
    EXPECT_NE(message.find("deadline"), std::string::npos) << "message: " << message;

    // 拒绝路径同时记录 SubmitRejected 失败事件（诊断可见）。
    EXPECT_GE(executor.get_failure_status().submit_rejected_count, 1u);
}

// ---------------------------------------------------------------------------
// 5. deadline 错过观测：DeadlineMissed 事件 + deadline_missed_count
// ---------------------------------------------------------------------------

TEST(SchedulingDeadlineMissed, QueuedTaskMissesDeadlineAndStillRuns) {
    Executor executor;
    init_executor(executor, 2);  // 2 worker：4 个 150ms 任务占满 ~300ms
    executor.set_recent_failure_capacity(16);

    // 所有任务共享同一 deadline（now+20ms）：同 deadline 退化为 FIFO，
    // 第 5 个任务必排在 4 个 150ms 阻塞任务之后，开始执行时已错过。
    const auto shared_deadline = Clock::now() + std::chrono::milliseconds(20);

    std::vector<std::future<void>> futures;
    for (int i = 0; i < 4; ++i) {
        futures.push_back(executor.submit_auto(
            task([] { std::this_thread::sleep_for(std::chrono::milliseconds(150)); })
                .name("miss-blocker")
                .deadline(shared_deadline)));
    }
    futures.push_back(executor.submit_auto(
        task([] { std::this_thread::sleep_for(std::chrono::milliseconds(5)); })
            .name("miss-last")
            .deadline(shared_deadline)));

    // 契约"取消/超时是请求不是中断"：错过的任务仍会执行完毕。
    for (auto& future : futures) {
        EXPECT_NO_THROW(future.get());
    }

    const auto status = executor.get_failure_status();
    EXPECT_GE(status.deadline_missed_count, 1u)
        << "queued task started after deadline must be counted";
    EXPECT_LE(status.deadline_missed_count, 5u);

    bool saw_named_miss = false;
    size_t missed_events = 0;
    for (const auto& event : executor.get_recent_failures()) {
        if (event.kind != FailureKind::DeadlineMissed) {
            continue;
        }
        ++missed_events;
        EXPECT_NE(event.message.find("deadline"), std::string::npos)
            << "message: " << event.message;
        if (event.task_id == "miss-last") {
            saw_named_miss = true;
        }
    }
    EXPECT_EQ(missed_events, status.deadline_missed_count)
        << "recent events and counter must agree";
    EXPECT_TRUE(saw_named_miss)
        << "the last-queued task must have recorded a DeadlineMissed event";
}

TEST(SchedulingDeadlineMissed, MetDeadlineRecordsNothing) {
    Executor executor;
    init_executor(executor, 2);

    auto future = executor.submit_auto(
        task([] { std::this_thread::sleep_for(std::chrono::milliseconds(5)); })
            .name("in-time-task")
            .deadline(Clock::now() + std::chrono::seconds(5)));
    EXPECT_NO_THROW(future.get());

    EXPECT_EQ(executor.get_failure_status().deadline_missed_count, 0u);
    for (const auto& event : executor.get_recent_failures()) {
        EXPECT_NE(event.kind, FailureKind::DeadlineMissed);
    }
}

// ---------------------------------------------------------------------------
// 6. QoS 端到端：映射后的优先级提交执行成功、无回归
//    （映射正确性已由 SchedulingQosMapping.* 单元覆盖）
// ---------------------------------------------------------------------------

TEST(SchedulingQosEndToEnd, AllQosClassesSubmitAndExecute) {
    Executor executor;
    init_executor(executor, 2);

    std::atomic<int> executed{0};
    const QosClass classes[] = {QosClass::BestEffort, QosClass::Standard,
                                QosClass::Interactive, QosClass::Critical};
    for (const QosClass qos : classes) {
        auto future = executor.submit_auto(task([&executed, qos] {
            executed.fetch_add(1, std::memory_order_relaxed);
        }).name(std::string("qos-") + qos_class_to_string(qos)).qos(qos));
        EXPECT_NO_THROW(future.get()) << "qos class "
                                      << qos_class_to_string(qos) << " failed";
    }
    EXPECT_EQ(executed.load(), 4);
    EXPECT_EQ(executor.get_failure_status().deadline_missed_count, 0u);
}

// ---------------------------------------------------------------------------
// 7. affinity advisory：不相交 → 接受 + AffinityMismatch 警告；相交 → 无警告
// ---------------------------------------------------------------------------

TEST(SchedulingAffinityAdvisory, DisjointCpusAcceptedWithMismatchWarning) {
    Executor executor;
    init_executor(executor, 2);  // 默认池绑核 0..hw-1（auto-allocate）
    executor.set_recent_routing_capacity(16);

    auto future = executor.submit_auto(
        task([] {}).name("affinity-far").affinity(AffinityHint{{100000}}));
    EXPECT_NO_THROW(future.get()) << "affinity is advisory: task must be accepted";

    bool found = false;
    for (const auto& decision : executor.get_recent_routing_decisions()) {
        if (decision.task_name != "affinity-far") {
            continue;
        }
        found = true;
        EXPECT_NE(decision.detail.find("AffinityMismatch"), std::string::npos)
            << "detail: " << decision.detail;
        // 0.6.1：advisory mismatch 不拒绝，但以结构化形式降级——
        // status = AcceptedDegraded，reason = AffinityMismatch，
        // diagnostics 携带 AffinityMismatch 位。
        EXPECT_EQ(decision.status, RoutingStatus::AcceptedDegraded)
            << "advisory mismatch must not reject";
        EXPECT_EQ(decision.reason, RoutingReason::AffinityMismatch);
        EXPECT_NE(decision.diagnostics & RoutingDiagnostics::AffinityMismatch, 0u);
    }
    EXPECT_TRUE(found) << "routing decision for affinity-far not recorded";
}

TEST(SchedulingAffinityAdvisory, IntersectingCpusProduceNoWarning) {
    Executor executor;
    init_executor(executor, 2);
    executor.set_recent_routing_capacity(16);

    auto future = executor.submit_auto(
        task([] {}).name("affinity-cpu0").affinity(AffinityHint{{0}}));
    EXPECT_NO_THROW(future.get());

    bool found = false;
    for (const auto& decision : executor.get_recent_routing_decisions()) {
        if (decision.task_name != "affinity-cpu0") {
            continue;
        }
        found = true;
        EXPECT_EQ(decision.detail.find("AffinityMismatch"), std::string::npos)
            << "detail: " << decision.detail;
    }
    EXPECT_TRUE(found) << "routing decision for affinity-cpu0 not recorded";
}

// ---------------------------------------------------------------------------
// 8. resource 检查：DefaultScheduler 单元级（伪造能力快照）
// ---------------------------------------------------------------------------

TEST(SchedulingResourceChecks, DeclaredGpuDeviceMismatchRejected) {
    DefaultScheduler scheduler;
    TaskOptions options;
    options.name = "gpu-wrong-device";
    options.intent = ExecutionIntent::CpuOrGpu;
    options.resources.gpu_device = 1;

    std::vector<ExecutorCapability> capabilities{make_cpu_capability(),
                                                 make_gpu_capability(0)};
    const auto decision = scheduler.route(make_request(options, true), capabilities);
    EXPECT_EQ(decision.reason, RoutingReason::BackendUnavailable);
    EXPECT_NE(decision.detail.find("gpu_device"), std::string::npos)
        << "detail: " << decision.detail;
}

TEST(SchedulingResourceChecks, DeclaredGpuDeviceMatchAccepted) {
    DefaultScheduler scheduler;
    TaskOptions options;
    options.intent = ExecutionIntent::CpuOrGpu;
    options.resources.gpu_device = 0;

    std::vector<ExecutorCapability> capabilities{make_cpu_capability(),
                                                 make_gpu_capability(0)};
    const auto decision = scheduler.route(make_request(options, true), capabilities);
    EXPECT_EQ(decision.reason, RoutingReason::GpuHeuristic);
    EXPECT_EQ(decision.selected_backend, ExecutionBackend::Gpu);
}

TEST(SchedulingResourceChecks, MemoryOverGpuAvailabilityRejected) {
    DefaultScheduler scheduler;
    TaskOptions options;
    options.intent = ExecutionIntent::CpuOrGpu;
    options.resources.memory_bytes = 2048;  // > free 1024

    std::vector<ExecutorCapability> capabilities{
        make_cpu_capability(), make_gpu_capability(0, /*total=*/4096, /*free=*/1024)};
    const auto decision = scheduler.route(make_request(options, true), capabilities);
    EXPECT_EQ(decision.reason, RoutingReason::CapacityPressure);
    EXPECT_NE(decision.detail.find("exceeds"), std::string::npos)
        << "detail: " << decision.detail;
}

TEST(SchedulingResourceChecks, MemoryWithinAvailabilityAccepted) {
    DefaultScheduler scheduler;
    TaskOptions options;
    options.intent = ExecutionIntent::CpuOrGpu;
    options.resources.memory_bytes = 512;

    std::vector<ExecutorCapability> capabilities{
        make_cpu_capability(), make_gpu_capability(0, /*total=*/4096, /*free=*/1024)};
    const auto decision = scheduler.route(make_request(options, true), capabilities);
    EXPECT_EQ(decision.reason, RoutingReason::GpuHeuristic);
    EXPECT_EQ(decision.selected_backend, ExecutionBackend::Gpu);
}

TEST(SchedulingResourceChecks, UnknownGpuMemoryTotalSkipsMemoryCheck) {
    DefaultScheduler scheduler;
    TaskOptions options;
    options.intent = ExecutionIntent::CpuOrGpu;
    options.resources.memory_bytes = 1u << 30;  // 巨额声明，总量未知须跳过

    std::vector<ExecutorCapability> capabilities{
        make_cpu_capability(), make_gpu_capability(0, /*total=*/0, /*free=*/0)};
    const auto decision = scheduler.route(make_request(options, true), capabilities);
    EXPECT_NE(decision.reason, RoutingReason::CapacityPressure)
        << "detail: " << decision.detail;
}

TEST(SchedulingResourceChecks, NoGpuCapabilityDelegatesToRouterSemantics) {
    DefaultScheduler scheduler;
    TaskOptions options;
    options.intent = ExecutionIntent::CpuOrGpu;
    options.resources.gpu_device = 1;

    // 无 GPU 能力条目：不在此重复拒绝语义，交给 TaskRouter
    // （BackendUnavailable / 回退语义），且不得崩溃。
    std::vector<ExecutorCapability> capabilities{make_cpu_capability()};
    const auto decision = scheduler.route(make_request(options, true), capabilities);
    EXPECT_EQ(decision.reason, RoutingReason::BackendUnavailable);
    EXPECT_FALSE(decision.fell_back);
    EXPECT_NE(decision.detail.find("GPU"), std::string::npos)
        << "detail: " << decision.detail;
}

// ---------------------------------------------------------------------------
// 9. 自定义 IScheduler 注入
// ---------------------------------------------------------------------------

TEST(SchedulingSchedulerInjection, InjectedSchedulerObservesRoutes) {
    Executor executor;
    init_executor(executor, 2);

    auto recording = std::make_unique<RecordingScheduler>();
    RecordingScheduler* recorder = recording.get();
    executor.set_scheduler(std::move(recording));
    EXPECT_EQ(executor.get_scheduler(), recorder);

    auto future = executor.submit_auto(task([] {}).name("via-recording"));
    EXPECT_NO_THROW(future.get());
    EXPECT_GE(recorder->route_count(), 1)
        << "injected scheduler must observe at least one route";
}

TEST(SchedulingSchedulerInjection, NullptrRestoresDefaultAndKeepsWorking) {
    Executor executor;
    init_executor(executor, 2);

    executor.set_scheduler(std::make_unique<RecordingScheduler>());
    executor.set_scheduler(nullptr);  // 恢复默认调度器
    EXPECT_NE(executor.get_scheduler(), nullptr);

    auto future = executor.submit_auto(task([] { return 42; }).name("after-restore"));
    EXPECT_NO_THROW(EXPECT_EQ(future.get(), 42));
}

// ---------------------------------------------------------------------------
// on_task_completed：预留接口默认无操作（§2.1 反馈通道现状）
// ---------------------------------------------------------------------------

TEST(SchedulingFeedbackChannel, OnTaskCompletedIsSafeNoOp) {
    DefaultScheduler scheduler;
    SchedulingFeedback feedback;
    feedback.task_id = "t";
    feedback.qos = QosClass::Interactive;
    feedback.success = true;
    EXPECT_NO_THROW(scheduler.on_task_completed(feedback));
}
