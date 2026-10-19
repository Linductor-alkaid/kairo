/// @file test_reliability_lifecycle_cr054_055_105_113_114.cpp
/// 0.7.2 §4.3 生命周期可靠性收尾的独立验证（验证代理编写，非实现方测试）：
///
/// - CR-054：RealtimeThreadExecutor 内置周期循环可中断停止
///   （stop 延迟与周期解耦；stop 请求后不再发起新一轮用户回调）。
/// - CR-055：BlockingIoExecutor 用户 wakeup() 移出 lifecycle_mutex_
///   （重入 request_stop/stop 不自死锁）+ stop 终结栅栏（并发双 stop
///   都在终结完成后返回）。
/// - CR-105：WorkerHandle 在 facade（Executor 实例）析构后经 liveness
///   weak 令牌退化为无操作，不再悬垂解引用。
/// - CR-113/114：ThreadPool resize 事务化（线程创建失败整体回滚返回
///   false、池状态不变且后续可用）+ worker 线程内缩容显式拒绝。
///
/// 说明：RealtimeThreadExecutor / BlockingIoExecutor 是 src 内部类型，
/// tests/CMakeLists.txt 已把 ../src 加进本目录所有测试的包含路径。

#include <kairo/blocking_io.hpp>
#include <kairo/config.hpp>
#include <kairo/executor.hpp>
#include <kairo/interfaces.hpp>
#include <kairo/types.hpp>

#include "kairo/realtime_thread_executor.hpp"
#include "kairo/blocking_io_executor.hpp"
#include "kairo/thread_pool/thread_pool.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <gtest/gtest.h>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

using namespace kairo;
using namespace std::chrono_literals;

namespace {

// ---------------------------------------------------------------------------
// CR-054 辅助：统计周期回调执行轮数
// ---------------------------------------------------------------------------

struct CycleCounter {
    std::atomic<int> rounds{0};
    std::chrono::steady_clock::time_point first_round{};

    void tick() {
        if (rounds.fetch_add(1) == 0) {
            first_round = std::chrono::steady_clock::now();
        }
    }
};

RealtimeThreadConfig large_cycle_config(CycleCounter& counter,
                                        int64_t period_ms) {
    RealtimeThreadConfig config;
    config.thread_name = "cr054_rt";
    // 0 优先级：测试环境无 SCHED_FIFO 权限时避免噪音
    config.thread_priority = 0;
    config.cycle_period_ns = period_ms * 1'000'000;
    config.cycle_callback = [&counter]() { counter.tick(); };
    return config;
}

// ---------------------------------------------------------------------------
// CR-054 辅助：手动 tick 的 ICycleManager（驱动外部周期路径）
// ---------------------------------------------------------------------------

class ManualCycleManager : public ICycleManager {
public:
    bool register_cycle(const std::string& name, int64_t /*period_ns*/,
                        std::function<void()> callback) noexcept(false) override {
        std::lock_guard<std::mutex> lock(mutex_);
        callbacks_[name] = std::move(callback);
        return true;
    }

    bool start_cycle(const std::string& name) noexcept(false) override {
        // 阻塞在 worker 线程上：周期 tick 完全由测试线程经 fire_tick()
        // 手动触发（模拟外部周期管理器投递 tick）。stop_cycle() 后退出。
        std::unique_lock<std::mutex> lock(mutex_);
        started_ = true;
        started_cv_.notify_all();
        stop_cv_.wait(lock, [this, &name] {
            return stopped_.count(name) != 0;
        });
        return true;
    }

    void stop_cycle(const std::string& name) noexcept(false) override {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_.insert(name);
        stop_cv_.notify_all();
    }

    CycleStatistics get_statistics(const std::string& name) const override {
        CycleStatistics stats;
        stats.name = name;
        return stats;
    }

    void fire_tick(const std::string& name) {
        std::function<void()> cb;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = callbacks_.find(name);
            if (it != callbacks_.end()) cb = it->second;
        }
        if (cb) cb();
    }

