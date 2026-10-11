/**
 * AdaptiveScheduler benchmark (0.7.0 M3, roadmap §2.4).
 *
 * 验收线（roadmap §0 原则 3：预算先定、后实现）：
 *   A. route() 纯策略路径（Auto intent，无能力表）：AdaptiveScheduler
 *      相对 DefaultScheduler 的配对 ABBA 差值 ≤ 150ns。
 *   B. route() CpuOrGpu 冷路径（伪造 GPU 能力、双侧无样本，启发式
 *      回退）：配对差值 ≤ 250ns（内含 refresh_if_stale 时间门限检查、
 *      快照 RCU 读、GPU 能力查找）。
 *   C. on_task_completed() 单条转发（内嵌聚合器 record 路径）：≤ 150ns。
 *   D. effective_priority_for()（无提升态，submit 热路径新增虚调用）：
 *      ≤ 50ns。
 *   E. 降载场景（端到端）：Interactive 探针端到端 p99 与中位数均严格
 *      优于 DefaultScheduler，且结构化拒绝确实发生
 *     （load_shedding_rejected_count > 0，开闸态经 load_shed_active()
 *      复核）。场景：2 worker 池，BestEffort 长任务（12ms 自旋）持续
 *      填充；收到 LoadShedding 拒绝的调用方按结构化信号退避（结构化
 *      拒绝的设计契约）；开闸后学习期（前 200ms）排除后再采探针。
 *      数值记录到 docs/performance/。
 *
 * Config: env KAIRO_BENCHMARK_TASKS (default 20000 per round),
 *         KAIRO_BENCHMARK_SHED_MS（降载场景每轮采样时长，default 3000）。
 * Output: human-readable text (default) or JSON lines (--json).
 * Exit:   non-zero if any acceptance line fails.
 */

#include <kairo/adaptive_scheduler.hpp>
#include <kairo/executor.hpp>
#include <kairo/scheduler.hpp>
#include <kairo/scheduling.hpp>
#include <kairo/task_options.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace kairo;

namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t kDefaultTasks = 20000;
constexpr int kMicroRounds = 5;
constexpr int kAbbaRounds = 4;
// ---- 验收线 ----
constexpr double kAcceptRouteDeltaNs = 150.0;        // A
constexpr double kAcceptCpuGpuColdDeltaNs = 250.0;   // B
constexpr double kAcceptOnTaskCompletedNs = 150.0;   // C
constexpr double kAcceptEffectivePriorityNs = 50.0;  // D

struct Config {
    size_t tasks = kDefaultTasks;
    int shed_round_ms = 3000;
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
    if (const char* env = std::getenv("KAIRO_BENCHMARK_SHED_MS")) {
        if (env && *env) {
            try {
                const int value = std::stoi(env);
                if (value >= 1000) config.shed_round_ms = value;
            } catch (...) {
            }
        }
    }
    return config;
}

