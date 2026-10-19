// 0.7.2 §4.2 GPU stream 完成计时反馈契约测试（无硬件，独立验证代理编写，
// 不修改库代码）。
//
// 用假 IGpuExecutor（经既有 repo 模式 #define private public 访问
// Executor::manager_ 注册）+ 可控 wants_feedback 的脚本调度器，验证
// Executor::submit_auto(CpuGpuTask) 的计时路由与反馈组装契约：
//  B1. supports==true + wants_feedback==true：走 submit_kernel_with_timing，
//      恰好一条 SchedulingFeedback；backend/executor_name/task_id/qos/
//      queue_wait>=0/execution_duration==completion-start（构造已知差值）/
//      success 与 failure_kind 映射正确。
//  B2. completion_ns==0（完成时点未知）→ execution_duration_ns==0；
//      success==false → failure_kind==GpuFailure 且 duration 仍 0。
//  B3. wants_feedback==false（脚本 feedback=false，等价 DefaultScheduler
//      的反馈开关）→ 不进计时路径（submit_kernel_with_timing 未被调用），
//      走既有非计时提交，零反馈。
//  B4. supports==false + wants_feedback==true → 回落 host 侧包装
//      （submit_kernel_with_timing 未被调用、submit_kernel_impl 被调用，
//      反馈仍产生——host 侧语义，duration 为真实执行时长）。
//  附加：deadline 字段映射（had_deadline / deadline_missed 取自
//  record.start_ns 与声明 deadline 的比较）。
//
// CUDA 实现（CudaExecutor）自身的失败交付路径见
// test_cuda_timing_failure_paths.cpp（仅 KAIRO_ENABLE_CUDA 构建编译）。

#include <gtest/gtest.h>

#include <kairo/types.hpp>
#include <kairo/interfaces.hpp>
#include <kairo/task_options.hpp>
#include <kairo/task_router.hpp>
#include <kairo/scheduler.hpp>
#include <kairo/executor_manager.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "test_gpu_timing_feedback_support.h"

#include <kairo/executor.hpp>

using namespace kairo;

namespace {

int64_t steady_now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// 假 GPU 执行器：submit_kernel_with_timing 只保存 (kernel, config, sink)
// 并返回未完成 future；测试线程随后用 run_pending_kernel() 模拟 kernel
// 发起、deliver_timing() 模拟经转交通道抵达的完成记录。submit_kernel_impl
// （submit_kernel 的底层）照既有 Mock 模式内联执行并落终态。
class FakeGpuTimingExecutor final : public IGpuExecutor {
public:
    FakeGpuTimingExecutor(std::string name, bool supports_timing)
        : name_(std::move(name)), supports_timing_(supports_timing) {}

    // ---- 0.7.2 §4.2 计时能力 ----
    bool supports_completion_timing() const noexcept override {
        return supports_timing_;
    }

    std::future<void> submit_kernel_with_timing(
        std::function<void(void*)> kernel_func,
        const gpu::GpuTaskConfig& config,
        KernelTimingSink timing_sink) override {
        timing_submit_calls_.fetch_add(1, std::memory_order_relaxed);
        pending_.kernel = std::move(kernel_func);
        pending_.config = config;
        pending_.sink = std::move(timing_sink);
        has_pending_.store(true, std::memory_order_release);
        pending_promise_ = std::make_shared<std::promise<void>>();
        return pending_promise_->get_future();
    }

    // ---- 受控交付（测试线程调用，模拟异步完成语义） ----
    bool has_pending() const { return has_pending_.load(std::memory_order_acquire); }

    const gpu::GpuTaskConfig& pending_config() const { return pending_.config; }

    void run_pending_kernel() {
        if (pending_.kernel) {
            kernel_runs_.fetch_add(1, std::memory_order_relaxed);
            pending_.kernel(nullptr);
        }
        if (pending_promise_) {
            pending_promise_->set_value();
        }
    }

    void deliver_timing(const gpu::GpuKernelTimingRecord& record) {
        sink_deliveries_.fetch_add(1, std::memory_order_relaxed);
        if (pending_.sink) {
            pending_.sink(record);
        }
    }

    // ---- 非计时提交路径（submit_kernel 的底层） ----
    std::future<void> submit_kernel_impl(
        std::function<void(void*)> kernel_func,
        const gpu::GpuTaskConfig& config) override {
        (void)config;
        impl_submit_calls_.fetch_add(1, std::memory_order_relaxed);
        std::promise<void> promise;
        auto future = promise.get_future();
        if (kernel_func) {
            kernel_func(nullptr);
            kernel_runs_.fetch_add(1, std::memory_order_relaxed);
        }
        promise.set_value();
        return future;
    }