    void wait_started() {
        std::unique_lock<std::mutex> lock(mutex_);
        started_cv_.wait(lock, [this] { return started_; });
    }

private:
    std::mutex mutex_;
    std::condition_variable started_cv_;
    std::condition_variable stop_cv_;
    std::map<std::string, std::function<void()>> callbacks_;
    std::set<std::string> stopped_;
    bool started_ = false;
};

// ---------------------------------------------------------------------------
// CR-055 / CR-105 辅助：可唤醒的阻塞 worker
// ---------------------------------------------------------------------------

struct WakeupCtl {
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> woke{false};
    // CR-055 重入注入点：worker::wakeup() 里调用（若已安装）。
    std::function<void()> on_wakeup;
};

class BlockingTestWorker : public IBlockingIoWorker {
public:
    explicit BlockingTestWorker(std::shared_ptr<WakeupCtl> ctl)
        : ctl_(std::move(ctl)) {}

    void run(StopToken stop_token) override {
        std::unique_lock<std::mutex> lock(ctl_->mutex);
        while (!stop_token.stop_requested()) {
            ctl_->cv.wait_for(lock, 50ms);
        }
    }

    void wakeup() noexcept override {
        ctl_->woke.store(true, std::memory_order_release);
        std::function<void()> reentry;
        {
            // 注意：不能持锁拷贝 on_wakeup 后在锁内调用——重入路径可能
            // 再次进入 wakeup。拷贝后锁外调用。
            std::lock_guard<std::mutex> lock(ctl_->mutex);
            reentry = ctl_->on_wakeup;
        }
        if (reentry) reentry();
        ctl_->cv.notify_all();
    }

private:
    std::shared_ptr<WakeupCtl> ctl_;
};

BlockingWorkerSpec make_worker_spec(
    const std::string& name, std::shared_ptr<WakeupCtl> ctl) {
    BlockingWorkerSpec spec;
    spec.name = name;
    spec.config.thread_name = name;
    spec.worker = std::make_unique<BlockingTestWorker>(std::move(ctl));
    return spec;
}

// 在独立线程带超时地运行 fn：超时视为死锁/挂起，返回 false。
// （超时的线程被分离：挂起的 fn 无法安全强杀，测试二进制在所有用例
// 结束后正常退出；断言失败先于进程退出报告。）
bool run_with_timeout(std::function<void()> fn, std::chrono::milliseconds budget) {
    auto done = std::make_shared<std::promise<void>>();
    auto finished = done->get_future();
    std::thread worker([fn = std::move(fn), done]() {
        fn();
        done->set_value();
    });
    const bool completed = finished.wait_for(budget) == std::future_status::ready;
    if (completed) {
        worker.join();
    } else {
        worker.detach();
    }
    return completed;
}

} // namespace

// ===========================================================================
// CR-054：RT 可中断停止
// ===========================================================================

TEST(ReliabilityCr054, StopLatencyDecoupledFromLargeCyclePeriod) {
    // 大周期（300ms）下 stop() 返回延迟不再随周期线性。
    // 旧实现 sleep_until 不可打断：平均延迟 ~150ms、最坏 ~300ms。
    // 宽松上界 120ms（机器负载敏感，故意不卡紧；新实现正常 <5ms）。
    constexpr int64_t kPeriodMs = 300;
    constexpr auto kStopBudget = 120ms;
    // 循环 3 轮降低偶发调度尖刺导致的假阳性。
    for (int round = 0; round < 3; ++round) {
        CycleCounter counter;
        RealtimeThreadExecutor executor("cr054_latency", large_cycle_config(counter, kPeriodMs));
        ASSERT_TRUE(executor.start());

        // 等待至少一轮用户回调，确保线程停在 cv 等待里（最坏情形）。
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (counter.rounds.load() < 1 &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(5ms);
        }
        ASSERT_GE(counter.rounds.load(), 1);

        // 再睡到周期中段之后：停机请求落在等待窗口内的概率最大化
        //（旧实现此处 stop 延迟接近一整个周期）。
        std::this_thread::sleep_for(150ms);

        const auto t0 = std::chrono::steady_clock::now();
        ASSERT_TRUE(executor.stop_and_join());
        const auto elapsed = std::chrono::steady_clock::now() - t0;
        EXPECT_LT(elapsed, kStopBudget)
            << "round " << round << ": stop latency "
            << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
            << "ms still coupled to " << kPeriodMs << "ms cycle";
        EXPECT_FALSE(executor.get_status().is_running);
    }
}

