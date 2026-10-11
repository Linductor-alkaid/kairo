// CR-071 提交吞吐快速门验收（roadmap M0 验收线）。
//
// 背景：TaskMonitor 分片化后带热路径快速门——统计采样率 0 时不触碰
// id 映射/统计分片，in-flight 采样率 0 或容量 0 时不触碰 in-flight 分片，
// 两门全关时 record_task_start/complete/timeout 零取锁。M0 验收线要求：
// 监控开启但两门全关（配置 B）的提交吞吐与监控完全禁用（配置 A）持平
// （±5%）。
//
// Method（对照 cr024_low_priority_starvation.cpp 的 standalone 风格）：
//  1. 每轮新建 ThreadPool（4 worker，队列容量 400000 > 20 万任务，
//     无背压拒绝），每组配置先跑 2 万任务 warmup 一轮并排空。
//  2. 4 生产者线程各提交 5 万个空任务（try_submit），测量生产者窗口
//     墙钟 → 提交吞吐 tasks/s。
//  3. 配置 A：不挂 monitor；B：挂 monitor 且 set_sampling_rate(0) +
//     set_in_flight_sampling_rate(0)；C：挂 monitor 默认全采样（参照组）。
//  4. ABBA 设计：每轮按 A,B,B,A 顺序跑（两组同轮配对样本，抵消线性负载/
//     降频漂移），另跑一次 C（全采样参照）。7 轮共 14 个配对样本；判定用
//     配对差 (B-A)/A 的中位数，|中位数| <= 5% 即达标。
//
// 编译（库与本机 Release 构建一致，-O3）：
//   g++ -O3 -DNDEBUG -std=c++20 \
//     -I include -I src \
//     review_verification/cr071_submit_throughput_gating.cpp \
//     build/src/libkairo.a -pthread -lrt -o \
//     review_verification/cr071_submit_throughput_gating
// 运行（绑核减少桌面噪声）：
//   taskset -c 4-13 review_verification/cr071_submit_throughput_gating
//
// Verdict: PASS iff |median(B)-median(A)| <= 5% * median(A)。
#include "kairo/thread_pool/thread_pool.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

#include "kairo/monitor/task_monitor.hpp"
#include "kairo/util/thread_utils.hpp"

using namespace kairo;
namespace chrono = std::chrono;

