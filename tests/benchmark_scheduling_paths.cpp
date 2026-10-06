/**
 * Scheduling-path benchmark (0.6.1 Scheduling Runtime stabilization).
 *
 * Establishes baselines for the scheduling paths that 0.6.1 observability
 * work touches, so regressions (extra allocations, locks, string building,
 * capability snapshot copies on the submit path) are detectable. This is a
 * baseline-capture benchmark, not an optimization target for 0.6.1.
 *
 * Measures:
 *   1. Facade submit baselines:
 *      a) submit_auto() with a bare TaskBuilder (policy-only routing);
 *      b) submit_auto() with a full SchedulingSpec (deadline + qos +
 *         affinity; affinity forces the capability-collection path);
 *      c) submit_auto() with a feedback scheduler installed
 *         (wants_feedback() == true measurement wrapper).
 *   2. EDF enqueue/dequeue on PriorityScheduler (same priority class,
 *      mixed deadlines vs no deadlines).
 *   3. DefaultScheduler::route() micro-costs:
 *      a) policy-only request (no capabilities consumed);
 *      b) affinity request against a populated capability snapshot.
 *   4. Capability snapshot collection cost (get_executor_capabilities).
 *
 * Config: env KAIRO_BENCHMARK_TASKS (default 20000 per phase).
 * Output: human-readable text (default) or JSON lines (--json).
 */

#include <kairo/executor.hpp>
#include <kairo/scheduler.hpp>
#include <kairo/scheduling.hpp>
#include <kairo/task_options.hpp>

#include "kairo/task/task.hpp"
#include "kairo/thread_pool/priority_scheduler.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <string>
#include <vector>

using namespace kairo;

namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t kDefaultTasks = 20000;

struct Config {
    size_t tasks = kDefaultTasks;
    bool json_output = false;
};

Config parse_config(int argc, char** argv) {
    Config config;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--json") {
            config.json_output = true;
        }
    }
    if (const char* env = std::getenv("KAIRO_BENCHMARK_TASKS")) {
        if (env && *env) {
            try {
                const unsigned long value = std::stoul(env);
                if (value > 0) config.tasks = static_cast<size_t>(value);
            } catch (...) {
            }
        }
    }
    return config;
}

double elapsed_ns(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double, std::nano>(end - begin).count();
}

void report(const Config& config, const std::string& name,
            double total_ns, size_t ops, const std::string& unit = "op") {
    const double per_op = total_ns / static_cast<double>(ops);
    if (config.json_output) {
        std::printf(
            "{\"benchmark\": \"scheduling_paths\", \"case\": \"%s\", "
            "\"ops\": %zu, \"ns_per_%s\": %.2f}\n",
            name.c_str(), ops, unit.c_str(), per_op);
    } else {
        std::printf("%-46s %8zu ops  %10.0f ns/%s\n",
                    name.c_str(), ops, per_op, unit.c_str());
    }
}

// 接收执行期反馈的调度器（§6.3 measurement contract 的开销样本）。
class FeedbackScheduler final : public IScheduler {
public:
    RoutingDecision route(const TaskRouter::Request& request,
                          const std::vector<ExecutorCapability>& capabilities) override {
        return default_.route(request, capabilities);
    }
    void on_task_completed(const SchedulingFeedback& feedback) override {
        received_.fetch_add(1, std::memory_order_relaxed);
        (void)feedback;
    }
    bool wants_feedback() const noexcept override { return true; }

    uint64_t received() const noexcept {
        return received_.load(std::memory_order_relaxed);
    }

private:
    DefaultScheduler default_;
    std::atomic<uint64_t> received_{0};
};

