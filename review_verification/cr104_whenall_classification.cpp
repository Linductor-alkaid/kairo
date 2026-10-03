// CR-104: WhenAll 依赖失败异常分类不一致
//（executor.cpp:608-611：parked 依赖路径在 562 行 reclassify 为
//  DependencyCancelled，WhenAll 分支把原异常原样存入节点）
//
// 思路：上游任务被取消（TaskCancelled(Explicit)），比较下列路径下用户
// future 收到的异常类型：
//   A  submit_after(上游)             —— parked 依赖路径（有 reclassify）
//   B  submit_after(when_all(U))，dependent 在上游失败前提交 —— 级联 reclassify
//   B2 submit_after(when_all(U))，dependent 在上游失败后提交 —— 提交期 reclassify
//   C  when_all(已失败的上游) + submit_after —— when_all 创建期存原异常
//   E  submit_after(when_all(when_all(U))) —— 两级 WhenAll
//   D  上游抛 runtime_error 的对照（两路径均应透传 runtime_error）
// 若所有路径最终异常类型一致 → 分类差异在 API 层面不可观测 → NOT REPRODUCED。
#include <kairo/executor.hpp>

#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace kairo;
using namespace std::chrono_literals;

namespace {

ExecutorConfig one_thread_config() {
    ExecutorConfig config;
    config.min_threads = 1;
    config.max_threads = 1;
    return config;
}

// 占住唯一 worker，直到 release；保证 setup 期间被测任务保持排队/parked。
class OccupiedPool {
public:
    explicit OccupiedPool(Executor& executor)
        : gate_(std::make_shared<std::promise<void>>())
        , entered_(std::make_shared<std::promise<void>>()) {
        occupied_ = executor.submit([gate = gate_, entered = entered_] {
            entered->set_value();
            gate->get_future().wait();
        });
        (void)entered_->get_future().wait();
    }
    void release() {
        if (!released_) {
            released_ = true;
            gate_->set_value();
        }
    }
    ~OccupiedPool() { release(); }

private:
    std::shared_ptr<std::promise<void>> gate_;
    std::shared_ptr<std::promise<void>> entered_;
    std::future<void> occupied_;
    bool released_ = false;
};

std::string describe(std::future<int>& f) {
    if (f.wait_for(5s) != std::future_status::ready) {
        return "TIMEOUT(5s)";
    }
    try {
        int v = f.get();
        return "VALUE(" + std::to_string(v) + ")";
    } catch (const TaskCancelled& e) {
        return std::string("TaskCancelled(reason=") +
               std::to_string(static_cast<int>(e.reason())) + ")";
    } catch (const std::exception& e) {
        return std::string("std::exception(") + typeid(e).name() + ": " +
               e.what() + ")";
    } catch (...) {
        return "unknown";
    }
}

}  // namespace