    // ---- 观测 ----
    int timing_submit_calls() const {
        return timing_submit_calls_.load(std::memory_order_relaxed);
    }
    int impl_submit_calls() const {
        return impl_submit_calls_.load(std::memory_order_relaxed);
    }
    int sink_deliveries() const {
        return sink_deliveries_.load(std::memory_order_relaxed);
    }
    int kernel_runs() const { return kernel_runs_.load(std::memory_order_relaxed); }

    // ---- IGpuExecutor 其余纯虚接口（最小桩） ----
    std::string get_name() const override { return name_; }

    gpu::GpuDeviceInfo get_device_info() const override {
        gpu::GpuDeviceInfo info;
        info.name = name_ + "_device";
        info.backend = gpu::GpuBackend::CUDA;
        info.device_id = 0;
        info.total_memory_bytes = 1024 * 1024 * 1024;
        info.free_memory_bytes = 512 * 1024 * 1024;
        return info;
    }

    gpu::GpuExecutorStatus get_status() const override {
        gpu::GpuExecutorStatus status;
        status.name = name_;
        status.is_running = true;
        status.backend = gpu::GpuBackend::CUDA;
        status.device_id = 0;
        status.queue_capacity = 64;
        return status;
    }

    bool start() override { return true; }
    void stop() override {}
    void wait_for_completion() override {}
    void* allocate_device_memory(size_t) override { return nullptr; }
    void free_device_memory(void*) override {}
    bool copy_to_device(void*, const void*, size_t, bool, int) override {
        return true;
    }
    bool copy_to_host(void*, const void*, size_t, bool, int) override {
        return true;
    }
    bool copy_device_to_device(void*, const void*, size_t, bool, int) override {
        return true;
    }
    void synchronize() override {}
    void synchronize_stream(int) override {}
    int create_stream() override { return 1; }
    void destroy_stream(int) override {}
    bool add_stream_callback(int, std::function<void()>) override {
        return true;
    }

private:
    struct Pending {
        std::function<void(void*)> kernel;
        gpu::GpuTaskConfig config;
        KernelTimingSink sink;
    };

    std::string name_;
    bool supports_timing_ = false;
    Pending pending_;
    std::atomic<bool> has_pending_{false};
    std::shared_ptr<std::promise<void>> pending_promise_;

    std::atomic<int> timing_submit_calls_{0};
    std::atomic<int> impl_submit_calls_{0};
    std::atomic<int> sink_deliveries_{0};
    std::atomic<int> kernel_runs_{0};
};

// 脚本调度器：固定路由到指定 GPU 执行器（Accepted / GpuHeuristic），
// wants_feedback 与反馈记录可控（参照 tests/test_adaptive_scheduler.cpp
// 的 ScriptedScheduler 与 tests/test_scheduling_contract_v061.cpp 的
// FeedbackScheduler 组合）。
class ScriptedGpuScheduler final : public IScheduler {
public:
    ScriptedGpuScheduler(std::string executor_name, bool feedback)
        : executor_name_(std::move(executor_name)), feedback_(feedback) {}

    RoutingDecision route(const TaskRouter::Request& request,
                          const std::vector<ExecutorCapability>&) override {
        route_calls_.fetch_add(1, std::memory_order_relaxed);
        RoutingDecision decision;
        decision.task_name = request.options.name;
        decision.requested_intent = request.options.intent;
        decision.reason = RoutingReason::GpuHeuristic;
        decision.status = RoutingStatus::Accepted;
        decision.selected_backend = ExecutionBackend::Gpu;
        decision.selected_executor_name = executor_name_;
        decision.detail = "scripted-gpu-timing";
        return decision;
    }

    bool wants_feedback() const noexcept override { return feedback_; }

    void on_task_completed(const SchedulingFeedback& feedback) override {
        std::lock_guard<std::mutex> lock(mutex_);
        feedbacks_.push_back(feedback);
    }

    std::vector<SchedulingFeedback> snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return feedbacks_;
    }

    int route_calls() const { return route_calls_.load(std::memory_order_relaxed); }

private:
    std::string executor_name_;
    bool feedback_;
    std::atomic<int> route_calls_{0};
    mutable std::mutex mutex_;
    std::vector<SchedulingFeedback> feedbacks_;
};