// 提交吞吐样本：批量提交 + 批量等待，避免 admission 反压干扰测量。
double measure_facade_submit(Executor& executor, size_t tasks,
                             bool with_spec, const std::string& name,
                             const Config& config) {
    const auto begin = Clock::now();
    constexpr size_t kBatch = 512;
    std::vector<std::future<void>> futures;
    futures.reserve(kBatch);
    const auto far_deadline = Clock::now() + std::chrono::hours(1);
    for (size_t i = 0; i < tasks; i += kBatch) {
        const size_t batch = std::min(kBatch, tasks - i);
        futures.clear();
        for (size_t j = 0; j < batch; ++j) {
            if (with_spec) {
                auto task = kairo::task([] {}).name("bench-spec")
                                .deadline(far_deadline)
                                .qos(QosClass::Interactive)
                                .affinity(AffinityHint{{0}});
                futures.push_back(executor.submit_auto(std::move(task)));
            } else {
                futures.push_back(executor.submit_auto(kairo::task([] {})));
            }
        }
        for (auto& future : futures) {
            future.get();
        }
    }
    const auto end = Clock::now();
    report(config, name, elapsed_ns(begin, end), tasks);
    return elapsed_ns(begin, end);
}

void init_executor(Executor& executor, size_t threads) {
    ExecutorConfig config;
    config.min_threads = threads;
    config.max_threads = threads;
    if (!executor.initialize(config)) {
        std::fprintf(stderr, "executor initialization failed\n");
        std::exit(1);
    }
}

}  // namespace

