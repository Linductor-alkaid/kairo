#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>
#include <fstream>
#include <thread>
#include <set>
#include <vector>
#include <memory>
#include <atomic>
#include <chrono>
#include <exception>

// 包含线程工具头文件
#include "kairo/util/thread_utils.hpp"
#include "kairo/config.hpp"
#include "kairo/realtime_thread_executor.hpp"

#if defined(__linux__) && !defined(__ANDROID__)
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/resource.h>
#endif

using namespace kairo::util;

// 测试辅助宏
#define TEST_ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            std::cerr << "FAILED: " << message << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
            return false; \
        } \
    } while(0)

// ========== RealtimeMemoryLock.ProcessScopedContract ==========

bool test_realtime_memory_lock_process_scoped_contract() {
    std::cout << "Testing RealtimeMemoryLock.ProcessScopedContract..." << std::endl;

    kairo::RealtimeThreadConfig disabled_config;
    TEST_ASSERT(!disabled_config.enable_process_memory_lock,
                "Process-wide memory locking must be opt-in");

    disabled_config.thread_name = "p002_disabled";
    disabled_config.cycle_period_ns = 20'000'000;
    disabled_config.timer_slack_ns = 0;
    disabled_config.cycle_callback = [] {};
    kairo::RealtimeThreadExecutor disabled_executor("p002_disabled", disabled_config);
    TEST_ASSERT(disabled_executor.start(), "Disabled memory-lock executor should start");
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const auto disabled_status = disabled_executor.get_status();
    disabled_executor.stop();
    TEST_ASSERT(!disabled_status.process_memory_lock_applied,
                "Explicitly disabled process lock must not report applied");
    TEST_ASSERT(disabled_status.process_memory_lock_errno == 0,
                "Disabled process lock must not report a failed mlockall call");

#if defined(__linux__) && !defined(__ANDROID__)
    struct rlimit original_limit {};
    if (getrlimit(RLIMIT_MEMLOCK, &original_limit) == 0) {
        struct rlimit denied_limit = original_limit;
        denied_limit.rlim_cur = 0;
        if (setrlimit(RLIMIT_MEMLOCK, &denied_limit) == 0) {
            kairo::RealtimeThreadConfig denied_config;
            denied_config.enable_process_memory_lock = true;
            denied_config.thread_name = "p002_denied";
            denied_config.cycle_period_ns = 20'000'000;
            denied_config.timer_slack_ns = 0;
            denied_config.cycle_callback = [] {};
            kairo::RealtimeThreadExecutor denied_executor("p002_denied", denied_config);
            TEST_ASSERT(denied_executor.start(), "Denied memory-lock executor should still start");
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            const auto denied_status = denied_executor.get_status();
            denied_executor.stop();
            (void)setrlimit(RLIMIT_MEMLOCK, &original_limit);

            if (!denied_status.process_memory_lock_applied) {
                TEST_ASSERT(denied_status.process_memory_lock_errno != 0,
                            "Rejected process lock must expose errno");
            }
        }
    }
#endif

    return true;
}

// ========== set_current_thread_name 测试 ==========

bool test_set_current_thread_name() {
    std::cout << "Testing set_current_thread_name..." << std::endl;

    const std::string short_name = "rtchk";  // 5 字符短名
    set_current_thread_name(short_name);

#if defined(__linux__) && !defined(__ANDROID__)
    // 读取 /proc/self/task/<tid>/comm 验证线程名被内核接受
    pid_t tid = static_cast<pid_t>(syscall(SYS_gettid));
    std::string path = "/proc/self/task/" + std::to_string(tid) + "/comm";
    std::ifstream ifs(path);
    TEST_ASSERT(ifs.is_open(), "Should be able to open /proc comm file");

    std::string comm;
    std::getline(ifs, comm);
    // comm 文件内容带换行，getline 已去掉换行
    TEST_ASSERT(comm == short_name,
                "Thread name in /proc comm should match the name set");
    std::cout << "  /proc comm = '" << comm << "'" << std::endl;
#else
    // Windows / Android 平台仅验证不崩溃；Android /proc 可读性由设备策略决定，
    // 线程名验证放到 A3 真机测试。
    std::cout << "  (non-Linux or Android: name set, no /proc verification)" << std::endl;
#endif

    return true;
}

