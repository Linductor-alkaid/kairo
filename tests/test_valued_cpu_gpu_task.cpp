// 0.7.3 §5.1 取值 CpuGpuTask（ResultAdapter<R> + ExecutionReport<R>）契约测试
// （独立验证代理编写，不修改库代码）。
//
// 无硬件环境：假 IGpuExecutor（复用 test_gpu_timing_feedback 的
// #define private public 注入辅助 TU 注册进 manager_）+ 固定路由到假 GPU
// 的脚本调度器。契约矩阵（docs/design/scheduling_runtime.md §17）：
//  C1. CPU 路径：值返回 / double→int 转换 / 异常保真 / decision 反映
//      AllowCpu 降级（backend == DefaultAsync，fell_back，FallbackPolicy）。
//  C2. void 两参拼写回归：cpu_gpu_task(cpu, gpu) 与 cpu_gpu_task<void>(...)
//      的 submit_auto 返回 std::future<void>（编译期 is_same_v 断言），
//      void GPU 提交经假执行器行为不变。
//  C3. GPU 路径（R 非 void）：
//      (i)   有 adapter：kernel 包装 settle 值路径（gpu callable 安排宿主
//            状态，gpu_extract 返回它）；
//      (ii)  gpu_extract 抛异常 → result 重抛原异常，且计时 sink 的失败
//            兜底结算为输家（不覆盖主结算）；
//      (iii) R 非 void 无 adapter + NoFallback → 提交即结构化拒绝
//            （get() 抛 runtime_error，decision.status == Rejected，假执行器
//            未收到任何提交）；
//      (iv)  同场景 AllowCpu → 提交前回退 CPU（reason == FallbackPolicy，
//            fell_back == true，AcceptedDegraded）；
//      (v)   执行器不支持完成计时 + NoFallback → 拒绝；
//      (vi)  提交侧同步失败（假执行器返回已就绪异常 future）→ result 即时
//            以异常就绪（不等待、不依赖 sink 转交）；
//      (vii) kernel 未运行 + 计时 sink 交付 success == false → result 经
//            兜底通道以 runtime_error 就绪（completion_ns == 0 → "work not
//            completed"），feedback 同构产出。
//  C4. 带 R 形态的 builder 链（prefer_gpu / characteristics / name / qos /
//      priority / gpu_config）编译可用且行为正确。

#include <gtest/gtest.h>

#include <kairo/types.hpp>
#include <kairo/interfaces.hpp>
#include <kairo/task_options.hpp>
#include <kairo/task_result.hpp>
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
#include <type_traits>
#include <utility>
#include <vector>

#include "test_gpu_timing_feedback_support.h"

#include <kairo/executor.hpp>

using namespace kairo;

namespace {

struct ValuedCustomError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// 假 GPU 执行器（取值路径扩展）：submit_kernel_with_timing 只保存
// (kernel, config, sink) 并返回未完成 future；测试线程经 run_pending_kernel()
// 模拟 kernel 发起、deliver_timing() 模拟完成记录抵达。fail_next_submit
// 置位时返回"已就绪且带异常"的 future，模拟提交侧同步失败（未运行）。
class FakeValuedGpuExecutor final : public IGpuExecutor {
public:
    FakeValuedGpuExecutor(std::string name, bool supports_timing)
        : name_(std::move(name)), supports_timing_(supports_timing) {}

    bool supports_completion_timing() const noexcept override {
        return supports_timing_;
    }

    std::future<void> submit_kernel_with_timing(
        std::function<void(void*)> kernel_func,
        const gpu::GpuTaskConfig& config,
        KernelTimingSink timing_sink) override {
        timing_submit_calls_.fetch_add(1, std::memory_order_relaxed);
        if (fail_next_submit_.exchange(false, std::memory_order_acq_rel)) {
            std::promise<void> failed;
            auto future = failed.get_future();
            failed.set_exception(std::make_exception_ptr(
                std::runtime_error("fake gpu executor not running")));
            return future;
        }
        pending_.kernel = std::move(kernel_func);
        pending_.config = config;
        pending_.sink = std::move(timing_sink);
        has_pending_.store(true, std::memory_order_release);
        pending_promise_ = std::make_shared<std::promise<void>>();
        return pending_promise_->get_future();
    }

    // ---- 非计时提交路径（submit_kernel 的底层；最小桩） ----
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