int main(int argc, char** argv) {
    const Config config = parse_config(argc, argv);
    if (!config.json_output) {
        std::printf("scheduling-path baselines (%zu tasks per phase)\n", config.tasks);
    }

    // ---- 1. Facade submit baselines ----
    {
        Executor executor;
        init_executor(executor, 4);
        // 观测关闭：routing ring buffer 是 submit 路径的额外变量，
        // 两种基线都在无观测下测量（与 CR-106 快速开关语义一致）。
        executor.set_recent_routing_capacity(0);
        // 预热：吸收首阶段的线程启动/缓存未命中偏差。
        measure_facade_submit(executor, 512, false, "warmup", config);
        measure_facade_submit(executor, config.tasks, false,
                              "submit_auto bare builder", config);
        measure_facade_submit(executor, config.tasks, true,
                              "submit_auto full spec (deadline+qos+affinity)", config);
    }
    {
        Executor executor;
        init_executor(executor, 4);
        executor.set_recent_routing_capacity(0);
        auto feedback_scheduler = std::make_unique<FeedbackScheduler>();
        auto* feedback_ptr = feedback_scheduler.get();
        executor.set_scheduler(std::move(feedback_scheduler));
        measure_facade_submit(executor, 512, false, "warmup", config);
        const auto begin = Clock::now();
        constexpr size_t kBatch = 512;
        std::vector<std::future<void>> futures;
        futures.reserve(kBatch);
        for (size_t i = 0; i < config.tasks; i += kBatch) {
            const size_t batch = std::min(kBatch, config.tasks - i);
            futures.clear();
            for (size_t j = 0; j < batch; ++j) {
                futures.push_back(executor.submit_auto(kairo::task([] {})));
            }
            for (auto& future : futures) {
                future.get();
            }
        }
        const auto end = Clock::now();
        // 计数含预热批次（wants_feedback 对所有实际执行的任务生效）。
        if (feedback_ptr->received() != 512 + config.tasks) {
            std::fprintf(stderr, "feedback count mismatch: %zu != %zu\n",
                         static_cast<size_t>(feedback_ptr->received()),
                         static_cast<size_t>(512 + config.tasks));
            return 1;
        }
        report(config, "submit_auto with feedback wrapper", elapsed_ns(begin, end),
               config.tasks);
    }

    // ---- 2. EDF enqueue/dequeue（PriorityScheduler 堆操作）----
    {
        PriorityScheduler scheduler;
        // Task 含 std::atomic 不可拷贝/移动（vector<Task> 连 reserve 都
        // 不可行）：按 unique_ptr 逐个构造后填充字段。
        std::vector<std::unique_ptr<Task>> tasks;
        tasks.reserve(config.tasks);
        const int64_t base_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch()).count();
        for (size_t i = 0; i < config.tasks; ++i) {
            tasks.emplace_back(std::make_unique<Task>());
            Task& task = *tasks.back();
            task.task_id = "edf-" + std::to_string(i);
            task.priority = TaskPriority::NORMAL;
            task.submit_time_ns = base_ns + static_cast<int64_t>(i);
            // 同优先级内一半任务带交错 deadline（EDF 键），一半无。
            task.deadline_ns = (i % 2 == 0) ? base_ns + static_cast<int64_t>(config.tasks - i)
                                            : 0;
            task.function = [] {};
        }
        const auto begin = Clock::now();
        for (auto& task : tasks) {
            scheduler.enqueue(std::move(*task));
        }
        const auto enqueued = Clock::now();
        Task popped;
        size_t dequeued = 0;
        while (scheduler.dequeue(popped)) {
            ++dequeued;
        }
        const auto end = Clock::now();
        if (dequeued != config.tasks) {
            std::fprintf(stderr, "EDF dequeue mismatch: %zu != %zu\n", dequeued,
                         static_cast<size_t>(config.tasks));
            return 1;
        }
        report(config, "EDF enqueue (mixed deadlines)", elapsed_ns(begin, enqueued),
               config.tasks);
        report(config, "EDF dequeue", elapsed_ns(enqueued, end), config.tasks);
    }

    // ---- 3. DefaultScheduler::route() micro-costs ----
    {
        DefaultScheduler scheduler;

        TaskOptions policy_options;
        policy_options.name = "bench-policy";
        const TaskRouter::Request policy_request{policy_options, false, std::nullopt};

        TaskOptions affinity_options;
        affinity_options.name = "bench-affinity";
        affinity_options.affinity = AffinityHint{{0}};
        const TaskRouter::Request affinity_request{affinity_options, false, std::nullopt};

        std::vector<ExecutorCapability> capabilities(4);
        for (size_t i = 0; i < capabilities.size(); ++i) {
            capabilities[i].backend = ExecutionBackend::DefaultAsync;
            capabilities[i].name = "default";
            capabilities[i].registered = true;
            capabilities[i].running = true;
            capabilities[i].supports_future_submission = true;
            capabilities[i].bound_cpus = {0, 1, 2, 3};
        }

        // 纯策略路径：不消费能力表（capabilities 为空，与 CR-106 惰性
        // 采集后的 facade 行为一致）。
        const auto begin = Clock::now();
        for (size_t i = 0; i < config.tasks; ++i) {
            const auto decision = scheduler.route(policy_request, {});
            (void)decision;
        }
        const auto end = Clock::now();
        report(config, "DefaultScheduler route policy-only", elapsed_ns(begin, end),
               config.tasks);

        // affinity 路径：携带能力快照（相交，无降级诊断）。
        const auto begin_affinity = Clock::now();
        for (size_t i = 0; i < config.tasks; ++i) {
            const auto decision = scheduler.route(affinity_request, capabilities);
            (void)decision;
        }
        const auto end_affinity = Clock::now();
        report(config, "DefaultScheduler route affinity+capabilities",
               elapsed_ns(begin_affinity, end_affinity), config.tasks);
    }

    // ---- 4. Capability snapshot collection ----
    {
        Executor executor;
        init_executor(executor, 4);
        const auto begin = Clock::now();
        size_t total = 0;
        for (size_t i = 0; i < config.tasks; ++i) {
            total += executor.get_executor_capabilities().size();
        }
        const auto end = Clock::now();
        if (total == 0) {
            std::fprintf(stderr, "capability snapshot unexpectedly empty\n");
            return 1;
        }
        report(config, "get_executor_capabilities", elapsed_ns(begin, end),
               config.tasks);
    }

    if (!config.json_output) {
        std::printf("done\n");
    }
    return 0;
}