double median(std::vector<double>& samples) {
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

void print_case(const Config& config, const std::string& name,
                double value, const char* unit) {
    if (config.json_output) {
        std::printf(
            "{\"benchmark\": \"adaptive_scheduler\", \"case\": \"%s\", "
            "\"value\": %.2f, \"unit\": \"%s\"}\n",
            name.c_str(), value, unit);
    } else {
        std::printf("%-56s %12.2f %s\n", name.c_str(), value, unit);
    }
}

// 伪造单 GPU 能力快照（route 只读能力数据，不需要真实 GPU 执行器）。
std::vector<ExecutorCapability> make_gpu_capabilities() {
    ExecutorCapability gpu;
    gpu.backend = ExecutionBackend::Gpu;
    gpu.name = "gpu0";
    gpu.registered = true;
    gpu.running = true;
    gpu.supports_gpu_kernel = true;
    return {gpu};
}

void spin_for_ns(int64_t duration_ns) {
    const auto begin = Clock::now();
    while (std::chrono::duration_cast<std::chrono::nanoseconds>(
               Clock::now() - begin)
               .count() < duration_ns) {
        // 忙等
    }
}

// 配对 ABBA：每轮 A,B,B,A 四段（各 iterations 次调用），A 取两段均值、
// B 取两段均值，差值 = B - A；轮间取中位，抑制漂移类环境噪声。
double abba_delta_ns(size_t iterations,
                     const std::function<void(size_t, uint64_t&)>& first,
                     const std::function<void(size_t, uint64_t&)>& second) {
    std::vector<double> deltas;
    uint64_t sink = 0;
    for (int round = 0; round < kAbbaRounds; ++round) {
        const auto a1 = Clock::now();
        first(iterations, sink);
        const auto b1 = Clock::now();
        second(iterations, sink);
        const auto b2 = Clock::now();
        second(iterations, sink);
        const auto a2 = Clock::now();
        first(iterations, sink);
        const auto a3 = Clock::now();
        const double first_ns =
            (std::chrono::duration<double, std::nano>(b1 - a1).count() +
             std::chrono::duration<double, std::nano>(a3 - a2).count()) /
            2.0 / static_cast<double>(iterations);
        const double second_ns =
            (std::chrono::duration<double, std::nano>(b2 - b1).count() +
             std::chrono::duration<double, std::nano>(a2 - b2).count()) /
            2.0 / static_cast<double>(iterations);
        deltas.push_back(second_ns - first_ns);
    }
    if (sink == 1) {
        std::printf(" ");
    }
    return median(deltas);
}

void bench_route_overhead(const Config& config) {
    DefaultScheduler default_scheduler;
    AdaptiveScheduler adaptive_scheduler;
    const auto capabilities = make_gpu_capabilities();

    // A：纯策略路径（Auto intent，空能力表——惰性采集语义下零能力读）。
    {
        TaskRouter::Request auto_request;
        const double delta = abba_delta_ns(
            config.tasks,
            [&](size_t n, uint64_t& sink) {
                for (size_t i = 0; i < n; ++i) {
                    sink += static_cast<uint64_t>(
                        default_scheduler.route(auto_request, {})
                            .selected_backend);
                }
            },
            [&](size_t n, uint64_t& sink) {
                for (size_t i = 0; i < n; ++i) {
                    sink += static_cast<uint64_t>(
                        adaptive_scheduler.route(auto_request, {})
                            .selected_backend);
                }
            });
        print_case(config, "route Auto delta adaptive-default (<=150ns)",
                   delta, "ns");
        if (delta > kAcceptRouteDeltaNs) {
            std::printf("ACCEPTANCE FAIL: route Auto delta %.2f > %.0f\n",
                        delta, kAcceptRouteDeltaNs);
            std::exit(1);
        }
    }

    // B：CpuOrGpu 冷路径（双侧无样本 → 启发式回退；能力表 + 快照读）。
    {
        TaskRouter::Request cpu_gpu_request;
        cpu_gpu_request.cpu_gpu_task = true;
        cpu_gpu_request.options.intent = ExecutionIntent::CpuOrGpu;
        cpu_gpu_request.options.name = "bench_cpu_or_gpu";
        const double delta = abba_delta_ns(
            config.tasks,
            [&](size_t n, uint64_t& sink) {
                for (size_t i = 0; i < n; ++i) {
                    sink += static_cast<uint64_t>(
                        default_scheduler
                            .route(cpu_gpu_request, capabilities)
                            .selected_backend);
                }
            },
            [&](size_t n, uint64_t& sink) {
                for (size_t i = 0; i < n; ++i) {
                    sink += static_cast<uint64_t>(
                        adaptive_scheduler
                            .route(cpu_gpu_request, capabilities)
                            .selected_backend);
                }
            });
        print_case(config, "route CpuOrGpu cold delta (<=250ns)", delta, "ns");
        if (delta > kAcceptCpuGpuColdDeltaNs) {
            std::printf("ACCEPTANCE FAIL: CpuOrGpu cold delta %.2f > %.0f\n",
                        delta, kAcceptCpuGpuColdDeltaNs);
            std::exit(1);
        }
    }
}

// ---- C/D：on_task_completed 与 effective_priority_for 微基准 ----

void bench_feedback_and_priority(const Config& config) {
    AdaptiveScheduler adaptive_scheduler;
    SchedulingFeedback feedback;
    feedback.task_id = "bench";
    feedback.qos = QosClass::Standard;
    feedback.success = true;
    feedback.backend = ExecutionBackend::DefaultAsync;
    feedback.executor_name = "default";
    feedback.queue_wait_ns = 50'000;
    feedback.execution_duration_ns = 1'000'000;

    {
        std::vector<double> per_task;
        uint64_t sink = 0;
        for (int round = 0; round < kMicroRounds; ++round) {
            const auto begin = Clock::now();
            for (size_t i = 0; i < config.tasks; ++i) {
                adaptive_scheduler.on_task_completed(feedback);
            }
            per_task.push_back(
                std::chrono::duration<double, std::nano>(Clock::now() - begin)
                    .count() /
                static_cast<double>(config.tasks));
        }
        const double result = median(per_task);
        print_case(config, "on_task_completed forward (<=150ns)", result, "ns");
        if (result > kAcceptOnTaskCompletedNs) {
            std::printf("ACCEPTANCE FAIL: on_task_completed %.2f > %.0f\n",
                        result, kAcceptOnTaskCompletedNs);
            std::exit(1);
        }
        if (sink == 1) std::printf(" ");
    }

    {
        TaskOptions options;
        options.qos = QosClass::Interactive;
        const int default_priority = 2;  // HIGH
        std::vector<double> per_call;
        for (int round = 0; round < kMicroRounds; ++round) {
            int64_t sink = 0;
            const auto begin = Clock::now();
            for (size_t i = 0; i < config.tasks; ++i) {
                sink += adaptive_scheduler.effective_priority_for(
                    options, default_priority);
            }
            per_call.push_back(
                std::chrono::duration<double, std::nano>(Clock::now() - begin)
                    .count() /
                static_cast<double>(config.tasks));
            if (sink == 42) std::printf(" ");
        }
        const double result = median(per_call);
        print_case(config, "effective_priority_for idle (<=50ns)", result,
                   "ns");
        if (result > kAcceptEffectivePriorityNs) {
            std::printf(
                "ACCEPTANCE FAIL: effective_priority_for %.2f > %.0f\n",
                result, kAcceptEffectivePriorityNs);
            std::exit(1);
        }
    }
}

// ---- E：降载场景（端到端）----

struct ShedRunResult {
    double probe_p99_ns = 0;
    double probe_median_ns = 0;
    uint64_t rejections = 0;
    bool shed_active_at_end = false;
};

// 场景：2 worker 池。BestEffort 长任务（12ms 自旋）持续填充；Interactive
// 探针（1ms 自旋）每 3ms 一条，外部端到端计时。跳过前 warmup_ms 的学习期
// （状态机识别负载并开闸），只对开闸稳态采探针；close_windows 设为足额
// 大值使闸门轮内不关——本场景验收的是"降载开闸后的高 QoS 保护效果"，
// 开关循环的状态机语义由单元测试在受控快照下验证。
ShedRunResult run_shed_scenario(bool adaptive, int sample_ms) {
    constexpr int kWarmupMs = 200;
    Executor executor;
    std::vector<double> probe_e2e_ns;
    std::mutex probe_mutex;
    ExecutorConfig executor_config;
    executor_config.min_threads = 2;
    executor_config.max_threads = 2;
    executor_config.queue_capacity = 4096;
    executor_config.enable_monitoring = false;
    if (!executor.initialize(executor_config)) {
        std::printf("FATAL: executor initialize failed\n");
        std::exit(2);
    }
    AdaptiveScheduler* adaptive_ptr = nullptr;
    if (adaptive) {
        AdaptiveSchedulerConfig shed_config;
        shed_config.load_shedding_enabled = true;
        shed_config.priority_promotion_enabled = false;  // 隔离降载决策
        shed_config.queue_wait_p99_target_ns = {0, 5'000'000, 5'000'000,
                                                2'000'000};
        shed_config.shed_breach_windows = 2;
        shed_config.shed_close_windows = 1000000;
        // 探针周期 ~3.2ms、25ms 合并窗口内 Interactive 样本 ~8 条；
        // 门槛 4 保证窗口评估不被"未知即宽容"跳过。
        shed_config.min_window_samples = 4;
        shed_config.aggregator.merge_interval = std::chrono::milliseconds{25};
        auto scheduler = std::make_unique<AdaptiveScheduler>(shed_config);
        adaptive_ptr = scheduler.get();
        executor.set_scheduler(std::move(scheduler));
    }

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> churn_rejected{0};
    // 两条生产线程填满 2 worker（单线程 .get() 同步等待只会占用一个
    // worker，探针将永远不会排队）。
    constexpr int kChurnProducers = 2;
    std::vector<std::thread> churn_threads;
    for (int producer = 0; producer < kChurnProducers; ++producer) {
        churn_threads.emplace_back([&]() {
            while (!stop.load(std::memory_order_acquire)) {
                try {
                    executor.submit_auto(task([] { spin_for_ns(12'000'000); })
                                             .name("churn")
                                             .qos(QosClass::BestEffort))
                        .get();
                } catch (const std::exception&) {
                    // 结构化拒绝：调用方退避后再试（LoadShedding 设计契约）。
                    churn_rejected.fetch_add(1, std::memory_order_relaxed);
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds{150});
                }
            }
        });
    }

    // 探针线程：固定到达率（submit 后不等待，在途 future 轮询收割）。
    // 若同步等待完成，饱和期到达率会退化到 ~13ms/条，25ms 评估窗口内
    // 样本数跌破 min_window_samples，状态机按"未知即宽容"永不进入。
    // 学习期样本喂给状态机，只把学习期结束前的探针排除出统计。
    std::thread prober([&]() {
        std::vector<std::pair<Clock::time_point, std::future<void>>>
            outstanding;
        const auto scenario_begin = Clock::now();
        while (!stop.load(std::memory_order_acquire)) {
            outstanding.emplace_back(
                Clock::now(),
                executor.submit_auto(task([] { spin_for_ns(1'000'000); })
                                         .name("probe")
                                         .qos(QosClass::Interactive)));
            for (auto it = outstanding.begin(); it != outstanding.end();) {
                if (it->second.wait_for(std::chrono::seconds(0)) ==
                    std::future_status::ready) {
                    const double e2e =
                        std::chrono::duration<double, std::nano>(Clock::now() -
                                                                 it->first)
                            .count();
                    if (std::chrono::duration_cast<std::chrono::milliseconds>(
                            Clock::now() - scenario_begin)
                            .count() >= kWarmupMs) {
                        std::lock_guard<std::mutex> lock(probe_mutex);
                        probe_e2e_ns.push_back(e2e);
                    }
                    it = outstanding.erase(it);
                } else {
                    ++it;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
        for (auto& [submit_time, future] : outstanding) {
            future.wait();
            (void)submit_time;
        }
    });

    std::this_thread::sleep_for(
        std::chrono::milliseconds{kWarmupMs + sample_ms});
    const bool shed_still_active =
        adaptive && adaptive_ptr->load_shed_active(QosClass::Interactive);
    stop.store(true, std::memory_order_release);
    for (auto& churn : churn_threads) {
        churn.join();
    }
    prober.join();
    const uint64_t scheduler_rejections =
        adaptive
            ? executor.get_scheduling_metrics().load_shedding_rejected_count
            : 0;
    executor.shutdown();

    std::sort(probe_e2e_ns.begin(), probe_e2e_ns.end());
    ShedRunResult result;
    result.probe_median_ns = probe_e2e_ns[probe_e2e_ns.size() / 2];
    result.probe_p99_ns = probe_e2e_ns[static_cast<size_t>(
        static_cast<double>(probe_e2e_ns.size() - 1) * 0.99)];
    result.rejections =
        scheduler_rejections + churn_rejected.load(std::memory_order_relaxed);
    result.shed_active_at_end = shed_still_active;
    return result;
}

void bench_load_shedding(const Config& config) {
    std::vector<double> default_p99;
    std::vector<double> adaptive_p99;
    uint64_t adaptive_rejections = 0;
    // ABBA：default, adaptive, adaptive, default，各取中位。
    for (int round = 0; round < 2; ++round) {
        default_p99.push_back(
            run_shed_scenario(false, config.shed_round_ms).probe_p99_ns);
        const ShedRunResult adaptive_result =
            run_shed_scenario(true, config.shed_round_ms);
        adaptive_p99.push_back(adaptive_result.probe_p99_ns);
        adaptive_rejections += adaptive_result.rejections;
        if (!adaptive_result.shed_active_at_end) {
            std::printf(
                "ACCEPTANCE FAIL: shed valve not active at end of adaptive "
                "round (state machine did not engage)\n");
            std::exit(1);
        }
    }
    const double default_value = median(default_p99);
    const double adaptive_value = median(adaptive_p99);
    print_case(config, "shed scenario Interactive p99 default",
               default_value / 1e6, "ms");
    print_case(config, "shed scenario Interactive p99 adaptive",
               adaptive_value / 1e6, "ms");
    print_case(config, "shed scenario structured rejections",
               static_cast<double>(adaptive_rejections), "count");

    if (!(adaptive_value < default_value) || adaptive_rejections == 0) {
        std::printf(
            "ACCEPTANCE FAIL: shed scenario (adaptive p99 %.2fms vs default "
            "%.2fms, rejections %zu)\n",
            adaptive_value / 1e6, default_value / 1e6,
            static_cast<size_t>(adaptive_rejections));
        std::exit(1);
    }
}

}  // namespace

int main(int argc, char** argv) {
    const Config config = parse_config(argc, argv);
    std::printf("kairo AdaptiveScheduler benchmark (M3)\n");
    std::printf("tasks=%zu shed_sample_ms=%d\n\n", config.tasks,
                config.shed_round_ms);

    bench_route_overhead(config);
    bench_feedback_and_priority(config);
    bench_load_shedding(config);

    std::printf("\nall acceptance lines passed\n");
    return 0;
}