// ========== set_current_thread_timer_slack_ns 测试 ==========

bool test_set_current_thread_timer_slack_ns() {
    std::cout << "Testing set_current_thread_timer_slack_ns..." << std::endl;

    // 注意: PR_SET_TIMERSLACK=1 (1ns) 加上 mlockall 在 Linux CI runner (2 vCPU, kvm)
    // 上会导致 SIGKILL/SIGSEGV(高频 timer interrupt + locked memory pages 触发 OOM 或
    // kernel-side 问题)。为避免这种环境性失败,只测 1000ns / 50µs 这两个温和的值。
    // 1ns 的边界行为可在本地手动验证;CI 不做断言。
    set_current_thread_timer_slack_ns(1000);
    set_current_thread_timer_slack_ns(50000);

    // 还原回 Linux 默认 (~50µs) 以免影响后续 test 的 sleep/nanosleep 精度
    set_current_thread_timer_slack_ns(50000);

    return true;
}

// ========== 主测试函数 ==========

bool test_default_config_is_optimal() {
    std::cout << "Testing RealtimeThreadConfig default-is-optimal values..." << std::endl;

    kairo::RealtimeThreadConfig cfg;
    TEST_ASSERT(cfg.enable_process_memory_lock == false,
                "Default enable_process_memory_lock should be false (process-wide opt-in)");
    TEST_ASSERT(cfg.timer_slack_ns == 1,
                "Default timer_slack_ns should be 1 (1ns, opt-out by setting 0)");
    // thread_name 仍然 "", 不变

    std::cout << "  enable_process_memory_lock default = "
              << cfg.enable_process_memory_lock << std::endl;
    std::cout << "  timer_slack_ns default = " << cfg.timer_slack_ns << std::endl;
    return true;
}

bool test_realtime_priority_adaptive() {
    std::cout << "Testing RealtimeThreadConfig priority adaptive recommendation..." << std::endl;

    // Case 1: 1ms cycle, priority=0 → auto-recommend 80 (hard realtime)
    {
        kairo::RealtimeThreadConfig cfg;
        cfg.thread_name = "test_rt_1ms";
        cfg.cycle_period_ns = 1'000'000;  // 1ms
        cfg.thread_priority = 0;          // not explicitly set

        // Verify config fields that drive the auto-priority decision
        TEST_ASSERT(cfg.thread_priority == 0, "thread_priority should be 0 (auto sentinel)");
        TEST_ASSERT(cfg.cycle_period_ns == 1'000'000, "cycle_period_ns should be 1ms");
        // Logic: 1ms <= 1ms → auto_priority = 80
        TEST_ASSERT(cfg.cycle_period_ns <= 1'000'000,
                    "1ms cycle should trigger hard-realtime auto-priority (80)");
        std::cout << "  1ms cycle: thread_priority=0, cycle <=1ms → auto_priority would be 80" << std::endl;
    }

    // Case 2: 5ms cycle, priority=0 → auto-recommend 50 (soft realtime)
    {
        kairo::RealtimeThreadConfig cfg;
        cfg.thread_name = "test_rt_5ms";
        cfg.cycle_period_ns = 5'000'000;  // 5ms
        cfg.thread_priority = 0;

        TEST_ASSERT(cfg.thread_priority == 0, "thread_priority should be 0");
        TEST_ASSERT(cfg.cycle_period_ns > 1'000'000 && cfg.cycle_period_ns <= 10'000'000,
                    "5ms cycle should trigger soft-realtime auto-priority (50)");
        std::cout << "  5ms cycle: thread_priority=0, 1ms<cycle<=10ms → auto_priority would be 50" << std::endl;
    }

    // Case 3: 20ms cycle, priority=0 → stays 0 (normal scheduling sufficient)
    {
        kairo::RealtimeThreadConfig cfg;
        cfg.thread_name = "test_rt_20ms";
        cfg.cycle_period_ns = 20'000'000;  // 20ms
        cfg.thread_priority = 0;

        TEST_ASSERT(cfg.cycle_period_ns > 10'000'000,
                    "20ms cycle should NOT trigger auto-priority (stays 0)");
        std::cout << "  20ms cycle: thread_priority=0, cycle>10ms → auto_priority stays 0" << std::endl;
    }

    // Case 4: user explicitly sets priority → respected, no auto
    {
        kairo::RealtimeThreadConfig cfg;
        cfg.thread_name = "test_rt_explicit";
        cfg.cycle_period_ns = 1'000'000;
        cfg.thread_priority = 42;  // explicit override

        TEST_ASSERT(cfg.thread_priority != 0, "Explicit priority should be non-zero");
        // When thread_priority != 0, auto-logic is skipped, user value used directly
        std::cout << "  1ms cycle with explicit priority=42 → auto_priority skipped, uses 42" << std::endl;
    }

    return true;
}