    // ---- 受控交付 ----
    bool has_pending() const {
        return has_pending_.load(std::memory_order_acquire);
    }

    const gpu::GpuTaskConfig& pending_config() const { return pending_.config; }

    void run_pending_kernel() {
        if (pending_.kernel) {
            kernel_runs_.fetch_add(1, std::memory_order_relaxed);
            try {
                pending_.kernel(nullptr);
            } catch (...) {
                if (pending_promise_) {
                    pending_promise_->set_exception(std::current_exception());
                }
                return;
            }
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

    void fail_next_submit() { fail_next_submit_.store(true); }

    int timing_submit_calls() const {
        return timing_submit_calls_.load(std::memory_order_relaxed);
    }
    int impl_submit_calls() const {
        return impl_submit_calls_.load(std::memory_order_relaxed);
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
    std::atomic<bool> fail_next_submit_{false};
    std::shared_ptr<std::promise<void>> pending_promise_;

    std::atomic<int> timing_submit_calls_{0};
    std::atomic<int> impl_submit_calls_{0};
    std::atomic<int> sink_deliveries_{0};
    std::atomic<int> kernel_runs_{0};
};

// 固定路由到指定 GPU 执行器的脚本调度器（忽略 gpu_selected 启发式二次
// 咨询，使"选中 GPU"成为确定前提；与 test_gpu_timing_feedback 同模式）。
class ScriptedValuedScheduler final : public IScheduler {
public:
    explicit ScriptedValuedScheduler(std::string executor_name,
                                     bool feedback = false)
        : executor_name_(std::move(executor_name)), feedback_(feedback) {}

    RoutingDecision route(const TaskRouter::Request& request,
                          const std::vector<ExecutorCapability>&) override {
        RoutingDecision decision;
        decision.task_name = request.options.name;
        decision.requested_intent = request.options.intent;
        decision.reason = RoutingReason::GpuHeuristic;
        decision.status = RoutingStatus::Accepted;
        decision.selected_backend = ExecutionBackend::Gpu;
        decision.selected_executor_name = executor_name_;
        decision.detail = "scripted-valued-gpu";
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

private:
    std::string executor_name_;
    bool feedback_;
    mutable std::mutex mutex_;
    std::vector<SchedulingFeedback> feedbacks_;
};

constexpr const char* kValuedFakeGpuName = "valued_probe_gpu";

gpu::GpuKernelTimingRecord failed_record_zero_completion() {
    gpu::GpuKernelTimingRecord record;
    record.submit_ns = 1;
    record.start_ns = 2;
    record.completion_ns = 0;  // 完成时点未知（停止排空/转交丢失）
    record.success = false;
    record.completed_on_stream = false;
    return record;
}

}  // namespace

class ValuedCpuGpuTaskTest : public ::testing::Test {
protected:
    void SetUp() override {
        ExecutorConfig config;
        config.min_threads = 2;
        config.max_threads = 2;
        ASSERT_TRUE(executor_.initialize(config));
    }

    void TearDown() override { executor_.shutdown(); }

    void RegisterFake(bool supports_timing) {
        auto fake = std::make_unique<FakeValuedGpuExecutor>(
            kValuedFakeGpuName, supports_timing);
        fake_ = fake.get();
        gpu_timing_test::register_fake_gpu(executor_, kValuedFakeGpuName,
                                           std::move(fake));
    }

    void SetScheduler(bool feedback = false) {
        auto scheduler = std::make_unique<ScriptedValuedScheduler>(
            kValuedFakeGpuName, feedback);
        scheduler_ = scheduler.get();
        executor_.set_scheduler(std::move(scheduler));
    }

    Executor executor_;
    FakeValuedGpuExecutor* fake_ = nullptr;
    ScriptedValuedScheduler* scheduler_ = nullptr;
};

// ---------------------------------------------------------------------------
// C1. CPU 路径（值返回 / 转换 / 异常 / decision 降级语义）
// ---------------------------------------------------------------------------

TEST_F(ValuedCpuGpuTaskTest, CpuPathValueAndConversion) {
    auto report = executor_.submit_auto(
        cpu_gpu_task<int>([]() -> double { return 3.9; },
                          [](void*) { FAIL() << "GPU path must not run"; })
            .fallback(FallbackPolicy::AllowCpu)
            .name("valued-cpu-int"));

    static_assert(std::is_same_v<decltype(report), ExecutionReport<int>>,
                  "non-void submit_auto must return ExecutionReport<R>");
    EXPECT_EQ(report.result.get(), 3) << "double 3.9 converts to int 3";
    EXPECT_EQ(report.decision.selected_backend, ExecutionBackend::DefaultAsync);
    EXPECT_EQ(report.decision.status, RoutingStatus::AcceptedDegraded);
    EXPECT_EQ(report.decision.reason, RoutingReason::FallbackPolicy);
    EXPECT_TRUE(report.decision.fell_back);
}

TEST_F(ValuedCpuGpuTaskTest, CpuPathStringAndIdentityReturnType) {
    auto report = executor_.submit_auto(
        cpu_gpu_task<std::string>([] { return std::string("from-cpu"); },
                                  [](void*) {})
            .fallback(FallbackPolicy::AllowCpu)
            .name("valued-cpu-string"));
    static_assert(std::is_same_v<decltype(report), ExecutionReport<std::string>>,
                  "explicit R names the report type");
    EXPECT_EQ(report.result.get(), "from-cpu");
    EXPECT_EQ(report.decision.selected_executor_name, "default");
}

TEST_F(ValuedCpuGpuTaskTest, CpuPathExceptionFidelity) {
    auto report = executor_.submit_auto(
        cpu_gpu_task<int>([]() -> int { throw ValuedCustomError("cpu-boom"); },
                          [](void*) {})
            .fallback(FallbackPolicy::AllowCpu)
            .name("valued-cpu-throw"));
    try {
        (void)report.result.get();
        FAIL() << "expected ValuedCustomError";
    } catch (const ValuedCustomError& error) {
        EXPECT_STREQ(error.what(), "cpu-boom");
    } catch (...) {
        FAIL() << "original exception type was not preserved";
    }
    // 执行期异常不改变路由结论：decision 仍是降级接受。
    EXPECT_EQ(report.decision.status, RoutingStatus::AcceptedDegraded);
    EXPECT_TRUE(report.decision.fell_back);
}

// ---------------------------------------------------------------------------
// C2. void 两参拼写回归（编译期类型 + 行为）
// ---------------------------------------------------------------------------

TEST_F(ValuedCpuGpuTaskTest, VoidTwoArgSpellingReturnsStdFuture) {
    RegisterFake(/*supports_timing=*/true);
    // feedback 开启以走计时提交路径（与既有 void 计时契约一致，便于
    // 断言"GPU 路径被原样使用"）。
    SetScheduler(/*feedback=*/false);

    // 两参拼写：R 缺省为 void → submit_auto 仍返回 std::future<void>。
    // feedback 关闭 → 走既有非计时 submit_kernel 路径（0.7.2 行为逐位
    // 不变：impl 提交、内联执行、零反馈）。
    auto gpu_future = executor_.submit_auto(
        cpu_gpu_task([] {}, [](void*) {}).name("void-regression-gpu"));
    static_assert(
        std::is_same_v<decltype(gpu_future), std::future<void>>,
        "two-arg cpu_gpu_task must keep the 0.7.2 std::future<void> return");
    EXPECT_EQ(fake_->timing_submit_calls(), 0);
    ASSERT_EQ(fake_->impl_submit_calls(), 1)
        << "void two-arg spelling must keep the non-timing submit path";
    EXPECT_NO_THROW(gpu_future.get());
    EXPECT_EQ(fake_->kernel_runs(), 1);

    // 显式 cpu_gpu_task<void>(...) 拼写等价（feedback 开启 → 计时路径）。
    SetScheduler(/*feedback=*/true);
    auto timed_future = executor_.submit_auto(
        cpu_gpu_task<void>([] {}, [](void*) {}).name("void-regression-timing"));
    static_assert(std::is_same_v<decltype(timed_future), std::future<void>>,
                  "explicit void R must also yield std::future<void>");
    ASSERT_TRUE(fake_->has_pending());
    fake_->run_pending_kernel();
    EXPECT_NO_THROW(timed_future.get());
    EXPECT_EQ(fake_->timing_submit_calls(), 1);

    // void CPU 路径（无 GPU 注册场景的 AllowCpu 回退同样返回 future<void>）。
    Executor bare;
    ExecutorConfig config;
    config.min_threads = 1;
    config.max_threads = 1;
    ASSERT_TRUE(bare.initialize(config));
    std::atomic<bool> cpu_ran{false};
    auto cpu_future = bare.submit_auto(
        cpu_gpu_task([&cpu_ran] { cpu_ran.store(true); }, [](void*) {})
            .fallback(FallbackPolicy::AllowCpu)
            .name("void-regression-cpu"));
    static_assert(std::is_same_v<decltype(cpu_future), std::future<void>>,
                  "void CPU fallback must also yield std::future<void>");
    EXPECT_NO_THROW(cpu_future.get());
    EXPECT_TRUE(cpu_ran.load());
    bare.shutdown();
}

// ---------------------------------------------------------------------------
// C3(i). GPU 取值：kernel 包装 settle 值路径
// ---------------------------------------------------------------------------

TEST_F(ValuedCpuGpuTaskTest, GpuSettlesValueThroughKernelWrapper) {
    RegisterFake(/*supports_timing=*/true);
    SetScheduler(/*feedback=*/false);

    double host_slot = 0.0;
    std::atomic<bool> extract_ran{false};
    auto report = executor_.submit_auto(
        cpu_gpu_task<double>([]() -> double { return -1.0; },
                             [&host_slot](void*) { host_slot = 42.5; })
            .result_adapter(ResultAdapter<double>{
                [&host_slot, &extract_ran]() -> double {
                    extract_ran.store(true);
                    return host_slot;
                }})
            .name("valued-gpu-ok"));

    EXPECT_EQ(report.decision.selected_backend, ExecutionBackend::Gpu);
    EXPECT_EQ(report.decision.status, RoutingStatus::Accepted);
    EXPECT_FALSE(report.decision.fell_back);
    ASSERT_TRUE(fake_->has_pending());

    EXPECT_FALSE(report.result.is_ready())
        << "value must not settle before the kernel runs";
    fake_->run_pending_kernel();
    EXPECT_EQ(report.result.wait_for(std::chrono::seconds(2)),
              std::future_status::ready);
    EXPECT_DOUBLE_EQ(report.result.get(), 42.5);
    EXPECT_TRUE(extract_ran.load())
        << "gpu_extract must run inside the kernel wrapper";
}

// ---------------------------------------------------------------------------
// C3(ii). gpu_extract 抛异常 → 原异常保真，sink 兜底为输家
// ---------------------------------------------------------------------------

TEST_F(ValuedCpuGpuTaskTest, GpuExtractExceptionPreservedAndSinkLoses) {
    RegisterFake(/*supports_timing=*/true);
    SetScheduler(/*feedback=*/false);

    auto report = executor_.submit_auto(
        cpu_gpu_task<int>([]() -> int { return -1; },
                          [](void*) { /* kernel ok */ })
            .result_adapter(ResultAdapter<int>{
                []() -> int { throw ValuedCustomError("extract-boom"); }})
            .name("valued-gpu-extract-throw"));

    ASSERT_TRUE(fake_->has_pending());
    fake_->run_pending_kernel();  // kernel 成功、extract 抛出 → 主结算异常
    // 主结算之后 sink 收到失败记录：兜底结算必须是输家，不覆盖原异常。
    fake_->deliver_timing(failed_record_zero_completion());

    try {
        (void)report.result.get();
        FAIL() << "expected ValuedCustomError";
    } catch (const ValuedCustomError& error) {
        EXPECT_STREQ(error.what(), "extract-boom");
    } catch (...) {
        FAIL() << "extract exception was not preserved (sink may have "
                  "overwritten the primary settlement)";
    }
}

// ---------------------------------------------------------------------------
// C3(iii). R 非 void 无 adapter + NoFallback → 提交即结构化拒绝
// ---------------------------------------------------------------------------

TEST_F(ValuedCpuGpuTaskTest, NonVoidNoAdapterNoFallbackRejectedAtSubmit) {
    RegisterFake(/*supports_timing=*/true);
    SetScheduler(/*feedback=*/false);

    auto report = executor_.submit_auto(
        cpu_gpu_task<int>([]() -> int { return 1; }, [](void*) {})
            .name("valued-no-adapter"));

    EXPECT_EQ(report.decision.status, RoutingStatus::Rejected);
    EXPECT_EQ(report.decision.reason, RoutingReason::Rejected);
    EXPECT_NE(report.decision.detail.find("ResultAdapter"), std::string::npos)
        << "rejection detail must name the missing adapter: "
        << report.decision.detail;
    // 结构化拒绝：报告即终态——不抛出，get() 以 runtime_error 就绪。
    try {
        (void)report.result.get();
        FAIL() << "expected std::runtime_error";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("ResultAdapter"),
                  std::string::npos);
    }
    // 拒绝发生在投递之前：假执行器未收到任何提交。
    EXPECT_EQ(fake_->timing_submit_calls(), 0);
    EXPECT_EQ(fake_->impl_submit_calls(), 0);
}

// ---------------------------------------------------------------------------
// C3(iv). 同场景 AllowCpu → 提交前回退 CPU
// ---------------------------------------------------------------------------

TEST_F(ValuedCpuGpuTaskTest, NonVoidNoAdapterAllowCpuFallsBackToCpu) {
    RegisterFake(/*supports_timing=*/true);
    SetScheduler(/*feedback=*/false);

    auto report = executor_.submit_auto(
        cpu_gpu_task<int>([]() -> int { return 77; }, [](void*) {})
            .fallback(FallbackPolicy::AllowCpu)
            .name("valued-no-adapter-allowcpu"));

    EXPECT_EQ(report.decision.status, RoutingStatus::AcceptedDegraded);
    EXPECT_EQ(report.decision.reason, RoutingReason::FallbackPolicy);
    EXPECT_TRUE(report.decision.fell_back);
    EXPECT_EQ(report.decision.selected_backend, ExecutionBackend::DefaultAsync);
    EXPECT_EQ(report.result.get(), 77);
    EXPECT_EQ(fake_->timing_submit_calls(), 0)
        << "fallback happens before submission; GPU path never engaged";
}

// ---------------------------------------------------------------------------
// C3(v). 执行器不支持完成计时 + NoFallback → 拒绝
// ---------------------------------------------------------------------------

TEST_F(ValuedCpuGpuTaskTest, NonVoidTimingUnsupportedNoFallbackRejected) {
    RegisterFake(/*supports_timing=*/false);
    SetScheduler(/*feedback=*/false);

    auto report = executor_.submit_auto(
        cpu_gpu_task<double>([]() -> double { return 1.0; },
                             [](void*) {})
            .result_adapter(ResultAdapter<double>{[]() -> double { return 0.0; }})
            .name("valued-no-timing"));

    EXPECT_EQ(report.decision.status, RoutingStatus::Rejected);
    EXPECT_NE(report.decision.detail.find("timing-capable"), std::string::npos)
        << report.decision.detail;
    EXPECT_THROW(report.result.get(), std::runtime_error);
    EXPECT_EQ(fake_->timing_submit_calls(), 0);
    EXPECT_EQ(fake_->impl_submit_calls(), 0);
}

// ---------------------------------------------------------------------------
// C3(vi). 提交侧同步失败 → result 即时以异常就绪
// ---------------------------------------------------------------------------

TEST_F(ValuedCpuGpuTaskTest, SubmitSideFailureSettlesResultImmediately) {
    RegisterFake(/*supports_timing=*/true);
    SetScheduler(/*feedback=*/false);
    fake_->fail_next_submit();

    auto report = executor_.submit_auto(
        cpu_gpu_task<int>([]() -> int { return 5; },
                          [](void*) { FAIL() << "kernel must not run"; })
            .result_adapter(ResultAdapter<int>{[]() -> int { return 0; }})
            .name("valued-submit-fail"));

    // 不 run_pending_kernel：提交侧已同步失败，result 必须即时就绪，
    // 不等待 sink 的转交通道。
    EXPECT_EQ(report.result.wait_for(std::chrono::milliseconds(100)),
              std::future_status::ready)
        << "submit-side failure must settle the result synchronously";
    try {
        (void)report.result.get();
        FAIL() << "expected std::runtime_error";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("not running"),
                  std::string::npos)
            << "original submit-side error must be preserved: " << error.what();
    }
    // 路由本身已接受：失败是投递层，不回写 Rejected。
    EXPECT_EQ(report.decision.status, RoutingStatus::Accepted);
    EXPECT_EQ(fake_->kernel_runs(), 0);
}

// ---------------------------------------------------------------------------
// C3(vii). kernel 未运行 + sink 失败交付 → 兜底 runtime_error + 反馈同构
// ---------------------------------------------------------------------------

TEST_F(ValuedCpuGpuTaskTest, TimingSinkFailureDeliversRuntimeErrorAndFeedback) {
    RegisterFake(/*supports_timing=*/true);
    SetScheduler(/*feedback=*/true);

    auto report = executor_.submit_auto(
        cpu_gpu_task<int>([]() -> int { return 1; }, [](void*) {})
            .result_adapter(ResultAdapter<int>{[]() -> int { return 0; }})
            .name("valued-sink-fail"));

    ASSERT_TRUE(fake_->has_pending());
    EXPECT_FALSE(report.result.is_ready());
    // 执行器侧失败（未运行即交付失败记录，completion_ns == 0 = 停止排空）。
    fake_->deliver_timing(failed_record_zero_completion());

    ASSERT_EQ(report.result.wait_for(std::chrono::seconds(2)),
              std::future_status::ready);
    try {
        (void)report.result.get();
        FAIL() << "expected std::runtime_error";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("work not completed"),
                  std::string::npos)
            << error.what();
    }