TEST(ReliabilityCr054, NoNewCallbackRoundAfterStopRequest) {
    // stop() 返回后：用户回调不再执行新一轮（计数冻结），且没有回调
    // 仍在并发执行（join 保证）。
    CycleCounter counter;
    RealtimeThreadExecutor executor("cr054_nocb", large_cycle_config(counter, 20));
    ASSERT_TRUE(executor.start());

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (counter.rounds.load() < 3 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_GE(counter.rounds.load(), 1);

    ASSERT_TRUE(executor.stop_and_join());
    const int rounds_at_stop = counter.rounds.load();

    // 2 个完整周期后计数不得增长（20ms 周期 ×2 + 余量）。
    std::this_thread::sleep_for(150ms);
    EXPECT_EQ(counter.rounds.load(), rounds_at_stop)
        << "user cycle callback ran a new round after stop returned";
}

TEST(ReliabilityCr054, CycleManagerTickAfterStopSkipsUserCallback) {
    // 外部周期管理器路径（cycle_loop）：stop 返回后管理器仍可能投递
    // 一次 tick（残余在途 tick），回调前的 running_ 复核必须跳过用户
    // 回调——fire_tick 直接调用 cycle_loop 本体，等价于"管理器投递的
    // 迟到 tick"。
    CycleCounter counter;
    RealtimeThreadConfig config = large_cycle_config(counter, 50);
    ManualCycleManager manager;
    config.cycle_manager = &manager;

    RealtimeThreadExecutor executor("cr054_mgr", config);
    ASSERT_TRUE(executor.start());
    manager.wait_started();

    // 停止前的一轮 tick 正常执行用户回调。
    manager.fire_tick("cr054_mgr");
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (counter.rounds.load() < 1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(2ms);
    }
    ASSERT_GE(counter.rounds.load(), 1);

    ASSERT_TRUE(executor.stop_and_join());

    // stop 返回后的迟到 tick：cycle_loop 仍被调（管理器投递），但用户
    // 回调不得执行。
    manager.fire_tick("cr054_mgr");
    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(counter.rounds.load(), 1)
        << "late cycle-manager tick started a new user callback round after stop";
}

// ===========================================================================
// CR-055：BlockingIO wakeup 重入 + stop 终结栅栏
// ===========================================================================

TEST(ReliabilityCr055, UserWakeupReentrancyIntoRequestStopDoesNotDeadlock) {
    // 用户 wakeup() 内部重入 request_stop()（聚合器/看门狗场景）：
    // 旧实现持 lifecycle_mutex_ 调 wakeup → 重入再次取锁 = 自死锁。
    // 直接在 BlockingIoExecutor 层验证（无 facade 间接层）。
    auto ctl = std::make_shared<WakeupCtl>();
    BlockingIoExecutor executor(
        "cr055_reentrant", BlockingIoConfig{},
        std::make_unique<BlockingTestWorker>(ctl));
    ASSERT_TRUE(executor.start());

    // 安装重入注入：wakeup 里再次进入 executor.request_stop()。
    ctl->on_wakeup = [&executor]() { executor.request_stop(); };

    // 旧实现在此死锁；新实现必须立刻返回。
    ASSERT_TRUE(run_with_timeout([&executor]() { executor.request_stop(); }, 5s))
        << "request_stop() deadlocked in user wakeup() re-entry (CR-055 regression)";
    // stop 必须已完成：wakeup 已投递、worker 已观察 stop token。
    ASSERT_TRUE(run_with_timeout([&executor]() { executor.stop(); }, 5s))
        << "stop() did not finish after wakeup re-entry";
    const auto status = executor.get_status();
    EXPECT_FALSE(status.is_running);
    EXPECT_GE(status.wakeup_count, 1U);
    ctl->on_wakeup = nullptr;
}

// 【验证代理实测：验收缺口，暂以 DISABLED 保留复现】
// 任务验收要求"wakeup() 重入 request_stop()/stop() 不死锁"。
// 实测：request_stop() 重入已通过（见上），但 stop() 重入死锁——
// 外层 stop() 持有终结栅栏后在锁外同步调用 perform_wakeup()；用户
// wakeup() 内重入 stop() 时见 fence=true 进入 stop_completion_cv_
// 等待，而栅栏只能由被阻塞的外层 stop() 在 join 后清除 → 循环等待。
// 最小复现：tests/test_reliability_...cpp 本用例 + /tmp 独立探针
// （BlockingIoExecutor 直连版，timeout 15s exit=124 挂起）。
// 等主循环修复（如：终结者线程自身跳过 fence 等待 / wakeup 先于
// fence 设置）后去掉 DISABLED_ 前缀复验。
TEST(ReliabilityCr055, UserWakeupReentrancyIntoStopViaFacadeHandle) {
    // 同上，经 facade 句柄（manager 间接层）重入 stop()。
    auto ctl = std::make_shared<WakeupCtl>();
    Executor executor;
    ExecutorConfig config;
    config.min_threads = 1;
    config.max_threads = 1;
    ASSERT_TRUE(executor.initialize(config));

    auto handle = executor.start_worker(make_worker_spec("cr055_reent_stop", ctl));
    ASSERT_TRUE(handle.started()) << "worker start admission failed";

    ctl->on_wakeup = [&handle]() { handle.stop(); };

    ASSERT_TRUE(run_with_timeout([&handle]() { handle.stop(); }, 5s))
        << "stop() deadlocked in user wakeup() re-entry (CR-055 regression)";
    const auto status = executor.get_blocking_io_worker_status("cr055_reent_stop");
    EXPECT_FALSE(status.is_running);
    EXPECT_GE(status.wakeup_count, 1U);
    ctl->on_wakeup = nullptr;
}

TEST(ReliabilityCr055, ConcurrentDoubleStopWaitsForFinalization) {
    // 并发双 stop()：第二个不得因 join 移交后的 moved-from 句柄提前
    // 返回，必须等终结（join/状态发布）完成后才返回。
    for (int round = 0; round < 8; ++round) {
        auto ctl = std::make_shared<WakeupCtl>();
        BlockingIoExecutor executor(
            "cr055_double", BlockingIoConfig{},
            std::make_unique<BlockingTestWorker>(ctl));
        ASSERT_TRUE(executor.start());

        auto stopper = [&executor]() { executor.stop(); };
        ASSERT_TRUE(run_with_timeout([&] {
            std::thread t1(stopper);
            std::thread t2(stopper);
            t1.join();
            t2.join();
        }, 10s)) << "round " << round << ": concurrent double stop() hung";

        const auto status = executor.get_status();
        EXPECT_FALSE(status.is_running) << "round " << round;
        EXPECT_EQ(status.stop_reason, BlockingIoStopReason::Requested) << "round " << round;

        // 终结后的第三次 stop 必须立即返回（栅栏已清除）。
        ASSERT_TRUE(run_with_timeout(stopper, 2s))
            << "round " << round << ": post-finalization stop() hung";
    }
}

TEST(ReliabilityCr055, ConcurrentStopViaFacadeHandles) {
    // facade 句柄版并发双 stop：两个线程同时 handle.stop()，都返回后
    // 状态必须一致（is_running=false、stop_reason=Requested），循环 8
    // 轮验证稳定。
    for (int round = 0; round < 8; ++round) {
        auto ctl = std::make_shared<WakeupCtl>();
        Executor executor;
        ExecutorConfig config;
        config.min_threads = 1;
        config.max_threads = 1;
        ASSERT_TRUE(executor.initialize(config));

        auto handle = executor.start_worker(make_worker_spec("cr055_double", ctl));
        ASSERT_TRUE(handle.started());

        auto stopper = [&handle]() { handle.stop(); };
        ASSERT_TRUE(run_with_timeout([&] {
            auto t1 = std::thread(stopper);
            auto t2 = std::thread(stopper);
            t1.join();
            t2.join();
        }, 10s)) << "round " << round << ": concurrent double stop() hung";

        const auto status = executor.get_blocking_io_worker_status("cr055_double");
        EXPECT_FALSE(status.is_running) << "round " << round;
        EXPECT_EQ(status.stop_reason, BlockingIoStopReason::Requested) << "round " << round;

        // 终结后的第三次 stop 必须立即返回（栅栏已清除）。
        ASSERT_TRUE(run_with_timeout(stopper, 2s))
            << "round " << round << ": post-finalization stop() hung";
    }
}

TEST(ReliabilityCr055, ConcurrentRequestStopAndStop) {
    // request_stop 与 stop 并发：无死锁，wakeup 恰好补发一次
    //（request_stop_locked 的 exchange 保证只有一个调用方负责 wakeup）。
    for (int round = 0; round < 8; ++round) {
        auto ctl = std::make_shared<WakeupCtl>();
        Executor executor;
        ExecutorConfig config;
        config.min_threads = 1;
        config.max_threads = 1;
        ASSERT_TRUE(executor.initialize(config));

        auto handle = executor.start_worker(make_worker_spec("cr055_mixed", ctl));
        ASSERT_TRUE(handle.started());

        ASSERT_TRUE(run_with_timeout([&] {
            std::thread t1([&handle]() { handle.request_stop(); });
            std::thread t2([&handle]() { handle.stop(); });
            t1.join();
            t2.join();
        }, 10s)) << "round " << round << ": concurrent request_stop/stop hung";

        const auto status = executor.get_blocking_io_worker_status("cr055_mixed");
        EXPECT_FALSE(status.is_running);
        EXPECT_EQ(status.stop_reason, BlockingIoStopReason::Requested);
        EXPECT_LE(status.wakeup_count, 2U) << "wakeup should not be re-issued per caller";
    }
}

// ===========================================================================
// CR-105：WorkerHandle 在 facade 析构后退化为无操作
// ===========================================================================

TEST(ReliabilityCr105, HandleOutlivingFacadeDegradesToNoOps) {
    // 退化 status 的契约：is_running=false、计数归零、stop_reason=None，
    // name 保留句柄自身的名字（不是空串）——只要不崩溃、不触达已析构
    // 的 manager（ASAN 兜底），即满足 CR-105。
    std::optional<WorkerHandle> surviving_handle;
    std::string worker_name = "cr105_survivor";
    {
        Executor executor;
        ExecutorConfig config;
        config.min_threads = 2;
        config.max_threads = 2;
        ASSERT_TRUE(executor.initialize(config));

        auto ctl = std::make_shared<WakeupCtl>();
        auto handle = executor.start_worker(make_worker_spec(worker_name, ctl));
        ASSERT_TRUE(handle.started());

        // facade 存活期间：句柄操作真实生效（对照组）。
        const auto alive_status = handle.status();
        EXPECT_TRUE(alive_status.is_running);
        EXPECT_EQ(handle.status().name, worker_name);

        surviving_handle.emplace(std::move(handle));
        // facade 在 scope 末尾析构（manager 析构 → liveness 控制块失效）。
    }

    // facade 已析构：三个操作必须退化为无操作，绝不悬垂解引用。
    // （ASAN 下任何悬垂读都会在此暴露。）
    EXPECT_NO_FATAL_FAILURE(surviving_handle->request_stop());
    EXPECT_NO_FATAL_FAILURE(surviving_handle->stop());
    BlockingIoExecutorStatus status;
    EXPECT_NO_FATAL_FAILURE(status = surviving_handle->status());
    EXPECT_FALSE(status.is_running);
    EXPECT_EQ(status.name, worker_name)
        << "degraded status keeps the handle's own name by contract";
    EXPECT_EQ(status.stop_reason, BlockingIoStopReason::None);
    EXPECT_EQ(status.wakeup_count, 0U);
}

TEST(ReliabilityCr105, HandleWhileFacadeAliveStillWorks) {
    // 对照组：facade 存活时句柄仍然全功能（liveness 令牌未失效）。
    Executor executor;
    ExecutorConfig config;
    config.min_threads = 1;
    config.max_threads = 1;
    ASSERT_TRUE(executor.initialize(config));

    auto ctl = std::make_shared<WakeupCtl>();
    auto handle = executor.start_worker(make_worker_spec("cr105_alive", ctl));
    ASSERT_TRUE(handle.started());

    const auto status = handle.status();
    EXPECT_EQ(status.name, "cr105_alive");
    EXPECT_TRUE(status.is_running);

    handle.stop();
    const auto after = handle.status();
    EXPECT_FALSE(after.is_running);
    EXPECT_EQ(after.stop_reason, BlockingIoStopReason::Requested);
}

TEST(ReliabilityCr105, DefaultHandleOperationsAreSafeNoOps) {
    // 默认构造句柄（manager_ == nullptr）：三操作同样安全。
    WorkerHandle handle;
    EXPECT_NO_FATAL_FAILURE(handle.request_stop());
    EXPECT_NO_FATAL_FAILURE(handle.stop());
    const auto status = handle.status();
    EXPECT_FALSE(status.is_running);
}

// ===========================================================================
// CR-113/114：ThreadPool resize 事务化 + worker 线程内缩容拒绝
// ===========================================================================

namespace {

// resize 受 [min_threads, max_threads] 约束（越界直接 false，与本次
// 改动无关）——显式给每档测试留出扩缩空间，initialize 以 min_threads
// 起步。
ThreadPoolConfig pool_config(size_t min_threads, size_t max_threads) {
    ThreadPoolConfig config;
    config.min_threads = min_threads;
    config.max_threads = max_threads;
    config.queue_capacity = 256;
    return config;
}

} // namespace

TEST(ReliabilityCr113, ResizeRollbackOnInjectedThreadCreateFailure) {
    ThreadPool pool;
    ASSERT_TRUE(pool.initialize(pool_config(2, 8)));
    ASSERT_EQ(pool.get_status().total_threads, 2U);

    // 注入：新线程 id >= 2 的创建（钩子在 std::thread 构造前调用）抛出。
    pool.set_worker_thread_start_hook_for_test([](size_t worker_id) {
        if (worker_id >= 2) {
            throw std::system_error(
                std::make_error_code(std::errc::resource_unavailable_try_again),
                "injected resize thread creation failure");
        }
    });

    EXPECT_FALSE(pool.resize(4))
        << "resize must fail wholesale when any new thread creation fails";
    // 事务性：workers/ids 无半更新——线程数保持 2，提交路径仍工作。
    EXPECT_EQ(pool.get_status().total_threads, 2U)
        << "failed resize left a partially published worker set";

    // 失败后池仍完全可用（后续 resize/submit/shutdown 正常）。
    auto result = pool.submit([] { return 41 + 1; });
    ASSERT_TRUE(run_with_timeout([&] { EXPECT_EQ(result.get(), 42); }, 5s))
        << "submit broken after failed resize";

    pool.set_worker_thread_start_hook_for_test(nullptr);
    EXPECT_TRUE(pool.resize(4));
    EXPECT_EQ(pool.get_status().total_threads, 4U);
    auto result2 = pool.submit([] { return 7 * 6; });
    ASSERT_TRUE(run_with_timeout([&] { EXPECT_EQ(result2.get(), 42); }, 5s));
    EXPECT_TRUE(pool.resize(3));
    EXPECT_EQ(pool.get_status().total_threads, 3U);

    ASSERT_TRUE(run_with_timeout([&] { pool.shutdown(); }, 10s));
}

// 【验证代理实测：实现缺陷，暂以 DISABLED 保留复现】
// 中途失败变体（2 个新线程中的第二个失败）：CR-113 的回滚 catch 块在
// grow 分支的 std::lock_guard lock(mutex_) 作用域内 join 未发布线程，
// 而 worker_thread 的启动门（thread_pool.cpp:190-194）先取 mutex_ 等
// initialized_——回滚 join 一个正卡在该门上的线程 = 自死锁。凡失败前
// 已建成 ≥1 个线程即必现。独立探针：resize(4) 注入 id=3 失败 →
// "[hook] throwing for id=3" 后永久挂起（timeout exit=124）。
// 注：RollbackOnInjectedThreadCreateFailure（首线程即失败）未触发此
// 路径仅因 new_threads 为空、join 循环为空。等主循环修复（join 移出
// mutex_ 作用域）后去掉 DISABLED_ 前缀复验。
TEST(ReliabilityCr113, ResizeRollbackOnSecondThreadCreateFailure) {
    // 中途失败变体：2 个新线程中的第二个失败（首个已建成的未发布线程
    // 必须被回滚 join，且不影响既有 worker）。
    ThreadPool pool;
    ASSERT_TRUE(pool.initialize(pool_config(2, 8)));

    pool.set_worker_thread_start_hook_for_test([](size_t worker_id) {
        if (worker_id == 3) {
            throw std::runtime_error("injected failure on the second new thread");
        }
    });

    EXPECT_FALSE(pool.resize(4));
    EXPECT_EQ(pool.get_status().total_threads, 2U);

    pool.set_worker_thread_start_hook_for_test(nullptr);
    EXPECT_TRUE(pool.resize(4));
    EXPECT_EQ(pool.get_status().total_threads, 4U);

    ASSERT_TRUE(run_with_timeout([&] { pool.shutdown(); }, 10s));
}

TEST(ReliabilityCr114, ShrinkResizeFromInsideWorkerTaskIsRejected) {
    ThreadPool pool;
    // min=2 留出缩容空间；initialize 以 min 起步，先主线程扩到 4。
    ASSERT_TRUE(pool.initialize(pool_config(2, 8)));
    ASSERT_TRUE(pool.resize(4));
    ASSERT_EQ(pool.get_status().total_threads, 4U);

    std::promise<bool> resize_result;
    auto resize_future = resize_result.get_future();
    std::atomic<bool> task_running{false};

    auto future = pool.submit([&] {
        task_running.store(true);
        resize_result.set_value(pool.resize(2));
    });

    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!task_running.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(2ms);
    }
    ASSERT_TRUE(task_running.load());
    // 任务内缩容必须显式拒绝（旧实现此处 join 自身 → system_error）。
    ASSERT_TRUE(run_with_timeout([&] { EXPECT_FALSE(resize_future.get()); }, 5s))
        << "in-worker shrink resize hung (self-join?)";

    // 拒绝是无副作用的：池状态不变且继续可用。
    EXPECT_EQ(pool.get_status().total_threads, 4U);
    auto probe = pool.submit([] { return 100; });
    ASSERT_TRUE(run_with_timeout([&] { EXPECT_EQ(probe.get(), 100); }, 5s));

    // 主线程缩容仍正常（对照组）。
    EXPECT_TRUE(pool.resize(2));
    EXPECT_EQ(pool.get_status().total_threads, 2U);

    ASSERT_TRUE(run_with_timeout([&] { pool.shutdown(); }, 10s));
}

TEST(ReliabilityCr114, GrowResizeFromInsideWorkerTaskStillWorks) {
    // CR-114 契约的另一半：worker 线程内的扩容不 join 自身，维持可用。
    ThreadPool pool;
    ASSERT_TRUE(pool.initialize(pool_config(2, 8)));

    std::promise<bool> resize_result;
    auto resize_future = resize_result.get_future();
    auto future = pool.submit([&] { resize_result.set_value(pool.resize(4)); });

    ASSERT_TRUE(run_with_timeout([&] { EXPECT_TRUE(resize_future.get()); }, 5s))
        << "in-worker grow resize must not be rejected";
    EXPECT_EQ(pool.get_status().total_threads, 4U);

    ASSERT_TRUE(run_with_timeout([&] { pool.shutdown(); }, 10s));
}
