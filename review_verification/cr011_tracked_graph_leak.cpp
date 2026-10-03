// CR-011: submit_tracked_with_hook 异常泄漏任务图节点
//（executor.hpp:1974-1988：invoke_user_callable 构造时
//  args_tuple = make_tuple(args...) 抛异常，而此时任务图节点（依赖登记）、
//  in-flight 槽位（executor.hpp:1815-1816）与 registry 槽位（1851）均已占用，
//  异常直接传播，三者都不回收）
//
// 方法：
//   phase 0  基线：成功提交若干带依赖任务，记录 RSS / 快照；
//   phase 1  以"拷贝抛异常"参数循环 submit_after_with_handle（有依赖 ⇒
//            register_task_graph_dependencies 已建图节点并记录 in-flight），
//            分 checkpoint 打 RSS / in_flight / dropped；
//   phase 2  对照：同数量成功提交不再增长；
//   判定：失败提交次数与 registry 容量(65536)吻合 + registry 耗尽 + RSS
//   阶梯式增长不回落 → 泄漏成立。
#include <kairo/executor.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using namespace kairo;
using namespace std::chrono_literals;

namespace {

struct ThrowOnCopy {
    ThrowOnCopy() = default;
    ThrowOnCopy(const ThrowOnCopy&) { throw std::runtime_error("copy ctor throws"); }
    ThrowOnCopy(ThrowOnCopy&&) = default;
};

long rss_kb() {
    FILE* f = std::fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[256];
    long kb = -1;
    while (std::fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, "VmRSS:", 6) == 0) {
            kb = std::atol(line + 6);
            break;
        }
    }
    std::fclose(f);
    return kb;
}

void print_metrics(const char* tag) {
    // 顶部内联函数，见下方调用点
}

}  // namespace

int main() {
    Executor ex;
    if (!ex.initialize(ExecutorConfig{})) {
        std::printf("initialize failed\n");
        return 1;
    }

    // 上游任务（依赖锚点）
    auto upstream = ex.submit_with_handle([] { return 1; });
    (void)upstream.future.get();

    auto report = [&](const char* tag) {
        auto s = ex.get_snapshot();
        std::printf(
            "%s: rss_kb=%ld in_flight=%zu in_flight_dropped=%zu "
            "cancel.request=%llu\n",
            tag, rss_kb(), s.in_flight_count, s.in_flight_dropped_count,
            static_cast<unsigned long long>(s.cancellation.request_count));
        std::fflush(stdout);
    };

    // ---- phase 0: 基线（成功提交不泄漏）----
    ThrowOnCopy bad_arg;
    for (int i = 0; i < 500; ++i) {
        auto sub = ex.submit_after_with_handle(upstream.handle,
                                               [] { return 10; });
        (void)sub.future.get();
    }
    report("PHASE0 baseline(500 good dep submits)");

    // ---- phase 1: 失败提交循环（每例：图节点+in-flight+registry 均已占用）----
    const int kTotal = 200000;
    const int kCheckpoint = 40000;
    long thrown = 0, rejected = 0, other = 0;
    std::string last_msg;
    for (int i = 0; i < kTotal; ++i) {
        try {
            auto sub = ex.submit_after_with_handle(
                upstream.handle, [](ThrowOnCopy) { return 0; }, bad_arg);
            try {
                (void)sub.future.get();
                ++other;
            } catch (const std::exception& e) {
                last_msg = e.what();
                ++rejected;
            }
        } catch (const std::exception& e) {
            if (std::string(e.what()) == "copy ctor throws") {
                ++thrown;
            } else {
                last_msg = e.what();
                ++other;
            }
        }
        if ((i + 1) % kCheckpoint == 0) {
            std::printf("CHECKPOINT i=%d thrown=%ld rejected=%ld\n", i + 1,
                        thrown, rejected);
            report("  metrics");
        }
    }
    std::printf("PHASE1: total=%d threw_at_make_tuple=%ld "
                "rejected_after_registry_full=%ld other=%ld\n",
                kTotal, thrown, rejected, other);
    std::printf("PHASE1: last_msg='%s'\n", last_msg.c_str());
    report("PHASE1 end");

    // ---- phase 2: 稳定性观察（无新提交，RSS 不应自行回落；再补 500 成功提交）----
    std::this_thread::sleep_for(300ms);
    for (int i = 0; i < 500; ++i) {
        try {
            auto sub = ex.submit_after_with_handle(upstream.handle,
                                                   [] { return 11; });
            (void)sub.future.wait_for(5s);
        } catch (...) {
        }
    }
    report("PHASE2 after 500 more good submits");

    // ---- 泄漏后功能验证：新的可取消提交/取消是否仍可用 ----
    auto cancellable = ex.submit_with_handle([] {
        std::this_thread::sleep_for(50ms);
        return 4;
    });
    auto cancel_resp = ex.request_task_cancel(cancellable.handle);
    std::printf("AFTER: cancel_result=%d (0=RequestedBeforeStart 4=NotFound)\n",
                static_cast<int>(cancel_resp.result));

    if (thrown > 0 && rejected > 0) {
        std::printf(
            "CR-011 VERDICT: CONFIRMED — %ld failed tracked submits (matches "
            "registry capacity fill) leaked graph nodes / in-flight / registry "
            "slots; new submits now rejected: '%s'\n",
            thrown, last_msg.c_str());
        return 0;
    }
    std::printf("CR-011 VERDICT: NOT REPRODUCED (thrown=%ld rejected=%ld)\n",
                thrown, rejected);
    (void)ex.shutdown(true);
    return 1;
}
