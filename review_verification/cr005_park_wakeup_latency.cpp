// CR-005: park_worker 丢失唤醒（lockfree_task_executor.cpp:526-544）。
// load + 普通 store 发布驻停位会覆盖生产者 wake_seq_.fetch_add(2) 增量；
// 被覆盖的增量不产生 notify_one。若终扫（步骤 3）未看到该任务，worker 将
// 驻停直到下一次 push 的 bump 才唤醒。
//
// 方法（x86 统计压测）：单 worker；单生产者以抖动间隔提交带序号任务，记录
// 每个任务 push→exec 延迟。丢失唤醒的运行时签名是【任务只在下一次 push 到来
// 时才被执行】，因此判别器为：
//   delay_i ≈ (push_{i+1} - push_i)（exec_i >= push_{i+1} - tol）
//     → LOST_WAKEUP_CANDIDATE（worker 驻停后未被本任务的 bump 唤醒）；
//   exec_i << push_{i+1}（早于下一次 push 就已执行）
//     → worker 侧调度停顿/过渡带，与丢失唤醒无关。
// 间隔默认 8-15ms：默认 50µs timer slack 下 worker 的 10µs-sleep 过渡带实测
// 约 3-6ms，8ms 以上保证每次都完整进入 futex 驻停路径（首轮 1.2-4ms 间隔的
// 运行实际上从未 park，数据作废）。
//
// 用法: ./cr005 [运行秒数=480] [间隔下限us=8000] [间隔上限us=15000]

#include "kairo/lockfree_task_executor.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <thread>
#include <vector>

using kairo::LockFreeTaskExecutor;

namespace {

struct Slot {
    std::atomic<uint64_t> push_ns{0};
    std::atomic<uint64_t> exec_ns{0};
};

}  // namespace

