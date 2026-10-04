// CR-010: submit_on_with_handle 异常路径泄漏 registry 槽位
//（executor.hpp:1329-1335：bound 构造抛异常时只 abandon ticket + rethrow，
//  已在 1256 行注册进 TaskCancellationRegistry 的 state 永不 finalize）
//
// 方法：拷贝构造抛异常的参数类型让 std::bind 的 decay-copy 在注册之后抛出，
// 循环失败提交直到 registry（容量 65536）被泄漏槽位填满，再验证：
//  (a) 正常可取消提交/取消是否仍可用；
//  (b) get_cancellation_status() / get_snapshot() 计数表现；
//  (c) 新提交是否收到 "Cancellation registry capacity exhausted"。
#include <kairo/executor.hpp>
#include <kairo/serial_execution_context.hpp>

#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>

using namespace kairo;
using namespace std::chrono_literals;

namespace {

struct ThrowOnCopy {
    ThrowOnCopy() = default;
    ThrowOnCopy(const ThrowOnCopy&) { throw std::runtime_error("copy ctor throws"); }
    ThrowOnCopy(ThrowOnCopy&&) = default;
};

// 提交后检查 future 是否携带 registry 容量耗尽异常
std::string settle_error(std::future<int>& f) {
    try {
        (void)f.get();
        return "";
    } catch (const std::exception& e) {
        return e.what();
    }
}

}  // namespace

int main() {
    Executor ex;
    if (!ex.initialize(ExecutorConfig{})) {
        std::printf("initialize failed\n");
        return 1;
    }
    SerialExecutionContext ctx;

    // ---- 基线：正常 serial 提交 + 取消可用 ----
    auto good = ex.submit_on_with_handle(ctx, [] { return 1; });
    std::string good_err = settle_error(good.future);
    auto cancellable = ex.submit_with_handle([] {
        std::this_thread::sleep_for(50ms);
        return 2;
    });
    auto cancel_resp = ex.request_task_cancel(cancellable.handle);
    std::printf("BASELINE: good_submit_err='%s' cancel_result=%d\n",
                good_err.c_str(), static_cast<int>(cancel_resp.result));
    (void)cancellable.future.wait_for(5s);

    auto snap0 = ex.get_snapshot();
    std::printf("BASELINE: cancellation.request_count=%llu completed_after=%llu "
                "in_flight=%zu\n",
                static_cast<unsigned long long>(snap0.cancellation.request_count),
                static_cast<unsigned long long>(
                    snap0.cancellation.completed_after_request_count),
                snap0.in_flight_count);

    // ---- 失败提交循环：bind 的 decay-copy 在 registry 注册之后抛出 ----
    ThrowOnCopy bad_arg;
    const int kTotal = 70000;
    int threw_copy = 0;         // bind 构造抛出（泄漏路径）
    int rejected_future = 0;    // 提交返回但 future 携带 registry 耗尽异常
    int other = 0;
    std::string last_reject_msg;
    for (int i = 0; i < kTotal; ++i) {
        try {
            auto sub = ex.submit_on_with_handle(ctx, [](ThrowOnCopy) { return 0; },
                                                bad_arg);
            std::string err = settle_error(sub.future);
            if (err.find("Cancellation registry capacity exhausted") !=
                std::string::npos) {
                ++rejected_future;
                last_reject_msg = err;
            } else {
                ++other;
            }
        } catch (const std::exception& e) {
            if (std::string(e.what()) == "copy ctor throws") {
                ++threw_copy;
            } else {
                ++other;
                last_reject_msg = e.what();
            }
        }
    }
    std::printf("LOOP: total=%d threw_at_bind=%d rejected_after_registry_full=%d "
                "other=%d\n",
                kTotal, threw_copy, rejected_future, other);
    std::printf("LOOP: last_reject_msg='%s'\n", last_reject_msg.c_str());

    // ---- 泄漏后验证 ----
    // (a) 新的正常 serial 提交是否被 registry 耗尽拒绝
    auto after = ex.submit_on_with_handle(ctx, [] { return 3; });
    std::string after_err = settle_error(after.future);
    std::printf("AFTER: normal_serial_submit_err='%s'\n", after_err.c_str());

    // (b) 正常可取消提交 + 取消是否仍生效（registry 同一实例）
    auto cancellable2 = ex.submit_with_handle([] {
        std::this_thread::sleep_for(50ms);
        return 4;
    });
    auto cancel_resp2 = ex.request_task_cancel(cancellable2.handle);
    std::printf("AFTER: cancel_result=%d (1=RequestedBeforeStart 5=NotFound "
                "4=AlreadyShutdown)\n",
                static_cast<int>(cancel_resp2.result));
    std::string c2_err;
    try {
        (void)cancellable2.future.get();
    } catch (const std::exception& e) {
        c2_err = e.what();
    }
    std::printf("AFTER: cancellable2_future_err='%s'\n", c2_err.c_str());

    // (c) 计数与趋势：取消计数不反映泄漏（槽位静默占用），靠 (a)/(c) 判定
    auto snap1 = ex.get_snapshot();
    std::printf("AFTER: cancellation.request_count=%llu in_flight=%zu\n",
                static_cast<unsigned long long>(snap1.cancellation.request_count),
                snap1.in_flight_count);

    bool registry_exhausted =
        after_err.find("Cancellation registry capacity exhausted") !=
        std::string::npos;
    if (threw_copy > 0 && registry_exhausted) {
        std::printf("CR-010 VERDICT: CONFIRMED — %d failed submits leaked registry "
                    "slots; subsequent valid serial submit rejected with registry "
                    "exhaustion\n",
                    threw_copy);
        return 0;
    }
    std::printf("CR-010 VERDICT: NOT REPRODUCED (threw=%d, after_err='%s')\n",
                threw_copy, after_err.c_str());
    (void)ex.shutdown(true);
    return 1;
}