constexpr const char* kFakeGpuName = "timing_probe_gpu";

}  // namespace

class GpuTimingFeedbackTest : public ::testing::Test {
protected:
    void SetUp() override {
        ExecutorConfig config;
        config.min_threads = 2;
        config.max_threads = 2;
        ASSERT_TRUE(executor_.initialize(config));
    }

    void RegisterFake(bool supports_timing) {
        auto fake = std::make_unique<FakeGpuTimingExecutor>(kFakeGpuName,
                                                            supports_timing);
        fake_ = fake.get();
        // 注册经 support TU 的注入辅助（MSVC 修饰名兼容，见
        // test_gpu_timing_feedback_support.h 注释）。
        gpu_timing_test::register_fake_gpu(executor_, kFakeGpuName,
                                           std::move(fake));
    }

    void SetScheduler(bool feedback) {
        auto scheduler =
            std::make_unique<ScriptedGpuScheduler>(kFakeGpuName, feedback);
        scheduler_ = scheduler.get();
        executor_.set_scheduler(std::move(scheduler));
    }

    Executor executor_;
    FakeGpuTimingExecutor* fake_ = nullptr;
    ScriptedGpuScheduler* scheduler_ = nullptr;
};

// ---------------------------------------------------------------------------
// B1. 计时提交 → 恰好一条 GPU 反馈
// ---------------------------------------------------------------------------

TEST_F(GpuTimingFeedbackTest, TimingSubmitProducesExactlyOneGpuFeedback) {
    RegisterFake(/*supports_timing=*/true);
    SetScheduler(/*feedback=*/true);

    std::atomic<bool> gpu_ran{false};
    auto future = executor_.submit_auto(
        cpu_gpu_task([] {}, [&gpu_ran](void*) { gpu_ran.store(true); })
            .name("gpu-t1")
            .qos(QosClass::Interactive));

    // 路由进计时路径：submit_kernel_with_timing 恰被调用一次，且提交
    // 侧（submit_kernel_impl）未被调用。
    ASSERT_TRUE(fake_->has_pending());
    EXPECT_EQ(fake_->timing_submit_calls(), 1);
    EXPECT_EQ(fake_->impl_submit_calls(), 0);
    EXPECT_EQ(fake_->pending_config().stream_id, 0);

    fake_->run_pending_kernel();  // 模拟 kernel 发起（launch 成功）
    EXPECT_NO_THROW(future.get());
    EXPECT_TRUE(gpu_ran.load());

    // 模拟 stream 完成回调经转交通道后的 sink 交付：completion 取当前
    // 时刻，start = completion - 1050ns（已知差值）。先睡 200us 保证
    // "completion - facade 提交时点" 远大于 1050ns，使 queue_wait >= 0
    // 成为确定性断言（不依赖精确时延）。
    std::this_thread::sleep_for(std::chrono::microseconds(200));
    const int64_t completion_ns = steady_now_ns();
    gpu::GpuKernelTimingRecord record;
    record.submit_ns = completion_ns - 1150;
    record.start_ns = completion_ns - 1050;
    record.completion_ns = completion_ns;
    record.stream_id = 0;
    record.success = true;
    record.completed_on_stream = true;
    fake_->deliver_timing(record);

    const auto feedbacks = scheduler_->snapshot();
    ASSERT_EQ(feedbacks.size(), 1u)
        << "exactly one SchedulingFeedback per timed submission";
    const auto& fb = feedbacks[0];
    EXPECT_EQ(fb.task_id, "gpu-t1");
    EXPECT_EQ(fb.qos, QosClass::Interactive);
    EXPECT_EQ(fb.backend, ExecutionBackend::Gpu);
    EXPECT_EQ(fb.executor_name, kFakeGpuName);
    EXPECT_TRUE(fb.success);
    EXPECT_EQ(fb.failure_kind, FailureKind::None);
    EXPECT_EQ(fb.execution_duration_ns, 1050)
        << "execution_duration must be completion_ns - start_ns";
    EXPECT_GE(fb.queue_wait_ns, 0)
        << "record start_ns is sampled after the facade submit timestamp";
    EXPECT_FALSE(fb.had_deadline);
    EXPECT_FALSE(fb.deadline_missed);

    EXPECT_EQ(executor_.get_scheduling_metrics().feedback_reported_count, 1u);
}