int main(int argc, char** argv) {
    const double run_seconds = argc > 1 ? std::atof(argv[1]) : 480.0;
    const int gap_min_us = argc > 2 ? std::atoi(argv[2]) : 8000;
    const int gap_max_us = argc > 3 ? std::atoi(argv[3]) : 15000;
    constexpr uint64_t kTolNs = 300000;  // 300µs 判别容差（futex 唤醒 ~50-200µs）

    LockFreeTaskExecutor exec(1024);
    if (!exec.start()) {
        std::fprintf(stderr, "FATAL: start failed\n");
        return 2;
    }

    const size_t slot_count = 1u << 21;  // 2M 槽位上限（480s @8ms = 60k 任务）
    auto slots_ptr = std::make_unique<Slot[]>(slot_count);
    Slot* slots = slots_ptr.get();
    std::atomic<uint64_t> seq{0};

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::duration_cast<
            std::chrono::steady_clock::duration>(std::chrono::duration<double>(
            run_seconds));

    std::mt19937 rng(20260930u);
    std::uniform_int_distribution<int> gap_us(gap_min_us, gap_max_us);

    uint64_t pushed = 0;
    uint64_t drop = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        const uint64_t id = seq.fetch_add(1, std::memory_order_relaxed);
        Slot& s = slots[id % slot_count];
        s.push_ns.store(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count(),
            std::memory_order_relaxed);
        s.exec_ns.store(0, std::memory_order_relaxed);
        Slot* sp = &s;
        if (exec.push_task([sp] {
                sp->exec_ns.store(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count(),
                    std::memory_order_relaxed);
            })) {
            ++pushed;
        } else {
            ++drop;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(gap_us(rng)));
    }

    while (exec.processed_count() < pushed) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    exec.stop();

    // ---- 统计 ----
    std::vector<double> delays_us;
    delays_us.reserve(pushed);
    for (uint64_t i = 0; i < pushed; ++i) {
        Slot& s = slots[i % slot_count];
        const uint64_t p = s.push_ns.load(std::memory_order_relaxed);
        const uint64_t e = s.exec_ns.load(std::memory_order_relaxed);
        if (e == 0) continue;
        delays_us.push_back(static_cast<double>(e - p) / 1000.0);
    }
    std::sort(delays_us.begin(), delays_us.end());
    const size_t n = delays_us.size();
    auto pct = [&](double q) -> double {
        if (n == 0) return -1.0;
        return delays_us[static_cast<size_t>(q * static_cast<double>(n - 1))];
    };

    // ---- 丢失唤醒判别：exec_i vs push_{i+1} ----
    // 真实丢失唤醒签名（三联）：
    //   S1: exec_i >= next_push                （本任务的 bump 丢失，靠下一次
    //                                            push 的 bump 才醒来）
    //   S2: exec_i - next_push <= 400_000ns    （醒来即执行，futex 唤醒量级）
    //   S3: delay_{i+1} <= 200us               （下一任务作为唤醒者立即被执行）
    // 仅满足"漂进容差窗口"的重尾 stall（exec < next_push 或 exec 远晚于
    // next_push）不算。
    size_t lost_wakeup_strict = 0;
    size_t worker_stall_cases = 0;   // 延迟大但早于下一次 push 执行
    size_t beyond_one_period = 0;    // delay > gap_prev（超过一个完整周期）
    int printed = 0;
    for (uint64_t i = 1; i + 1 < pushed; ++i) {
        Slot& s = slots[i % slot_count];
        const uint64_t p = s.push_ns.load(std::memory_order_relaxed);
        const uint64_t e = s.exec_ns.load(std::memory_order_relaxed);
        if (e == 0) continue;
        const double delay = static_cast<double>(e - p) / 1000.0;
        if (delay <= 500.0) continue;  // 只分析 >500µs 的样本
        const uint64_t prev =
            slots[(i - 1) % slot_count].push_ns.load(std::memory_order_relaxed);
        const uint64_t next =
            slots[(i + 1) % slot_count].push_ns.load(std::memory_order_relaxed);
        const uint64_t e_next =
            slots[(i + 1) % slot_count].exec_ns.load(std::memory_order_relaxed);
        const double gap_prev = static_cast<double>(p - prev) / 1000.0;
        const double gap_next = static_cast<double>(next - p) / 1000.0;
        const double d_next_us = static_cast<double>(e - next) / 1000.0;
        const double next_delay_us =
            e_next ? static_cast<double>(e_next - next) / 1000.0 : -1.0;
        const bool s1 = e >= next;
        const bool s2 = e - next <= 400000;
        const bool s3 = e_next != 0 && (e_next - next) <= 200000;
        const char* cls;
        if (s1 && s2 && s3) {
            ++lost_wakeup_strict;
            cls = "LOST_WAKE";
        } else {
            ++worker_stall_cases;
            cls = "stall";
        }
        if (delay > gap_prev) ++beyond_one_period;
        if (printed < 40) {
            std::printf("  %8llu d=%9.1fus gp=%9.1fus gn=%9.1fus "
                        "exec-next=%+9.1fus nextDelay=%8.1fus %s\n",
                        static_cast<unsigned long long>(i), delay, gap_prev,
                        gap_next, d_next_us, next_delay_us, cls);
            ++printed;
        }
    }

    std::printf("==== CR-005 park wakeup latency stress ====\n");
    std::printf("run=%.0fs gaps=[%d,%d]us pushed=%llu dropped=%llu sampled=%zu\n",
                run_seconds, gap_min_us, gap_max_us,
                static_cast<unsigned long long>(pushed),
                static_cast<unsigned long long>(drop), n);
    if (n == 0) {
        std::printf("no samples\n");
        return 2;
    }
    const double med = pct(0.50);
    std::printf("min=%.1fus p50=%.1fus p90=%.1fus p99=%.1fus p99.9=%.1fus max=%.1fus\n",
                delays_us.front(), med, pct(0.90), pct(0.99), pct(0.999),
                delays_us.back());
    std::printf("max/median ratio = %.1f\n",
                delays_us.back() / (med > 0 ? med : 1.0));
    std::printf("discriminator (>500us samples): lost_wakeup_strict="
                "(exec>=next_push & exec-next<=400us & nextTaskDelay<=200us)"
                " = %zu ; worker_stall_cases=%zu ; delay>gap_prev=%zu\n",
                lost_wakeup_strict, worker_stall_cases, beyond_one_period);

    std::printf("---- verdict ----\n");
    if (lost_wakeup_strict > 0) {
        std::printf("CR-005 CONFIRMED(statistical): %zu task(s) match the full "
                    "lost-wakeup signature (executed only upon the NEXT push, "
                    "woke+ran it immediately).\n",
                    lost_wakeup_strict);
        return 0;
    }
    std::printf("CR-005 NOT REPRODUCED: no task matched the full lost-wakeup "
                "signature (all >500us delays executed before the following "
                "push or without the wake-and-run-next pattern = worker-side "
                "stalls). Lost-wakeup window is code-reasoned but not "
                "runtime-reproducible on x86 TSO.\n");
    return 1;
}
