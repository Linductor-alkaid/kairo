// P-008: Windows 处理器组上大于 64 CPU 的亲和性。
// 通过 ProcessorGroupApi 接缝注入替身，模拟两个处理器组（组 0 共 64 个
// CPU、组 1 共 8 个 CPU，即逻辑编号 64..71），验证：
//  - CPU 63 / 64 分别映射到组 0 末位与组 1 首位（旧实现直接拒绝 >= 64）；
//  - 指向未激活 CPU（如 72）与跨组列表（如 {0, 64}）被整体拒绝；
//  - 单组机器（<= 64 CPU）保持既有编号语义；
//  - get_current_thread_affinity() 按 group * 64 + 序号 还原编号；
//  - RealtimeThreadExecutor 显式 affinity 的应用状态如实上报。
#include "kairo/util/thread_utils.hpp"

#include <gtest/gtest.h>

#ifndef _WIN32

TEST(ThreadUtilsProcessorGroup, DisabledOnNonWindows) {
    GTEST_SKIP() << "处理器组亲和性仅在 Windows 上编译";
}

#else  // _WIN32

#include <windows.h>

#include <chrono>
#include <thread>
#include <utility>
#include <vector>

#include "kairo/config.hpp"
#include "kairo/realtime_thread_executor.hpp"

using kairo::util::ProcessorGroupAffinity;
using kairo::util::ProcessorGroupApi;
using kairo::util::get_current_thread_affinity;
using kairo::util::set_cpu_affinity;
using kairo::util::set_processor_group_api_for_test;

namespace {

// 替身：模拟任意组拓扑，并记录 set_thread_group_affinity 收到的请求。
class FakeProcessorGroupApi : public ProcessorGroupApi {
public:
    explicit FakeProcessorGroupApi(std::vector<unsigned long> cpus_per_group)
        : cpus_per_group_(std::move(cpus_per_group)) {}

    unsigned short get_active_processor_group_count() override {
        return static_cast<unsigned short>(cpus_per_group_.size());
    }

    unsigned long get_active_processor_count(unsigned short group) override {
        if (group >= cpus_per_group_.size()) {
            return 0;
        }
        return cpus_per_group_[group];
    }

    int get_thread_group_affinity(void* /*thread*/, ProcessorGroupAffinity* out) override {
        *out = current_;
        return 1;
    }

    int set_thread_group_affinity(void* /*thread*/,
                                  const ProcessorGroupAffinity* affinity) override {
        last_set_ = *affinity;
        current_ = *affinity;
        ++set_calls_;
        return 1;
    }