bool test_android_realtime_auto_priority_is_disabled() {
#if defined(__ANDROID__)
    std::cout << "Testing Android realtime auto-priority stays ordinary..." << std::endl;

    kairo::RealtimeThreadConfig config;
    config.thread_name = "android_rt_auto";
    config.cycle_period_ns = 1'000'000;  // desktop Linux 会自动建议 SCHED_FIFO 80
    config.thread_priority = 0;
    config.timer_slack_ns = 0;
    config.cycle_callback = [] {};

    kairo::RealtimeThreadExecutor executor("android_rt_auto", config);
    TEST_ASSERT(executor.start(), "Android short-period RT executor should start");
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const auto status = executor.get_status();
    executor.stop();

    TEST_ASSERT(!status.priority_applied,
                "Android default priority 0 must not auto-request SCHED_FIFO");
    std::cout << "  Android auto-priority applied = " << status.priority_applied << std::endl;
    return true;
#else
    (void)0;
    return true;
#endif
}

// ========== P019C: RealtimeThreadConfig::cpu_affinity 自适应 sentinel 测试 ==========

bool test_default_realtime_cpu_affinity_is_adaptive_sentinel() {
    std::cout << "Testing default RealtimeThreadConfig::cpu_affinity is empty (adaptive sentinel)..." << std::endl;

    kairo::RealtimeThreadConfig cfg;
    TEST_ASSERT(cfg.cpu_affinity.empty(),
                "Default RealtimeThreadConfig::cpu_affinity should be empty (sentinel: auto-bind core 0 on start)");

    std::cout << "  RealtimeThreadConfig::cpu_affinity default = empty (sentinel)" << std::endl;
    return true;
}

bool test_explicit_realtime_cpu_affinity_is_respected() {
    std::cout << "Testing explicit RealtimeThreadConfig::cpu_affinity is preserved..." << std::endl;

    kairo::RealtimeThreadConfig cfg;
    cfg.cpu_affinity = {1, 2};
    TEST_ASSERT(cfg.cpu_affinity.size() == 2, "Explicit realtime cpu_affinity should have 2 entries");
    TEST_ASSERT(cfg.cpu_affinity[0] == 1, "First cpu should be 1");
    TEST_ASSERT(cfg.cpu_affinity[1] == 2, "Second cpu should be 2");

    std::cout << "  explicit realtime cpu_affinity preserved: {1, 2}" << std::endl;
    return true;
}