// ---------------------------------------------------------------------------
// B2. 未知完成时点与失败映射
// ---------------------------------------------------------------------------

TEST_F(GpuTimingFeedbackTest, UnknownCompletionAndFailureMapping) {
    RegisterFake(true);
    SetScheduler(true);

    // 提交 1：kernel 成功但 completion_ns == 0（完成时点不可得，如转交
    // 通道溢出/执行器停止）→ duration 按 0 = 未知，仍算 success。
    auto f1 = executor_.submit_auto(
        cpu_gpu_task([] {}, [](void*) {}).name("gpu-unknown-completion"));
    ASSERT_TRUE(fake_->has_pending());
    fake_->run_pending_kernel();
    EXPECT_NO_THROW(f1.get());

    const int64_t now1 = steady_now_ns();
    gpu::GpuKernelTimingRecord unknown;
    unknown.submit_ns = now1 - 1000;
    unknown.start_ns = now1 - 900;
    unknown.completion_ns = 0;  // 未知
    unknown.success = true;
    unknown.completed_on_stream = false;
    fake_->deliver_timing(unknown);

    // 提交 2：success == false（launch 失败类）→ GpuFailure；即便带
    // 非 0 completion，duration 仍为 0（未知口径，不把失败当"快"）。
    auto f2 = executor_.submit_auto(
        cpu_gpu_task([] {}, [](void*) {}).name("gpu-failed"));
    ASSERT_TRUE(fake_->has_pending());
    fake_->run_pending_kernel();
    EXPECT_NO_THROW(f2.get());

    const int64_t now2 = steady_now_ns();
    gpu::GpuKernelTimingRecord failed;
    failed.submit_ns = now2 - 1000;
    failed.start_ns = now2 - 900;
    failed.completion_ns = now2;
    failed.success = false;
    failed.completed_on_stream = false;
    fake_->deliver_timing(failed);

    const auto feedbacks = scheduler_->snapshot();
    ASSERT_EQ(feedbacks.size(), 2u);
    EXPECT_EQ(feedbacks[0].task_id, "gpu-unknown-completion");
    EXPECT_TRUE(feedbacks[0].success);
    EXPECT_EQ(feedbacks[0].failure_kind, FailureKind::None);
    EXPECT_EQ(feedbacks[0].execution_duration_ns, 0)
        << "completion_ns == 0 must map to duration 0 (unknown), not 'fast'";

    EXPECT_EQ(feedbacks[1].task_id, "gpu-failed");
    EXPECT_FALSE(feedbacks[1].success);
    EXPECT_EQ(feedbacks[1].failure_kind, FailureKind::GpuFailure);
    EXPECT_EQ(feedbacks[1].execution_duration_ns, 0)
        << "failed launches must report unknown duration";
    EXPECT_EQ(feedbacks[1].backend, ExecutionBackend::Gpu);
    EXPECT_EQ(feedbacks[1].executor_name, kFakeGpuName);
}

// ---------------------------------------------------------------------------
// 附加：deadline 字段映射
// ---------------------------------------------------------------------------

TEST_F(GpuTimingFeedbackTest, DeadlineFieldsMapFromRecordStart) {
    RegisterFake(true);
    SetScheduler(true);

    // 提交 1：已过期的 deadline → had_deadline && deadline_missed
    //（record.start_ns 晚于过期 deadline，确定性成立）。
    auto f1 = executor_.submit_auto(
        cpu_gpu_task([] {}, [](void*) {})
            .name("gpu-late")
            .deadline(std::chrono::steady_clock::now() -
                      std::chrono::milliseconds(1)));
    ASSERT_TRUE(fake_->has_pending());
    fake_->run_pending_kernel();
    EXPECT_NO_THROW(f1.get());
    std::this_thread::sleep_for(std::chrono::microseconds(200));
    const int64_t completion1 = steady_now_ns();
    gpu::GpuKernelTimingRecord late;
    late.submit_ns = completion1 - 1150;
    late.start_ns = completion1 - 1050;
    late.completion_ns = completion1;
    late.success = true;
    fake_->deliver_timing(late);

    // 提交 2：远期 deadline → had_deadline && !deadline_missed
    auto f2 = executor_.submit_auto(
        cpu_gpu_task([] {}, [](void*) {})
            .name("gpu-in-time")
            .deadline(std::chrono::steady_clock::now() +
                      std::chrono::hours(1)));
    ASSERT_TRUE(fake_->has_pending());
    fake_->run_pending_kernel();
    EXPECT_NO_THROW(f2.get());
    const int64_t completion2 = steady_now_ns();
    gpu::GpuKernelTimingRecord in_time;
    in_time.submit_ns = completion2 - 1150;
    in_time.start_ns = completion2 - 1050;
    in_time.completion_ns = completion2;
    in_time.success = true;
    fake_->deliver_timing(in_time);

    const auto feedbacks = scheduler_->snapshot();
    ASSERT_EQ(feedbacks.size(), 2u);
    EXPECT_EQ(feedbacks[0].task_id, "gpu-late");
    EXPECT_TRUE(feedbacks[0].had_deadline);
    EXPECT_TRUE(feedbacks[0].deadline_missed)
        << "start_ns after an already-expired deadline must be a miss";
    EXPECT_EQ(feedbacks[1].task_id, "gpu-in-time");
    EXPECT_TRUE(feedbacks[1].had_deadline);
    EXPECT_FALSE(feedbacks[1].deadline_missed);
}