    int set_calls() const { return set_calls_; }
    const ProcessorGroupAffinity& last_set() const { return last_set_; }

private:
    std::vector<unsigned long> cpus_per_group_;
    ProcessorGroupAffinity current_{};
    ProcessorGroupAffinity last_set_{};
    int set_calls_ = 0;
};

class ApiRestoreGuard {
public:
    explicit ApiRestoreGuard(ProcessorGroupApi* api) {
        set_processor_group_api_for_test(api);
    }
    ~ApiRestoreGuard() { set_processor_group_api_for_test(nullptr); }
};

// 替身不读句柄值，只要求类型是 set_cpu_affinity 的形参类型。MSVC 与 MinGW
// win32 线程模型下 native_handle_type 就是 void*，直接用 GetCurrentThread()
// 伪句柄；MinGW posix 线程模型（winpthreads）下它是 pthread_t（整数），不能
// 从伪 HANDLE 转换——改用 pthread_self()，其值也是真实合法句柄。
std::thread::native_handle_type test_thread_handle() {
#if defined(__MINGW32__) && defined(__WINPTHREADS_VERSION_MAJOR)
    return pthread_self();
#else
    return static_cast<std::thread::native_handle_type>(GetCurrentThread());
#endif
}

TEST(ThreadUtilsProcessorGroup, MapsCpu63ToGroup0LastBit) {
    // 两组：组 0 满配 64 CPU，组 1 有 8 CPU。
    FakeProcessorGroupApi api({64, 8});
    ApiRestoreGuard guard(&api);

    EXPECT_TRUE(set_cpu_affinity(test_thread_handle(), {63}));
    ASSERT_EQ(api.set_calls(), 1);
    EXPECT_EQ(api.last_set().group, 0);
    EXPECT_EQ(api.last_set().mask, 1ULL << 63);
}

TEST(ThreadUtilsProcessorGroup, MapsCpu64ToGroup1FirstBit) {
    FakeProcessorGroupApi api({64, 8});
    ApiRestoreGuard guard(&api);

    // 旧实现把 Windows CPU 编号限制在 0-63；组感知路径下 64 是组 1 的首 CPU。
    EXPECT_TRUE(set_cpu_affinity(test_thread_handle(), {64}));
    ASSERT_EQ(api.set_calls(), 1);
    EXPECT_EQ(api.last_set().group, 1);
    EXPECT_EQ(api.last_set().mask, 1ULL << 0);
}

TEST(ThreadUtilsProcessorGroup, SameGroupListIsOneMask) {
    FakeProcessorGroupApi api({64, 8});
    ApiRestoreGuard guard(&api);

    EXPECT_TRUE(set_cpu_affinity(test_thread_handle(), {64, 71}));
    ASSERT_EQ(api.set_calls(), 1);
    EXPECT_EQ(api.last_set().group, 1);
    EXPECT_EQ(api.last_set().mask, (1ULL << 0) | (1ULL << 7));
}

TEST(ThreadUtilsProcessorGroup, RejectsCpuBeyondActiveCount) {
    FakeProcessorGroupApi api({64, 8});
    ApiRestoreGuard guard(&api);

    // 组 1 只有 8 个 CPU（64..71），72 不存在。
    EXPECT_FALSE(set_cpu_affinity(test_thread_handle(), {72}));
    EXPECT_EQ(api.set_calls(), 0);
}

TEST(ThreadUtilsProcessorGroup, RejectsCrossGroupRequest) {
    FakeProcessorGroupApi api({64, 8});
    ApiRestoreGuard guard(&api);

    // 单线程亲和性只能表达一个 (group, mask)，跨组列表整体拒绝且不发起设置。
    EXPECT_FALSE(set_cpu_affinity(test_thread_handle(), {0, 64}));
    EXPECT_EQ(api.set_calls(), 0);
}

TEST(ThreadUtilsProcessorGroup, RejectsInvalidIds) {
    FakeProcessorGroupApi api({64, 8});
    ApiRestoreGuard guard(&api);

    EXPECT_FALSE(set_cpu_affinity(test_thread_handle(), {}));
    EXPECT_FALSE(set_cpu_affinity(test_thread_handle(), {-1}));
    EXPECT_EQ(api.set_calls(), 0);
}

TEST(ThreadUtilsProcessorGroup, SingleGroupKeepsLegacyNumbering) {
    // 单组机器（<= 64 CPU）兼容行为：编号 0..count-1 有效。
    FakeProcessorGroupApi api({4});
    ApiRestoreGuard guard(&api);

    EXPECT_TRUE(set_cpu_affinity(test_thread_handle(), {0, 3}));
    EXPECT_EQ(api.last_set().group, 0);
    EXPECT_EQ(api.last_set().mask, (1ULL << 0) | (1ULL << 3));

    // 单组下 63 未激活、64 超出组数，均拒绝。
    EXPECT_FALSE(set_cpu_affinity(test_thread_handle(), {63}));
    EXPECT_FALSE(set_cpu_affinity(test_thread_handle(), {64}));
}

TEST(ThreadUtilsProcessorGroup, GetAffinityReportsGroupBasedIds) {
    FakeProcessorGroupApi api({64, 8});
    ApiRestoreGuard guard(&api);

    // 组 1 内的 CPU 70：读回应为组基址 64 + 组内序号 6 = 70。
    EXPECT_TRUE(set_cpu_affinity(test_thread_handle(), {70}));
    const std::vector<int> ids = get_current_thread_affinity();
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids[0], 70);
}

// 真实 API 路径：显式 affinity 的应用状态如实上报（组 0 的 CPU 0 在任何
// Windows runner 上都存在；不依赖具体核数）。affinity 在 worker 线程上
// 异步应用，start() 返回不代表已生效，需要有界轮询等待状态可见。
TEST(ThreadUtilsProcessorGroup, RealtimeExplicitAffinityStatusReported) {
    ApiRestoreGuard guard(nullptr);  // 确保使用真实 Win32 API

    kairo::RealtimeThreadConfig config;
    config.thread_name = "p008_affinity";
    config.cycle_period_ns = 20'000'000;
    config.timer_slack_ns = 0;
    config.cycle_callback = [] {};
    config.cpu_affinity = {0};

    kairo::RealtimeThreadExecutor executor("p008_rt", config);
    ASSERT_TRUE(executor.start());

    bool applied = false;
    for (int i = 0; i < 3000 && !applied; ++i) {
        applied = executor.get_status().cpu_affinity_applied;
        if (!applied) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    executor.stop_and_join();

    EXPECT_TRUE(applied)
        << "CPU 0 (group 0) exists on every Windows host; explicit affinity must apply";
}

}  // namespace

#endif  // _WIN32