    // feedback_enabled 时兜底通道同构构建 SchedulingFeedback：恰一条、
    // success == false、GpuFailure。
    const auto feedbacks = scheduler_->snapshot();
    ASSERT_EQ(feedbacks.size(), 1u);
    EXPECT_FALSE(feedbacks[0].success);
    EXPECT_EQ(feedbacks[0].failure_kind, FailureKind::GpuFailure);
    EXPECT_EQ(feedbacks[0].backend, ExecutionBackend::Gpu);
    EXPECT_EQ(feedbacks[0].executor_name, kValuedFakeGpuName);
    EXPECT_EQ(feedbacks[0].task_id, "valued-sink-fail");
    EXPECT_EQ(feedbacks[0].execution_duration_ns, 0)
        << "failed delivery must report unknown duration";
}

// ---------------------------------------------------------------------------
// C4. 带 R 形态的 builder 链
// ---------------------------------------------------------------------------

TEST_F(ValuedCpuGpuTaskTest, BuilderChainUsableWithNonVoidR) {
    // 不注册 GPU 执行器：prefer_gpu/大特征在"无可用 GPU + AllowCpu"下
    // 必须回退 CPU，验证带 R 形态的完整 builder 链（name/qos/priority/
    // fallback/prefer_gpu/data_size/compute_intensity/gpu_config/
    // result_adapter）编译可用且行为正确。
    gpu::GpuTaskConfig custom_config;
    custom_config.stream_id = 0;

    auto report = executor_.submit_auto(
        cpu_gpu_task<int>([]() -> int { return 31 + 11; }, [](void*) {})
            .name("valued-builder-chain")
            .qos(QosClass::Interactive)
            .priority(TaskPriority::HIGH)
            .fallback(FallbackPolicy::AllowCpu)
            .prefer_gpu(true)
            .data_size(2 * 1024 * 1024)
            .compute_intensity(4.0f)
            .gpu_config(custom_config)
            .result_adapter(ResultAdapter<int>{[]() -> int { return -1; }}));

    EXPECT_EQ(report.result.get(), 42);
    EXPECT_EQ(report.decision.task_name, "valued-builder-chain");
    EXPECT_EQ(report.decision.selected_backend, ExecutionBackend::DefaultAsync);
    EXPECT_EQ(report.decision.status, RoutingStatus::AcceptedDegraded);
    EXPECT_TRUE(report.decision.fell_back)
        << "prefer_gpu characteristics must not override AllowCpu fallback "
           "when no usable GPU executor is registered";
}

TEST_F(ValuedCpuGpuTaskTest, BuilderChainRoutesToGpuWithAdapter) {
    RegisterFake(/*supports_timing=*/true);
    SetScheduler(/*feedback=*/false);

    auto report = executor_.submit_auto(
        cpu_gpu_task<long>(
            []() -> long { return 0; },
            [](void* stream) { EXPECT_EQ(stream, nullptr); })
            .name("valued-chain-gpu")
            .prefer_gpu(true)
            .data_size(4 * 1024 * 1024)
            .compute_intensity(8.0f)
            .result_adapter(ResultAdapter<long>{[]() -> long { return 4096; }}));

    ASSERT_TRUE(fake_->has_pending());
    fake_->run_pending_kernel();
    EXPECT_EQ(report.result.get(), 4096);
    EXPECT_EQ(report.decision.selected_backend, ExecutionBackend::Gpu);
    EXPECT_EQ(report.decision.selected_executor_name, kValuedFakeGpuName);
}