namespace {

constexpr int kProducers = 4;
constexpr long kTasksPerProducer = 50000;
constexpr long kTotalTasks = kProducers * kTasksPerProducer;
constexpr int kRounds = 7;
constexpr size_t kQueueCapacity = 400000;
// 生产者各绑一核，worker 绑到错开的高编号核：消除线程迁移与抢占噪声。
constexpr int kProducerBaseCpu = 4;
const std::vector<int> kWorkerCpus = {10, 11, 12, 13};

enum class Config { A_disabled, B_gates_closed, C_full_sampling };

const char* config_name(Config c) {
    switch (c) {
        case Config::A_disabled: return "A_disabled";
        case Config::B_gates_closed: return "B_gates_closed";
        case Config::C_full_sampling: return "C_full_sampling";
    }
    return "?";
}

double median_of(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

// 单轮：建池 → warmup 2 万并排空 → 4 生产者提交 20 万 → 测提交窗口。
// 返回提交吞吐 tasks/s；drain_ms 为排空耗时（信息性）。
double run_round(Config config, long& drain_ms) {
    ThreadPoolConfig cfg;
    cfg.min_threads = 4;
    cfg.max_threads = 4;
    cfg.queue_capacity = kQueueCapacity;
    // worker 绑到高编号核，与生产者（各绑 4-7 号核）错开。
    cfg.cpu_affinity = kWorkerCpus;

    ThreadPool pool;
    if (!pool.initialize(cfg)) {
        printf("FATAL: initialize failed\n");
        return -1.0;
    }

    monitor::TaskMonitor monitor;
    if (config != Config::A_disabled) {
        pool.set_task_monitor(&monitor);
        if (config == Config::B_gates_closed) {
            monitor.set_sampling_rate(0.0);
            monitor.set_in_flight_sampling_rate(0.0);
        }
    }

    auto drain = [&pool, &drain_ms]() {
        const auto t0 = chrono::steady_clock::now();
        for (int i = 0; i < 6000; ++i) {  // 有界：最多 ~30s
            if (pool.get_status().queue_size == 0) break;
            std::this_thread::sleep_for(chrono::milliseconds(5));
        }
        drain_ms = chrono::duration_cast<chrono::milliseconds>(
                       chrono::steady_clock::now() - t0)
                       .count();
    };

    // 注意：try_submit 拒绝空 std::function（thread_pool.cpp:1007），
    // 每次提交构造一个非空的空体任务——三组配置的提交路径构造开销一致。
    auto make_task = []() -> std::function<void()> {
        return std::function<void()> ([] {});
    };

    // warmup：首触分配/惰性初始化不进测量窗口。
    std::atomic<long> warm_left{20000};
    {
        std::vector<std::thread> ws;
        for (int w = 0; w < kProducers; ++w) {
            ws.emplace_back([&pool, &warm_left, &make_task]() {
                while (warm_left.fetch_sub(1, std::memory_order_relaxed) > 0) {
                    while (!pool.try_submit(make_task())) {
                        std::this_thread::yield();
                    }
                }
            });
        }
        for (auto& t : ws) t.join();
    }
    long warm_drain = 0;
    (void)warm_drain;
    drain();

    // 测量窗口：4 生产者 × 5 万空任务。
    std::atomic<long> retry_failures{0};
    const auto t_start = chrono::steady_clock::now();
    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (int w = 0; w < kProducers; ++w) {
        producers.emplace_back([&pool, &retry_failures, &make_task, w]() {
            // 生产者各自绑核（4+w），与 worker（10-13）错开。
            util::set_current_thread_affinity({kProducerBaseCpu + w});
            for (long i = 0; i < kTasksPerProducer; ++i) {
                // 队列容量 > 总任务数，正常路径不应失败；失败自旋重试。
                while (!pool.try_submit(make_task())) {
                    retry_failures.fetch_add(1, std::memory_order_relaxed);
                    std::this_thread::yield();
                }
            }
        });
    }
    for (auto& t : producers) t.join();
    const auto t_end = chrono::steady_clock::now();

    const double seconds =
        chrono::duration_cast<chrono::duration<double>>(t_end - t_start)
            .count();
    drain();

    if (retry_failures.load() != 0) {
        printf("  WARN: %ld submit retries\n", retry_failures.load());
    }
    pool.shutdown();
    return static_cast<double>(kTotalTasks) / seconds;
}

}  // namespace

int main() {
    setbuf(stdout, nullptr);
    printf("CR-071 submit-throughput gating check: %d producers x %ld tasks, "
           "4 workers, queue=%zu, %d ABBA rounds (A,B,B,A per round + C ref; "
           "ABBA 配对抵消线性漂移)\n",
           kProducers, kTasksPerProducer, kQueueCapacity, kRounds);

    // ABBA 设计：每轮 A,B,B,A 取两组 (A,B) 配对样本，另跑一次 C 作参照。
    // 判定用配对差（同轮背靠背，抵消频率/负载线性漂移），中位数汇总。
    std::vector<double> a_all, b_all, c_all;
    std::vector<double> paired_delta_pct;  // (b-a)/a per pair
    a_all.reserve(2 * kRounds);
    b_all.reserve(2 * kRounds);
    c_all.reserve(kRounds);
    paired_delta_pct.reserve(2 * kRounds);

    for (int r = 0; r < kRounds; ++r) {
        const Config seq[4] = {Config::A_disabled, Config::B_gates_closed,
                               Config::B_gates_closed, Config::A_disabled};
        double vals[4];
        for (int s = 0; s < 4; ++s) {
            long drain_ms = 0;
            vals[s] = run_round(seq[s], drain_ms);
            printf("round %d seq%d %-16s %10.0f tasks/s (drain %ld ms)\n",
                   r + 1, s + 1, config_name(seq[s]), vals[s], drain_ms);
        }
        a_all.push_back(vals[0]);
        b_all.push_back(vals[1]);
        b_all.push_back(vals[2]);
        a_all.push_back(vals[3]);
        paired_delta_pct.push_back((vals[1] - vals[0]) / vals[0] * 100.0);
        paired_delta_pct.push_back((vals[2] - vals[3]) / vals[3] * 100.0);

        long drain_ms = 0;
        const double tps_c = run_round(Config::C_full_sampling, drain_ms);
        c_all.push_back(tps_c);
        printf("round %d %-16s %10.0f tasks/s (drain %ld ms)\n", r + 1,
               config_name(Config::C_full_sampling), tps_c, drain_ms);
    }

    const double med_a = median_of(a_all);
    const double med_b = median_of(b_all);
    const double med_c = median_of(c_all);
    const double med_paired = median_of(paired_delta_pct);
    const bool pass = std::abs(med_paired) <= 5.0;

    printf("\nmedian A (monitor disabled)      = %10.0f tasks/s (%zu samples)\n",
           med_a, a_all.size());
    printf("median B (gates closed, both 0)  = %10.0f tasks/s (%zu samples)\n",
           med_b, b_all.size());
    printf("median C (full sampling, ref)    = %10.0f tasks/s (%zu samples)\n",
           med_c, c_all.size());
    printf("median paired delta (B-A)/A      = %+.2f%%\n", med_paired);
    printf("M0 acceptance: |median paired (B-A)/A| <= 5%% -> %s\n",
           pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