int main() {
    Executor ex;
    if (!ex.initialize(one_thread_config())) {
        std::printf("initialize failed\n");
        return 1;
    }

    // ---- A: 直接 parked 依赖路径 ----
    std::string a_desc;
    {
        OccupiedPool occupied(ex);
        auto upstream = ex.submit_with_handle([] { return 1; });
        auto dependent =
            ex.submit_after_with_handle(upstream.handle, [] { return 2; });
        auto resp = ex.request_task_cancel(upstream.handle);
        occupied.release();
        (void)upstream.future.wait_for(5s);
        a_desc = describe(dependent.future);
        std::printf("A  submit_after(U) then cancel U        : %s (cancel=%d)\n",
                    a_desc.c_str(), static_cast<int>(resp.result));
    }

    // ---- B: when_all 在上游失败前创建，dependent 也提前提交（parked on W）----
    std::string b_desc;
    {
        OccupiedPool occupied(ex);
        auto upstream = ex.submit_with_handle([] { return 1; });
        TaskHandle combined = ex.when_all({upstream.handle});
        auto dependent = ex.submit_after_with_handle(combined, [] { return 3; });
        auto resp = ex.request_task_cancel(upstream.handle);
        occupied.release();
        b_desc = describe(dependent.future);
        std::printf("B  after(when_all(U)) parked            : %s (cancel=%d)\n",
                    b_desc.c_str(), static_cast<int>(resp.result));
    }

    // ---- B2: when_all 提前创建，dependent 在上游已失败后提交 ----
    std::string b2_desc;
    {
        OccupiedPool occupied(ex);
        auto upstream = ex.submit_with_handle([] { return 1; });
        TaskHandle combined = ex.when_all({upstream.handle});
        auto resp = ex.request_task_cancel(upstream.handle);
        occupied.release();
        (void)upstream.future.wait_for(5s);
        auto dependent = ex.submit_after_with_handle(combined, [] { return 3; });
        b2_desc = describe(dependent.future);
        std::printf("B2 after(when_all(U)) submit-after-fail : %s (cancel=%d)\n",
                    b2_desc.c_str(), static_cast<int>(resp.result));
    }

    // ---- C: when_all 在上游已失败后创建（创建期即把原异常存入节点）----
    std::string c_desc;
    {
        OccupiedPool occupied(ex);
        auto upstream = ex.submit_with_handle([] { return 1; });
        auto resp = ex.request_task_cancel(upstream.handle);
        occupied.release();
        (void)upstream.future.wait_for(5s);
        TaskHandle combined = ex.when_all({upstream.handle});
        auto dependent = ex.submit_after_with_handle(combined, [] { return 3; });
        c_desc = describe(dependent.future);
        std::printf("C  when_all(failed U) then after        : %s (cancel=%d)\n",
                    c_desc.c_str(), static_cast<int>(resp.result));
    }

    // ---- E: 两级 when_all ----
    std::string e_desc;
    {
        OccupiedPool occupied(ex);
        auto upstream = ex.submit_with_handle([] { return 1; });
        TaskHandle w1 = ex.when_all({upstream.handle});
        TaskHandle w2 = ex.when_all({w1});
        auto dependent = ex.submit_after_with_handle(w2, [] { return 3; });
        auto resp = ex.request_task_cancel(upstream.handle);
        occupied.release();
        e_desc = describe(dependent.future);
        std::printf("E  after(when_all(when_all(U)))         : %s (cancel=%d)\n",
                    e_desc.c_str(), static_cast<int>(resp.result));
    }

    // ---- D: 对照——上游抛普通异常，when_all 路径应透传 runtime_error ----
    std::string d_desc;
    {
        OccupiedPool occupied(ex);
        auto upstream = ex.submit_with_handle(
            []() -> int { throw std::runtime_error("boom"); });
        TaskHandle combined = ex.when_all({upstream.handle});
        auto dependent = ex.submit_after_with_handle(combined, [] { return 3; });
        occupied.release();
        d_desc = describe(dependent.future);
        std::printf("D  after(when_all(U throws))            : %s\n",
                    d_desc.c_str());
    }

    // ---- 判定：取消传播各路径的异常类型是否一致 ----
    const std::string expected =
        "TaskCancelled(reason=" +
        std::to_string(static_cast<int>(TaskCancellationReason::DependencyCancelled)) +
        ")";
    bool a_ok = a_desc == expected;
    bool b_ok = b_desc == expected;
    bool b2_ok = b2_desc == expected;
    bool c_ok = c_desc == expected;
    bool e_ok = e_desc == expected;
    bool d_ok = d_desc.rfind("std::exception", 0) == 0 &&
                d_desc.find("boom") != std::string::npos;

    std::printf("EXPECTED for A/B/B2/C/E: %s\n", expected.c_str());
    if (a_ok && b_ok && b2_ok && c_ok && e_ok && d_ok) {
        std::printf(
            "CR-104 VERDICT: NOT REPRODUCED — 所有用户可见路径异常分类一致"
            "（WhenAll 节点内部存原异常，但结算给用户前均被 reclassify）\n");
        (void)ex.shutdown(true);
        return 1;
    }
    std::printf(
        "CR-104 VERDICT: CONFIRMED — 存在路径分类不一致 (A=%d B=%d B2=%d "
        "C=%d E=%d D=%d)\n",
        a_ok, b_ok, b2_ok, c_ok, e_ok, d_ok);
    (void)ex.shutdown(true);
    return 0;
}
