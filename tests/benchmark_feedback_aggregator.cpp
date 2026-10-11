/**
 * Feedback-aggregator benchmark (0.7.0 M2, roadmap §2.3).
 *
 * 验收线：开启聚合时每任务聚合开销 ≤ 100ns。聚合的每任务新增代价即
 * record() 快路径（worker 线程分片原子累加）；该线以单线程单键
 * record 用例为准（含键匹配、EWMA CAS、直方图累加、慢路径首次建槽）。
 *
 * Measures:
 *   1. record() micro:
 *      a) single key, single thread（快路径验收线用例）;
 *      b) alternating 4 keys, single thread（键扫描压力）;
 *      c) 4 threads concurrent, same key（分片路径 + 共享聚合器）.
 *   2. snapshot() RCU 读代价（atomic shared_ptr load + 拷贝）。
 *   3. refresh() 重合并代价（8 键 × 32 分片已填充）。
 *   4. End-to-end submit_auto：feedback 调度器把样本转发进聚合器
 *      （M3 AdaptiveScheduler 形态）vs 只计数的调度器，差值即全链路
 *      每任务聚合代价。
 *
 * Config: env KAIRO_BENCHMARK_TASKS (default 20000 per round).
 * Output: human-readable text (default) or JSON lines (--json).
 */

#include <kairo/executor.hpp>
#include <kairo/feedback_aggregator.hpp>
#include <kairo/scheduler.hpp>
#include <kairo/scheduling.hpp>
#include <kairo/task_options.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <thread>
#include <vector>

using namespace kairo;