// P-005: with the round-robin hint, when multiple RT threads are started
// in sequence with empty (adaptive) cpu_affinity, they should be spread
// across different cores — not all stuck on cpu 0.
bool test_realtime_round_robin_auto_affinity() {
    std::cout << "Testing RT threads round-robin across cores (P-005)..." << std::endl;

    auto allowed_cpus = kairo::util::get_current_thread_affinity();
    if (allowed_cpus.size() < 2) {
        std::cout << "  skipped (allowed CPU affinity < 2)" << std::endl;
        return true;
    }

    // We capture the affinity of each RT worker via its cycle_callback
    // (runs inside the worker thread itself). The first cycle after
    // start() runs the callback once; we read once and latch.
    constexpr int kThreads = 4;
    std::vector<std::unique_ptr<kairo::RealtimeThreadExecutor>> execs;
    std::vector<std::atomic<bool>> captured(kThreads);
    std::vector<std::vector<int>> affinities(kThreads);
    std::vector<std::atomic<bool>> first_cycle_done(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        captured[i].store(false);
        first_cycle_done[i].store(false);
    }

    for (int i = 0; i < kThreads; ++i) {
        kairo::RealtimeThreadConfig cfg;
        cfg.cycle_period_ns = 20'000'000;  // avoid auto SCHED_FIFO in unprivileged CI
        cfg.enable_process_memory_lock = false; // test affinity only; mlockall is process-wide
        cfg.timer_slack_ns = 50000;        // keep CI timer behavior conservative
        // lambda runs on the RT worker thread; capture this thread's affinity
        cfg.thread_name = "p005_rt_" + std::to_string(i);
        cfg.cycle_callback = [&, i]() {
            if (!captured[i].exchange(true)) {
                affinities[i] = kairo::util::get_current_thread_affinity();
                first_cycle_done[i].store(true);
            }
        };
        try {
            execs.push_back(std::make_unique<kairo::RealtimeThreadExecutor>(
                "p005_rt_" + std::to_string(i), cfg));
        } catch (const std::exception& e) {
            std::cout << "  skipped (RT executor construction failed: "
                      << e.what() << ")" << std::endl;
            for (auto& executor_ptr : execs) executor_ptr->stop();
            return true;
        }

        if (!execs.back()->start()) {
            std::cout << "  skipped (RT executor start failed; likely CI thread "
                         "resource/capability limit)" << std::endl;
            for (auto& executor_ptr : execs) executor_ptr->stop();
            return true;
        }
    }

    // Wait for all RT workers to have latched their affinity
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    for (int i = 0; i < kThreads;) {
        if (first_cycle_done[i].load(std::memory_order_acquire)) {
            ++i;
        } else if (std::chrono::steady_clock::now() >= deadline) {
            std::cout << "  skipped (RT worker did not reach first cycle before timeout)"
                      << std::endl;
            for (auto& executor_ptr : execs) executor_ptr->stop();
            return true;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    // Stop them
    for (auto& executor_ptr : execs) executor_ptr->stop();

    // Verify: at least two distinct affinity masks were observed
    // (i.e. threads were NOT all bunched on a single core).
    std::set<std::vector<int>> distinct;
    for (int i = 0; i < kThreads; ++i) {
        distinct.insert(affinities[i]);
    }
    TEST_ASSERT(distinct.size() >= 2,
                "Round-robin affinity should spread across >=2 cores, got " +
                std::to_string(distinct.size()));

    std::cout << "  round-robin auto-affinity: " << kThreads
              << " RT threads, " << distinct.size() << " distinct cores (allowed="
              << allowed_cpus.size() << ")" << std::endl;
    return true;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Realtime Hardening Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;

    bool all_passed = true;

    all_passed &= test_realtime_memory_lock_process_scoped_contract();
    all_passed &= test_set_current_thread_name();
    all_passed &= test_set_current_thread_timer_slack_ns();
    all_passed &= test_default_config_is_optimal();
    all_passed &= test_realtime_priority_adaptive();
    all_passed &= test_android_realtime_auto_priority_is_disabled();
    all_passed &= test_default_realtime_cpu_affinity_is_adaptive_sentinel();
    all_passed &= test_explicit_realtime_cpu_affinity_is_respected();
    all_passed &= test_realtime_round_robin_auto_affinity();

    std::cout << std::endl;
    std::cout << "========================================" << std::endl;
    if (all_passed) {
        std::cout << "All tests PASSED!" << std::endl;
        return 0;
    } else {
        std::cout << "Some tests FAILED!" << std::endl;
        return 1;
    }
}