// ---------------------------------------------------------------------------
// B3. wants_feedback == false：零开销基线（不进计时路径）
// ---------------------------------------------------------------------------

TEST_F(GpuTimingFeedbackTest, WantsFeedbackFalseSkipsTimingPath) {
    RegisterFake(/*supports_timing=*/true);
    // feedback=false 的脚本调度器与 DefaultScheduler 的反馈开关一致，
    // 但固定 GPU 路由以隔离"wants_feedback 开关"这单一变量。
    SetScheduler(/*feedback=*/false);

    std::atomic<bool> gpu_ran{false};
    auto future = executor_.submit_auto(
        cpu_gpu_task([] {}, [&gpu_ran](void*) { gpu_ran.store(true); })
            .name("gpu-no-feedback"));

    // 不进计时路径：走既有 submit_kernel（host 侧无反馈包装）。
    EXPECT_EQ(fake_->timing_submit_calls(), 0)
        << "feedback-disabled submits must not use submit_kernel_with_timing";
    EXPECT_EQ(fake_->impl_submit_calls(), 1);
    EXPECT_NO_THROW(future.get());
    EXPECT_TRUE(gpu_ran.load());
    EXPECT_EQ(fake_->kernel_runs(), 1);
    EXPECT_EQ(fake_->sink_deliveries(), 0);
    EXPECT_EQ(executor_.get_scheduling_metrics().feedback_reported_count, 0u);
}

// ---------------------------------------------------------------------------
// B4. 执行器不支持计时：回落 host 侧包装，反馈仍产生
// ---------------------------------------------------------------------------

TEST_F(GpuTimingFeedbackTest, UnsupportedExecutorFallsBackToHostWrapper) {
    RegisterFake(/*supports_timing=*/false);
    SetScheduler(/*feedback=*/true);

    auto future = executor_.submit_auto(
        cpu_gpu_task(
            [] {},
            [](void*) { std::this_thread::sleep_for(std::chrono::microseconds(50)); })
            .name("gpu-fallback"));

    // 回落路径：submit_kernel_with_timing 未被调用；submit_kernel_impl
    // 被调用；kernel 在 impl 内联执行。
    EXPECT_EQ(fake_->timing_submit_calls(), 0)
        << "non-timing executors must not receive submit_kernel_with_timing";
    EXPECT_EQ(fake_->impl_submit_calls(), 1);
    EXPECT_NO_THROW(future.get());
    EXPECT_EQ(fake_->kernel_runs(), 1);

    // host 侧反馈仍然产生（0.6.1 语义）：backend/executor_name 一致，
    // duration 为真实执行时长（>= 50us 睡眠下界），success。
    const auto feedbacks = scheduler_->snapshot();
    ASSERT_EQ(feedbacks.size(), 1u);
    const auto& fb = feedbacks[0];
    EXPECT_EQ(fb.task_id, "gpu-fallback");
    EXPECT_EQ(fb.backend, ExecutionBackend::Gpu);
    EXPECT_EQ(fb.executor_name, kFakeGpuName);
    EXPECT_TRUE(fb.success);
    EXPECT_EQ(fb.failure_kind, FailureKind::None);
    EXPECT_GE(fb.execution_duration_ns, 50'000)
        << "host-side duration measures the real kernel run (>= sleep floor)";
    EXPECT_GE(fb.queue_wait_ns, 0);
}