namespace {

using Clock = std::chrono::steady_clock;
using scheduling::FeedbackAggregator;
using scheduling::FeedbackKey;
using kairo::SchedulingFeedback;

constexpr size_t kDefaultTasks = 20000;
constexpr int kRounds = 5;
// M2 验收线：开启聚合时每任务聚合开销 ≤ 100ns。
constexpr double kAcceptanceNsPerTask = 100.0;

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

void report(const Config& config, const std::string& name, double ns_per_op,
            size_t ops, const std::string& unit = "op") {
    if (config.json_output) {
        std::printf(
            "{\"benchmark\": \"feedback_aggregator\", \"case\": \"%s\", "
            "\"ops\": %zu, \"ns_per_%s\": %.2f}\n",
            name.c_str(), ops, unit.c_str(), ns_per_op);
    } else {
        std::printf("%-52s %8zu ops  %10.1f ns/%s\n",
                    name.c_str(), ops, ns_per_op, unit.c_str());
    }
}

double median(std::vector<double>& samples) {
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

SchedulingFeedback make_feedback(QosClass qos, int64_t queue_wait_ns,
                                 int64_t duration_ns, bool success = true) {
    SchedulingFeedback feedback;
    feedback.task_id = "bench";
    feedback.qos = qos;
    feedback.success = success;
    feedback.backend = ExecutionBackend::DefaultAsync;
    feedback.executor_name = "default";
    feedback.queue_wait_ns = queue_wait_ns;
    feedback.execution_duration_ns = duration_ns;
    return feedback;
}

// ---- 1. record() micro ----

void bench_record_single_key(const Config& config) {
    FeedbackAggregator aggregator;
    const SchedulingFeedback sample =
        make_feedback(QosClass::Standard, 50'000, 1'000'000);
    std::vector<double> per_task;
    for (int round = 0; round < kRounds; ++round) {
        const auto begin = Clock::now();
        for (size_t i = 0; i < config.tasks; ++i) {
            aggregator.record(sample);
        }
        per_task.push_back(elapsed_ns(begin, Clock::now()) /
                           static_cast<double>(config.tasks));
    }
    const double result = median(per_task);
    report(config, "record single key (acceptance <= 100ns)", result,
           config.tasks, "task");
}

void bench_record_alternating_keys(const Config& config) {
    FeedbackAggregator aggregator;
    // 先建满 4 键，稳态测量键扫描（同分片 4 槽线性匹配）。
    for (int k = 0; k < 4; ++k) {
        aggregator.register_key(
            {ExecutionBackend::DefaultAsync, "default",
             static_cast<QosClass>(k)});
    }
    const std::array<SchedulingFeedback, 4> samples{
        make_feedback(QosClass::BestEffort, 50'000, 1'000'000),
        make_feedback(QosClass::Standard, 50'000, 1'000'000),
        make_feedback(QosClass::Interactive, 50'000, 1'000'000),
        make_feedback(QosClass::Critical, 50'000, 1'000'000)};
    std::vector<double> per_task;
    for (int round = 0; round < kRounds; ++round) {
        size_t key_index = 0;
        const auto begin = Clock::now();
        for (size_t i = 0; i < config.tasks; ++i) {
            aggregator.record(samples[key_index]);
            key_index = (key_index + 1) % samples.size();
        }
        per_task.push_back(elapsed_ns(begin, Clock::now()) /
                           static_cast<double>(config.tasks));
    }
    report(config, "record alternating 4 keys", median(per_task),
           config.tasks, "task");
}

void bench_record_four_threads(const Config& config) {
    FeedbackAggregator aggregator;
    const SchedulingFeedback sample =
        make_feedback(QosClass::Standard, 50'000, 1'000'000);
    std::vector<double> per_task;
    for (int round = 0; round < kRounds; ++round) {
        std::atomic<bool> go{false};
        std::vector<std::thread> workers;
        const auto begin_wall = Clock::now();
        for (int t = 0; t < 4; ++t) {
            workers.emplace_back([&]() {
                while (!go.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                for (size_t i = 0; i < config.tasks; ++i) {
                    aggregator.record(sample);
                }
            });
        }
        go.store(true, std::memory_order_release);
        for (auto& worker : workers) {
            worker.join();
        }
        per_task.push_back(elapsed_ns(begin_wall, Clock::now()) /
                           static_cast<double>(config.tasks * 4));
    }
    report(config, "record 4 threads same key", median(per_task),
           config.tasks * 4, "task");
}

// ---- 2. snapshot / refresh ----

void bench_snapshot_read(const Config& config) {
    FeedbackAggregator aggregator;
    const SchedulingFeedback sample =
        make_feedback(QosClass::Standard, 50'000, 1'000'000);
    for (size_t i = 0; i < 1000; ++i) {
        aggregator.record(sample);
    }
    (void)aggregator.refresh();
    std::vector<double> per_op;
    for (int round = 0; round < kRounds; ++round) {
        uint64_t sink = 0;
        const auto begin = Clock::now();
        for (size_t i = 0; i < config.tasks; ++i) {
            sink += aggregator.snapshot()->merge_count;
        }
        per_op.push_back(elapsed_ns(begin, Clock::now()) /
                         static_cast<double>(config.tasks));
        if (sink == std::numeric_limits<uint64_t>::max()) {
            std::fprintf(stderr, "unreachable\n");  // 防优化器消除读链
        }
    }
    report(config, "snapshot() RCU read", median(per_op), config.tasks);
}

void bench_refresh_merge(const Config& config) {
    FeedbackAggregator aggregator;
    // 8 键（4 档 QoS × 2 执行器名）× 4 线程填充（32 分片均有数据）。
    const std::array<const char*, 8> names = {
        "default", "default", "default", "default",
        "gpu-0",   "gpu-0",   "gpu-0",   "gpu-0"};
    for (size_t k = 0; k < names.size(); ++k) {
        aggregator.register_key(
            {ExecutionBackend::DefaultAsync, names[k],
             static_cast<QosClass>(k % 4)});
    }
    const SchedulingFeedback sample =
        make_feedback(QosClass::Standard, 50'000, 1'000'000);
    for (size_t i = 0; i < 1000; ++i) {
        aggregator.record(sample);
    }
    (void)aggregator.refresh();
    std::vector<double> per_op;
    for (int round = 0; round < kRounds; ++round) {
        const auto begin = Clock::now();
        (void)aggregator.refresh();
        per_op.push_back(elapsed_ns(begin, Clock::now()));
    }
    report(config, "refresh() merge (8 keys x 32 shards)", median(per_op),
           1, "merge");
}

// ---- 3. End-to-end（feedback 调度器 × 聚合转发）----

// 只计数的 feedback 调度器（0.6.1 测量包装基准同款形态）。
class CountingFeedbackScheduler final : public IScheduler {
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

private:
    DefaultScheduler default_;
    std::atomic<uint64_t> received_{0};
};

// 转发进聚合器的 feedback 调度器（M3 AdaptiveScheduler 的样本入口形态）。
class AggregatingFeedbackScheduler final : public IScheduler {
public:
    explicit AggregatingFeedbackScheduler(FeedbackAggregator& aggregator)
        : aggregator_(aggregator) {}
    RoutingDecision route(const TaskRouter::Request& request,
                          const std::vector<ExecutorCapability>& capabilities) override {
        return default_.route(request, capabilities);
    }
    void on_task_completed(const SchedulingFeedback& feedback) override {
        aggregator_.record(feedback);
    }
    bool wants_feedback() const noexcept override { return true; }

private:
    DefaultScheduler default_;
    FeedbackAggregator& aggregator_;
};

void init_executor(Executor& executor, size_t threads) {
    ExecutorConfig executor_config;
    executor_config.min_threads = threads;
    executor_config.max_threads = threads;
    if (!executor.initialize(executor_config)) {
        std::fprintf(stderr, "executor initialization failed\n");
        std::exit(1);
    }
}

double run_submit_phase(Executor& executor, size_t tasks) {
    constexpr size_t kBatch = 512;
    std::vector<std::future<void>> futures;
    futures.reserve(kBatch);
    const auto begin = Clock::now();
    for (size_t i = 0; i < tasks; i += kBatch) {
        const size_t batch = std::min(kBatch, tasks - i);
        futures.clear();
        for (size_t j = 0; j < batch; ++j) {
            futures.push_back(executor.submit_auto(kairo::task([] {})));
        }
        for (auto& future : futures) {
            future.get();
        }
    }
    return elapsed_ns(begin, Clock::now()) / static_cast<double>(tasks);
}

// ABBA 交替各 kRounds 轮取中位数：反馈转发进聚合器 vs 只计数，
// 差值即全链路（submit→worker 反馈→聚合）每任务聚合代价的配对估计。
void bench_facade_e2e(const Config& config) {
    std::vector<double> plain_samples;
    std::vector<double> aggregating_samples;
    for (int round = 0; round < kRounds; ++round) {
        {
            Executor executor;
            init_executor(executor, 4);
            executor.set_recent_routing_capacity(0);
            executor.set_scheduler(std::make_unique<CountingFeedbackScheduler>());
            run_submit_phase(executor, 512);  // 预热
            plain_samples.push_back(run_submit_phase(executor, config.tasks));
        }
        {
            Executor executor;
            init_executor(executor, 4);
            executor.set_recent_routing_capacity(0);
            FeedbackAggregator aggregator;
            executor.set_scheduler(
                std::make_unique<AggregatingFeedbackScheduler>(aggregator));
            run_submit_phase(executor, 512);  // 预热
            aggregating_samples.push_back(
                run_submit_phase(executor, config.tasks));
        }
    }
    const double plain = median(plain_samples);
    const double aggregating = median(aggregating_samples);
    if (config.json_output) {
        std::printf(
            "{\"benchmark\": \"feedback_aggregator\", \"case\": "
            "\"facade_e2e_plain\", \"ops\": %zu, \"ns_per_task\": %.2f}\n",
            config.tasks, plain);
        std::printf(
            "{\"benchmark\": \"feedback_aggregator\", \"case\": "
            "\"facade_e2e_aggregating\", \"ops\": %zu, \"ns_per_task\": "
            "%.2f, \"delta_ns_per_task\": %.2f}\n",
            config.tasks, aggregating, aggregating - plain);
    } else {
        std::printf("%-52s %8zu ops  %10.1f ns/task\n",
                    "facade e2e plain feedback", config.tasks, plain);
        std::printf("%-52s %8zu ops  %10.1f ns/task (delta %+.1f)\n",
                    "facade e2e aggregating feedback", config.tasks,
                    aggregating, aggregating - plain);
    }
}

}  // namespace

int main(int argc, char** argv) {
    const Config config = parse_config(argc, argv);
    if (!config.json_output) {
        std::printf("feedback-aggregator baselines (%zu tasks per round, "
                    "%d rounds, median)\n",
                    config.tasks, kRounds);
    }

    bench_record_single_key(config);
    bench_record_alternating_keys(config);
    bench_record_four_threads(config);
    bench_snapshot_read(config);
    bench_refresh_merge(config);
    bench_facade_e2e(config);

    // 验收线复核（以单线程单键快路径为准）。
    FeedbackAggregator aggregator;
    const SchedulingFeedback sample =
        make_feedback(QosClass::Standard, 50'000, 1'000'000);
    std::vector<double> per_task;
    for (int round = 0; round < kRounds; ++round) {
        const auto begin = Clock::now();
        for (size_t i = 0; i < config.tasks; ++i) {
            aggregator.record(sample);
        }
        per_task.push_back(elapsed_ns(begin, Clock::now()) /
                           static_cast<double>(config.tasks));
    }
    const double acceptance = median(per_task);
    const bool pass = acceptance <= kAcceptanceNsPerTask;
    if (config.json_output) {
        std::printf(
            "{\"benchmark\": \"feedback_aggregator\", \"case\": "
            "\"acceptance_record_single_key\", \"ns_per_task\": %.2f, "
            "\"budget_ns\": %.2f, \"pass\": %s}\n",
            acceptance, kAcceptanceNsPerTask, pass ? "true" : "false");
    } else {
        std::printf("acceptance: record single-key median %.1f ns/task vs "
                    "budget %.1f ns/task -> %s\n",
                    acceptance, kAcceptanceNsPerTask, pass ? "PASS" : "FAIL");
    }
    return pass ? 0 : 1;
}
